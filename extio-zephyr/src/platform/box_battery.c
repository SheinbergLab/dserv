/*
 * box_battery.c -- read the cell through the board's voltage divider.
 *
 * See box_battery.h for why this exists and why the read runs on a workqueue.
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <errno.h>

#include "box_battery.h"

LOG_MODULE_REGISTER(box_battery, LOG_LEVEL_INF);

/* VBUS lives on the SoC, not on the divider, so it is deliberately outside the
 * vbatt guard below: a board with no cell can still say whether it is on
 * external power. */
#if defined(CONFIG_SOC_FAMILY_NORDIC_NRF)
#include <hal/nrf_power.h>
#endif

int box_battery_vbus(void)
{
#if defined(CONFIG_SOC_FAMILY_NORDIC_NRF) && defined(NRF_POWER_HAS_USBREG) && NRF_POWER_HAS_USBREG
	return nrf_power_usbregstatus_vbusdet_get(NRF_POWER) ? 1 : 0;
#else
	return -ENOTSUP;
#endif
}

#define VBATT_NODE DT_NODELABEL(vbatt)

#if DT_NODE_EXISTS(VBATT_NODE) && defined(BOX_HAVE_ADC)

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/adc/voltage_divider.h>
#include <zephyr/drivers/gpio.h>

#include "box_ain.h"

static const struct voltage_divider_dt_spec vbatt =
	VOLTAGE_DIVIDER_DT_SPEC_GET(VBATT_NODE);

/* `power-gpios` is optional in the binding -- a board whose divider is wired
 * permanently across the cell simply omits it, and .port stays NULL. */
static const struct gpio_dt_spec vbatt_pwr =
	GPIO_DT_SPEC_GET_OR(VBATT_NODE, power_gpios, {0});

/* HOW LONG THE BORROW IS GUARANTEED FOR. It self-expires, so this is the window
 * in which a read that dies mid-sweep keeps the converter away from the sampler
 * -- not a duration we intend to use. The read itself is one 40 us conversion
 * plus the settle below. 250 ms is generous enough to survive a preemption and
 * short enough that a wedged read costs a quarter second of analog, once. */
#define BORROW_MS   250

/* Let the divider come up before converting. The node is high-impedance by
 * construction (~338 kOhm on the XIAO) and the pad has the SAADC's input
 * capacitance plus whatever the board puts there, so switching the enable pin
 * and converting in the same breath samples the RISE, not the cell -- low, and
 * low by an amount that depends on the cell's own voltage. 5 ms is many RC
 * constants and costs nothing at this cadence. */
#define SETTLE_MS   5

/* ONE MINUTE. A cell moves a few mV an hour; the cost of asking is a borrow that
 * stops the sampler, so asking often would perturb the analog path far more than
 * it would tell anyone about the battery. */
#define READ_EVERY_MS 60000

/* ---- ITS OWN WORKQUEUE, NOT THE SYSTEM ONE ----
 *
 * box_ain_borrow() waits up to AIN_BORROW_WAIT_MS -- 1500 -- for the sampler to
 * actually stop, and then this work sleeps SETTLE_MS on top. Putting that on the
 * system workqueue would hand a shared queue a stall of a second and a half:
 * nothing else in this firmware posts there today, but Zephyr's own subsystems
 * do, and "nobody else uses it yet" is not a property this file can promise for
 * the next person.
 *
 * PRIORITY BELOW EVERYTHING THAT MATTERS. The sampler is cooperative -2 and the
 * service loop is main; a battery reading must never preempt either, and it has
 * a whole minute of slack in which to be scheduled. 5 sits under the publisher
 * thread's 4 for the same reason.
 *
 * 1 KB: this is a leaf task -- one adc_read (ISR-driven underneath, the caller
 * just waits on a semaphore), two GPIO writes and a LOG call on the error path.
 * Nothing here recurses. */
#define BATT_STACK_SIZE 1024
#define BATT_PRIO       5

K_THREAD_STACK_DEFINE(batt_stack, BATT_STACK_SIZE);
static struct k_work_q batt_q;

static void batt_work_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(batt_work, batt_work_fn);

static uint8_t   ready;
static atomic_t  have;              /* a reading has completed since boot */
static atomic_t  last_mv, last_raw;
static atomic_t  n_reads, n_busy;
static uint32_t  next_ms;
static uint8_t   scheduled;

int box_battery_init(void)
{
	int rc;

	if (!adc_is_ready_dt(&vbatt.port)) {
		LOG_ERR("vbatt: adc %s not ready", vbatt.port.dev->name);
		return -ENODEV;
	}

	/* The channel node is `status = "disabled"` so that box_adc.c does not
	 * enumerate it as a field channel (see the overlay). Disabled keeps it out
	 * of DT_FOREACH_CHILD_STATUS_OKAY; it does NOT configure itself, so the
	 * setup box_adc.c does for its own channels has to happen here. */
	rc = adc_channel_setup_dt(&vbatt.port);
	if (rc) {
		LOG_ERR("vbatt: channel setup: %d", rc);
		return rc;
	}

	if (vbatt_pwr.port) {
		if (!gpio_is_ready_dt(&vbatt_pwr)) {
			LOG_ERR("vbatt: power-gpios not ready");
			return -ENODEV;
		}
		/* INACTIVE, not "low": the DT flags carry the polarity (ACTIVE_LOW on
		 * the XIAO), which is the single place it is stated. Leaving the
		 * divider switched off between reads is the whole reason the pin
		 * exists -- 4.2 V across 1.51 MOhm is ~2.8 uA, small against the
		 * radio but permanent, and it would be drawn flat by a shelved box
		 * that is otherwise off. */
		rc = gpio_pin_configure_dt(&vbatt_pwr, GPIO_OUTPUT_INACTIVE);
		if (rc) {
			LOG_ERR("vbatt: power-gpios configure: %d", rc);
			return rc;
		}
	}

	k_work_queue_start(&batt_q, batt_stack, K_THREAD_STACK_SIZEOF(batt_stack),
			   BATT_PRIO, NULL);
	k_thread_name_set(&batt_q.thread, "batt");

	ready = 1;
	return 0;
}

int box_battery_present(void) { return ready; }

/* The read. Runs on this module's OWN workqueue -- see the queue definition
 * above on why not the system one, and box_battery.h on why not the service
 * loop. */
static void batt_work_fn(struct k_work *w)
{
	int16_t  buf = 0;
	int32_t  v;
	struct adc_sequence seq = {
		.buffer      = &buf,
		.buffer_size = sizeof buf,
	};
	int rc;

	ARG_UNUSED(w);
	if (!ready) {
		return;
	}

	/* box_adc.h: the sampling thread owns the converter, and adc_read() on a
	 * suspended one blocks FOREVER with no error. -EBUSY here means the sampler
	 * did not stop in time, and the contract is that we must then not touch the
	 * device at all -- so count it and leave. A missing battery datapoint is
	 * recoverable; a sweep racing the sampler is not. */
	rc = box_ain_borrow(BORROW_MS);
	if (rc) {
		atomic_inc(&n_busy);
		return;
	}

	if (vbatt_pwr.port) {
		gpio_pin_set_dt(&vbatt_pwr, 1);
		k_msleep(SETTLE_MS);
	}

	rc = adc_sequence_init_dt(&vbatt.port, &seq);
	if (rc == 0) {
		rc = adc_read(vbatt.port.dev, &seq);
	}

	if (vbatt_pwr.port) {
		gpio_pin_set_dt(&vbatt_pwr, 0);
	}
	box_ain_return();

	if (rc) {
		LOG_WRN("vbatt: read: %d", rc);
		return;
	}

	/* SIGNED, and clamped at zero rather than trusted. The SAADC is a
	 * differential part internally and returns small negatives for a grounded
	 * input; letting one through into a uint32_t would publish ~4.29e9 mV. */
	v = buf;
	if (v < 0) {
		v = 0;
	}
	atomic_set(&last_raw, (atomic_val_t) v);

	rc = adc_raw_to_millivolts_dt(&vbatt.port, &v);
	if (rc) {
		LOG_WRN("vbatt: scale: %d", rc);
		return;
	}
	/* Undo the divider: v is the voltage at the PAD, the cell is
	 * v * full_ohms / output_ohms. -ENOTSUP only when full-ohms is absent,
	 * which for this node would be a board file that forgot half the divider. */
	rc = voltage_divider_scale_dt(&vbatt, &v);
	if (rc) {
		LOG_WRN("vbatt: divider: %d", rc);
		return;
	}

	atomic_set(&last_mv, (atomic_val_t) v);
	atomic_set(&have, 1);
	atomic_inc(&n_reads);
}

/* One conversion with the enable pin held in `on` (logical, so DT polarity
 * applies). Caller owns the borrow. Returns raw counts or negative errno. */
static int probe_once(int on)
{
	int16_t buf = 0;
	struct adc_sequence seq = { .buffer = &buf, .buffer_size = sizeof buf };
	int rc;

	if (vbatt_pwr.port) {
		gpio_pin_set_dt(&vbatt_pwr, on);
		k_msleep(SETTLE_MS);
	} else if (!on) {
		return -ENOTSUP;         /* nothing to switch: no "off" state exists */
	}

	rc = adc_sequence_init_dt(&vbatt.port, &seq);
	if (rc == 0) {
		rc = adc_read(vbatt.port.dev, &seq);
	}
	if (rc) {
		return rc;
	}
	return (buf < 0) ? 0 : (int) buf;
}

/* ---- THE BRING-UP PROBE ----
 *
 * Answers the ONE question a single reading cannot: is the divider actually
 * connected when we convert? A wrong ratio and a disconnected divider both
 * produce a plausible-looking number, and they need opposite fixes -- one is two
 * constants in the overlay, the other is the wrong pin or the wrong polarity.
 *
 * Reads with the enable ASSERTED and DEASSERTED. If those differ, the pin really
 * is gating something and the resistors are the remaining question. If they are
 * the same, the pin is doing nothing: either it is not the enable, or the DT
 * polarity is inverted, and the pad being converted is floating.
 *
 * Exists because on 2026-09-03 the first hardware reading put the pad at 637 mV
 * -- too low for a 3.7-4.2 V cell behind ANY sane divider -- and nothing in a
 * single sample could say which of those two it was. */
int box_battery_probe(uint16_t *on, uint16_t *off)
{
	int rc, r_on, r_off;

	if (!ready) {
		return -ENODEV;
	}
	rc = box_ain_borrow(BORROW_MS);
	if (rc) {
		return rc;
	}

	r_on  = probe_once(1);
	r_off = probe_once(0);

	if (vbatt_pwr.port) {
		gpio_pin_set_dt(&vbatt_pwr, 0);      /* leave it switched off */
	}
	box_ain_return();

	if (r_on < 0) {
		return r_on;
	}
	if (on) {
		*on = (uint16_t) r_on;
	}
	if (off) {
		*off = (r_off < 0) ? 0xFFFF : (uint16_t) r_off;
	}
	return 0;
}

void box_battery_service(uint32_t now_ms)
{
	if (!ready) {
		return;
	}
	/* First read shortly after boot rather than a minute into it: a box that
	 * comes up on a flat cell should say so before it dies, and somebody
	 * watching a fresh boot should not have to wait out the full period to see
	 * whether the divider works at all. */
	if (!scheduled) {
		scheduled = 1;
		next_ms   = now_ms + 2000;
	}
	if ((int32_t) (now_ms - next_ms) < 0) {
		return;
	}
	next_ms = now_ms + READ_EVERY_MS;
	k_work_schedule_for_queue(&batt_q, &batt_work, K_NO_WAIT);
}

int box_battery_get(uint32_t *mv, uint16_t *raw)
{
	if (!ready) {
		return -ENODEV;
	}
	if (!atomic_get(&have)) {
		return -EAGAIN;
	}
	if (mv) {
		*mv = (uint32_t) atomic_get(&last_mv);
	}
	if (raw) {
		*raw = (uint16_t) atomic_get(&last_raw);
	}
	return 0;
}

void box_battery_stats(uint32_t *reads, uint32_t *busy)
{
	if (reads) {
		*reads = (uint32_t) atomic_get(&n_reads);
	}
	if (busy) {
		*busy = (uint32_t) atomic_get(&n_busy);
	}
}

#else  /* no vbatt node, or no ADC in this build */

int  box_battery_init(void)                       { return -ENODEV; }
int  box_battery_present(void)                    { return 0; }
void box_battery_service(uint32_t now_ms)         { ARG_UNUSED(now_ms); }
int  box_battery_get(uint32_t *mv, uint16_t *raw)
{
	ARG_UNUSED(mv); ARG_UNUSED(raw);
	return -ENODEV;
}
int  box_battery_probe(uint16_t *on, uint16_t *off)
{
	ARG_UNUSED(on); ARG_UNUSED(off);
	return -ENODEV;
}
void box_battery_stats(uint32_t *reads, uint32_t *busy)
{
	if (reads) { *reads = 0; }
	if (busy)  { *busy  = 0; }
}

#endif

/* ---- the curve ----
 *
 * NOMINAL, at light load, and flagged as such in the header. A lithium cell
 * spends most of its discharge between 3.7 and 3.9 V, so the middle of this
 * table is the part that will be wrong; the knees at either end are the part
 * anyone reads. REPLACE IT once state/batt/mv has produced a real discharge log
 * -- that log is the reason this module was written.
 *
 * Deliberately outside the #if: a host asking what 3800 mV means gets the same
 * answer from any box, including one with no cell of its own. */
static const struct { uint16_t mv; uint8_t pct; } curve[] = {
	{ 4200, 100 }, { 4100, 92 }, { 4000, 84 }, { 3900, 74 },
	{ 3850,  66 }, { 3800, 58 }, { 3750, 48 }, { 3700, 40 },
	{ 3650,  30 }, { 3600, 22 }, { 3500, 12 }, { 3400,  5 },
	{ 3300,   0 },
};

uint8_t box_battery_pct(uint32_t mv)
{
	size_t i;

	if (mv >= curve[0].mv) {
		return 100;
	}
	for (i = 1; i < ARRAY_SIZE(curve); i++) {
		if (mv >= curve[i].mv) {
			/* Linear between the two bracketing points. Integer maths on
			 * purpose: this is a gauge, and a float here would drag the
			 * soft-float library into a build that has no other use for
			 * it. */
			uint32_t span = curve[i - 1].mv - curve[i].mv;
			uint32_t up   = mv - curve[i].mv;
			uint32_t dp   = curve[i - 1].pct - curve[i].pct;

			return (uint8_t) (curve[i].pct + (up * dp) / span);
		}
	}
	return 0;
}
