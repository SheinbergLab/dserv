/*
 * box_gpio.c -- RW612/Zephyr implementation of the box_gpio.h seam.
 * See the header for the design rationale (devicetree pins, k_timer pulse,
 * gpio_callback DI capture) and how it differs from pico_gpio.h.
 */
#include "box_gpio.h"
#include "box_event.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

/* ---- the pin map: box pin n -> ANY gpio, chosen per board ----
 *
 * Two schemes, in priority order.
 *
 * 1. PIN MAP (preferred). The board declares an ordered list of gpio specs in
 *    `zephyr,user/box-gpios`, so box pin n resolves to whatever the board says.
 *    This buys three things the old scheme could not:
 *      * pins on ANY port, not just one (the FRDM-MCXN947's Arduino header is
 *        spread across gpio0/gpio1/gpio4, and six of fourteen digital pins were
 *        simply unreachable before);
 *      * per-pin FLAGS from devicetree -- notably GPIO_ACTIVE_LOW, which is how
 *        the board's LEDs stop being inverted without inventing a config field;
 *      * box pin numbering CHOSEN PER BOARD, so it can match the silkscreen.
 *    `zephyr,user` is used rather than a custom compatible so no binding file is
 *    needed; `box-reserved` lists indices that must never be claimed.
 *
 * 2. LEGACY single-port alias `box-gpio-port`, giving box pin n -> port.n. Kept
 *    so teensy40/teensy41/frdm_rw612 build and behave EXACTLY as before -- they
 *    are not on the bench for this change, and silently renumbering their pins
 *    would be the worst possible way to find that out.
 *
 * The frozen wire contract is untouched either way: `config/pin/<n>` and
 * `cmd/do/<n>` stay a flat index, it just resolves somewhere sane.
 */
#define BOX_USER_NODE DT_PATH(zephyr_user)

#if DT_NODE_HAS_PROP(BOX_USER_NODE, box_gpios)
#define BOX_PINMAP_ENTRY(i, _) \
	GPIO_DT_SPEC_GET_BY_IDX_OR(BOX_USER_NODE, box_gpios, i, {0})
#elif DT_NODE_EXISTS(DT_ALIAS(box_gpio_port))
#define BOX_PINMAP_ENTRY(i, _) \
	{ .port = DEVICE_DT_GET(DT_ALIAS(box_gpio_port)), \
	  .pin = (gpio_pin_t) (i), .dt_flags = 0 }
#else
#error "board overlay must supply zephyr,user/box-gpios or the box-gpio-port alias"
#endif

static const struct gpio_dt_spec specs[BOX_NPINS] = {
	LISTIFY(BOX_NPINS, BOX_PINMAP_ENTRY, (,))
};

/* A pin is refused if the board does not map it at all, or lists it reserved.
 * On the MCXN947 that is Arduino D5 = P1_21 = ENET0_MDIO: it exists on the
 * header, it is the PHY's, and claiming it would take the box off the network
 * to no purpose. Declaring it in the map and refusing it here is deliberate --
 * the map then documents the whole header rather than quietly omitting a pin. */
static inline int box_gpio_reserved(int n)
{
	if (n < 0 || n >= BOX_NPINS || specs[n].port == NULL) {
		return 1;
	}
#if DT_NODE_HAS_PROP(BOX_USER_NODE, box_reserved)
	static const uint8_t rsv[] = DT_PROP(BOX_USER_NODE, box_reserved);

	for (size_t k = 0; k < ARRAY_SIZE(rsv); k++) {
		if (rsv[k] == (uint8_t) n) {
			return 1;
		}
	}
#endif
	return 0;
}

/* ---- the box clock: THE monotonic microsecond source for every event ----
 *
 * Everything the box timestamps comes through here -- DI edges (from the ISR),
 * the sync-pin latch, scheduled-event arming, and box_clock_stamp()'s box_us
 * input. If this returns a constant, every timestamp the box publishes is a
 * constant, and nothing else in the firmware notices.
 *
 * WHICH IS EXACTLY WHAT HAPPENED (2026-08-22, nRF52840 DK). This was
 * unconditionally k_cyc_to_us_floor64(k_cycle_get_64()), and k_cycle_get_64()
 * is only implemented when the timer driver selects
 * CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER. The mcux, systick and nRF GRTC drivers
 * do; the nRF52840's NRF_RTC_TIMER does NOT. So on that board this returned 0
 * forever: console `now` read `box 0.000000 s` on a box up for minutes, every
 * DI event carried the sync offset as its timestamp (a constant that LOOKS like
 * a real time -- worse than the honest 0 an unsynced box sends), and every pin
 * with debounce > 0 went permanently silent, because box_gpio_poll_di() routes
 * debounced pins to a settle window measured against this clock while
 * debounce-0 pins ride the FIFO and publish directly. Four buttons, two of
 * each, 90 s of pressing: 42 events from the FIFO pair, zero from the other.
 * Full autopsy in PORTING.md.
 *
 * KEYED ON THE CAPABILITY, NOT ON THE BOARD. A #if on CONFIG_BOARD_* would have
 * to be extended by hand for every future part, and the next one to lack the
 * counter would fail the same silent way -- which is the whole failure mode
 * this comment exists to prevent. The Kconfig IS the question being asked.
 *
 * The fallback's resolution is the tick rate, not the cycle rate:
 *
 *     nRF52840   32768 Hz  ->  30.5 us   (this path)
 *     XIAO 54LM20A / MCXN947 / Teensy    -> 64-bit cycle counter, unchanged
 *
 * 30.5 us is a DELIBERATE ACCEPTANCE for the Nordic tier, not an oversight:
 * those boxes are destined for BLE peripherals and in-cage trainers where
 * +/-50 us is fine, and buying microseconds means a dedicated nRF TIMER at
 * 1 MHz -- which pins HFCLK on and costs milliamps on a battery box, for
 * precision that tier does not need. A rig box that DOES need microseconds
 * should be on silicon that has the counter (every other board here). If a
 * Nordic box ever needs it too, add the TIMER behind this same function; do
 * not widen the fallback and hope.
 *
 * k_uptime_ticks() is 64-bit, monotonic, ISR-safe and always implemented, so
 * the fallback needs no wrap handling of its own. */
static inline uint64_t now_us(void)
{
#if defined(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER)
	return k_cyc_to_us_floor64(k_cycle_get_64());
#else
	return k_ticks_to_us_floor64(k_uptime_ticks());
#endif
}

/* ---- DI edge capture + debounce ---- */
static volatile uint64_t di_first_edge_us[BOX_NPINS];  /* press/release moment  */
static volatile uint64_t di_last_edge_us[BOX_NPINS];   /* moving quiet-since     */
static volatile uint8_t  di_unsettled[BOX_NPINS];
static uint8_t           di_pub_level[BOX_NPINS];      /* poller-only            */

/* A whole pulse can hide inside one poll gap: the ISR latches its first and
 * last edge, but if the pin is back at the published level by the time the
 * poller looks, the old code found lvl == di_pub_level and dropped the
 * episode -- both edges gone. Observed on the wire (extio_test, 2026-08-02):
 * 1 ms loopback pulses vanished whole whenever a USB TX stall delayed the
 * poll past the pulse width. The episode's timestamps were captured all
 * along, so reconstruct the pair instead: emit the away-level edge at
 * first_edge and queue the return edge at last_edge for the next poll call.
 * A multi-pulse train inside one gap still collapses to one pair (first/last
 * bound the episode) -- with the UDC pool fix shrinking stalls to sub-pulse
 * lengths that residue is out of reach of a real schedule. */
static uint8_t  di_synth_pending[BOX_NPINS];           /* poller-only */
static uint64_t di_synth_t[BOX_NPINS];
static uint8_t  di_synth_lvl[BOX_NPINS];

/* ---- the DI edge FIFO (+24): every edge survives any stall ----
 *
 * The latch above keeps only first/last stamps per pin, so a service stall
 * longer than a pulse train folded the train's middle edges into one
 * reconstructed pair -- measured on 2026-08-02 (a 10.7 ms loop_max_us
 * stall vs a 10 ms six-pulse train: 2 edges survived of 12; the +23 sched
 * ledger proved every pulse FIRED). For this box the record outranks the
 * deadline, so debounce-0 pins now ride a shared edge FIFO: the ISR
 * appends {pin, level, t_us} per edge, the poller drains in order, and a
 * stall merely delays delivery of exact records. Pins with debounce_ms > 0
 * stay on the latch path -- there, collapsing bounces is the intended
 * behavior, not a defect. Overflow drops NEWEST and counts honestly
 * (dbg/di_fifo_drop): 128 entries absorb a 10 ms stall at a physically
 * implausible 12 kHz edge rate. Two edges inside one IRQ latency still
 * merge at the pad (read-level race); the poller restores count parity by
 * emitting the pair at the same stamp -- honest to resolution. */
#define DI_FIFO_DEPTH 128
typedef struct { uint64_t t_us; uint8_t pin; uint8_t level; } di_edge_t;
static di_edge_t di_fifo[DI_FIFO_DEPTH];
static volatile uint16_t di_fifo_head, di_fifo_tail;   /* ISR writes head */
static volatile uint32_t di_fifo_drops;
/* `do` commands refused because the target pin is not an output. Non-zero means
 * a host is driving a pin the box's own config says is an input, an ain channel,
 * or off -- previously that silently succeeded by re-muxing the pad. */
static uint32_t do_refused;
static uint8_t  di_use_fifo[BOX_NPINS];                /* poller-maintained */
static uint8_t  di_pair_pending[BOX_NPINS];            /* count-parity slot  */
static uint64_t di_pair_t[BOX_NPINS];
static uint8_t  di_pair_lvl[BOX_NPINS];

/* ---- hardware obs-sync input: raw edge latch (clock anchor, unpublished) ---- */
static volatile int      sync_pin = -1;
static volatile uint64_t sync_edge[2];                 /* [0]=fall(end) [1]=rise(begin) */

/* gpio_callback is PER CONTROLLER, so a map spanning several ports needs one
 * registration each. Four covers any board here (the MCXN947 uses three:
 * gpio0/gpio1/gpio4); overflow is refused in add_input() rather than silently
 * dropping a pin's interrupts. */
#define BOX_MAX_PORTS 4
static const struct device *cb_dev[BOX_MAX_PORTS];
static struct gpio_callback cb_obj[BOX_MAX_PORTS];
static gpio_port_pins_t     cb_mask[BOX_MAX_PORTS];
static uint8_t              cb_n;
static volatile uint32_t    di_isr_n;      /* TEMP diagnostic: ISR entries */

/* single ISR for every configured input + the sync pin, on every port. `pins`
 * is a mask in the CONTROLLER's pin space, so a box pin matches only when both
 * its controller and its pin agree -- with one port that reduces to the old
 * BIT(i) test, and with several it is what keeps gpio1.2 from being mistaken
 * for gpio0.2. 64-bit fields are written here and read under irq_lock in the
 * poller (single-core CM33). */
static void di_isr(const struct device *dev, struct gpio_callback *cb,
		   gpio_port_pins_t pins)
{
	ARG_UNUSED(cb);
	uint64_t t = now_us();
	bool woke = false;

	di_isr_n++;                          /* TEMP diagnostic */

	for (int i = 0; i < BOX_NPINS; i++) {
		if (specs[i].port != dev || !(pins & BIT(specs[i].pin))) {
			continue;
		}
		if (i == sync_pin) {                         /* latch, do not report */
			int lvl = gpio_pin_get_dt(&specs[i]);
			sync_edge[lvl ? 1 : 0] = t;
			continue;
		}
		if (di_use_fifo[i]) {
			uint16_t h = di_fifo_head;
			uint16_t n = (uint16_t) ((h + 1) % DI_FIFO_DEPTH);

			if (n == di_fifo_tail) {
				di_fifo_drops++;         /* full: drop NEWEST, count */
			} else {
				di_fifo[h].t_us  = t;
				di_fifo[h].pin   = (uint8_t) i;
				di_fifo[h].level = (uint8_t) gpio_pin_get_dt(&specs[i]);
				di_fifo_head = n;
			}
			woke = true;
			continue;
		}
		if (!di_unsettled[i]) {
			di_first_edge_us[i] = t;
			di_unsettled[i] = 1;
		}
		di_last_edge_us[i] = t;
		woke = true;
	}
	if (woke) {
		box_event_signal();          /* wake the service loop now, don't wait for a poll */
	}
}

uint64_t box_gpio_sync_edge_us(int rising)
{
	unsigned int k = irq_lock();
	uint64_t t = sync_edge[rising ? 1 : 0];
	irq_unlock(k);
	return t;
}

/* ---- non-blocking pulse: a per-pin k_timer drops the edge ----
 * One timer per pin, so concurrent pulses on distinct pins never contend and
 * there is no channel pool to exhaust (and so no blocking fallback -- the old
 * k_busy_wait fallback stalled the service loop for the whole width, which made
 * the DI poller miss both edges of a looped-back pulse). Expiry runs in the
 * system-clock ISR; resolution is the 100 us kernel tick, accepted for this box.
 *
 * This replaces the hardware-counter (GPT/CTIMER alarm) scheme. That path was
 * broken by Zephyr's counter_mcux_gpt driver: set_alarm() enables the compare
 * interrupt WITHOUT clearing a stale compare status flag, and the GPT runs in
 * restart mode by default (counter resets at -- and re-crosses -- the old
 * compare value forever), so every alarm after the first fired immediately and
 * the "pulse" was a microsecond sliver. See PORTING.md for the full autopsy. */
static struct k_timer pulse_fall[BOX_NPINS];

static void pulse_fall_expired(struct k_timer *t)
{
	int pin = (int)(t - pulse_fall);
	gpio_pin_set_dt(&specs[pin], 0);
}

uint32_t box_gpio_reserved_mask(void)
{
	uint32_t m = 0;

	for (int i = 0; i < BOX_NPINS && i < 32; i++) {
		if (box_gpio_reserved(i)) {
			m |= (1u << i);
		}
	}
	return m;
}

int box_gpio_init(void)
{
	/* Every DISTINCT controller the map names has to be up. A map spanning
	 * three ports fails usefully here rather than at the first write to the one
	 * port that was not ready. */
	for (int i = 0; i < BOX_NPINS; i++) {
		if (specs[i].port && !device_is_ready(specs[i].port)) {
			return -1;
		}
	}
	for (int i = 0; i < BOX_NPINS; i++) {
		k_timer_init(&pulse_fall[i], pulse_fall_expired, NULL);
	}
	return 0;
}

/* Accumulate box pin `i` into its controller's interrupt mask, creating the
 * slot on first use. Returns 0 if the port table is full -- refused loudly at
 * the caller rather than leaving a configured input with no ISR behind it. */
static int add_input(int i)
{
	for (int k = 0; k < cb_n; k++) {
		if (cb_dev[k] == specs[i].port) {
			cb_mask[k] |= BIT(specs[i].pin);
			return 1;
		}
	}
	if (cb_n >= BOX_MAX_PORTS) {
		return 0;
	}
	cb_dev[cb_n]  = specs[i].port;
	cb_mask[cb_n] = BIT(specs[i].pin);
	cb_n++;
	return 1;
}

void box_gpio_apply_config(const box_config_t *c)
{
	for (int k = 0; k < cb_n; k++) {             /* rebuild the input set */
		gpio_remove_callback(cb_dev[k], &cb_obj[k]);
	}
	cb_n = 0;

	for (int i = 0; i < BOX_NPINS; i++) {
		if (box_gpio_reserved(i)) {
			continue;
		}
		/* Disarm first, re-arm only for inputs below. A pin that was
		 * EVER an input this boot keeps its port-level EDGE_BOTH
		 * armed unless explicitly disabled -- removing the callback
		 * only silences the EVENTS. A floating ex-input then fires
		 * the GPIO ISR on every bounce with nothing visible in any
		 * counter: an invisible CPU storm (office-stim 2026-08-04,
		 * shield swap left 6 loop pins chattering "off"). */
		gpio_pin_interrupt_configure_dt(&specs[i], GPIO_INT_DISABLE);
		switch (c->pin_mode[i]) {
		case 1:                                  /* output */
			gpio_pin_configure_dt(&specs[i], GPIO_OUTPUT_INACTIVE);
			break;
		case 2:                                  /* input */
		case 3:                                  /* input + pull-up */
			gpio_pin_configure_dt(&specs[i],
				GPIO_INPUT | (c->pin_mode[i] == 3 ? GPIO_PULL_UP : 0));
			gpio_pin_interrupt_configure_dt(&specs[i], GPIO_INT_EDGE_BOTH);
			di_unsettled[i] = 0;
			di_pub_level[i] = (uint8_t) gpio_pin_get_dt(&specs[i]);
			(void) add_input(i);
			break;
		case PIN_MODE_AIN:                       /* handed to the ADC */
			/* GPIO_DISCONNECTED sets PCR[MUX]=0 on this SoC, which IS
			 * kPORT_PinDisabledOrAnalog -- so the analog mux is reached
			 * through Zephyr's own API rather than by poking the port.
			 * It also clears IBE, so the digital input buffer stops
			 * loading the pad. */
			gpio_pin_configure_dt(&specs[i], GPIO_DISCONNECTED);
			break;
		default:                                 /* 0 = off: RELEASE the pad */
			/* `off` used to mean "pad left alone", which made it an
			 * absence rather than a state -- and the two only agreed
			 * after a reboot. Fresh boot: nothing had configured the
			 * pad, so `off` looked like high-Z. Set `off` at runtime
			 * on a pin that was `out` and it KEPT DRIVING, because
			 * nothing reconfigured it and disarming an interrupt does
			 * nothing to an output. Same config, two different
			 * electrical realities depending on history.
			 *
			 * Now it releases: high-Z, no drive, input buffer off, no
			 * interrupt (disarmed above for every pin). GPIO_DISCONNECTED
			 * is the same call `ain` uses -- PCR[MUX]=0 on this SoC.
			 * Boards whose driver does not implement it fall back to a
			 * plain input, which still does not DRIVE, and that is the
			 * property `off` has to guarantee. */
			if (gpio_pin_configure_dt(&specs[i], GPIO_DISCONNECTED) != 0) {
				(void) gpio_pin_configure_dt(&specs[i], GPIO_INPUT);
			}
			break;
		}
	}

	/* obs-mirror output (always output, any pin, overrides pin_mode) */
	if (obs_mirror_enabled(c)) {
		int p = obs_mirror_pin(c);
		if (p >= 0 && p < BOX_NPINS && !box_gpio_reserved(p)) {
			gpio_pin_configure_dt(&specs[p], GPIO_OUTPUT_INACTIVE);
		}
	}

	/* hardware obs-sync input (raw edge latch, kept out of the DI report path) */
	sync_pin = -1;
	sync_edge[0] = sync_edge[1] = 0;
	if (sync_input_enabled(c)) {
		int p = sync_input_pin(c);
		if (p >= 0 && p < BOX_NPINS && !box_gpio_reserved(p)) {
			gpio_pin_configure_dt(&specs[p], GPIO_INPUT);
			gpio_pin_interrupt_configure_dt(&specs[p], GPIO_INT_EDGE_BOTH);
			(void) add_input(p);
			sync_pin = p;
		}
	}

	for (int k = 0; k < cb_n; k++) {
		gpio_init_callback(&cb_obj[k], di_isr, cb_mask[k]);
		gpio_add_callback(cb_dev[k], &cb_obj[k]);
	}
}

void box_gpio_exec(const box_config_t *c, const gpio_cmd_t *cmd)
{
	if (cmd->op == GPIO_OP_NONE) {
		return;
	}
	if (cmd->pin >= BOX_NPINS || box_gpio_reserved(cmd->pin)) {
		return;
	}

	/* REFUSE a `do` on a pin that is not an output, rather than making it one.
	 *
	 * This used to configure any non-output pin as an output on the spot, so a
	 * bare command could precede a mode set. What it actually bought was a
	 * command that always "worked": `do 5 1` on an input, an ain channel, or a
	 * pin explicitly set `off` all silently re-muxed the pad and drove it --
	 * the CLI answering OK either way. That is the config-field-that-lies shape
	 * from the other direction: the pin mode said one thing and a command
	 * quietly overrode it, including on a pad handed to the ADC.
	 *
	 * pin_is_output() answers from the SAME ordering apply_config uses, so the
	 * obs pin still accepts the scheduled-onset path (cmd/do/<pin>/at_abs) at
	 * any pin_mode, and the sync input refuses even at `mode out`.
	 *
	 * Counted, never silent -- a refused command that vanished would be worse
	 * than the override it replaces. */
	if (!pin_is_output(c, cmd->pin)) {
		do_refused++;
		return;
	}

	/* any new DO op on a pin cancels its pending pulse falling edge, so a
	 * SET can never be clobbered later by a stale timer */
	k_timer_stop(&pulse_fall[cmd->pin]);

	if (cmd->op == GPIO_OP_SET) {
		gpio_pin_set_dt(&specs[cmd->pin], cmd->value ? 1 : 0);
		return;
	}
	if (cmd->op != GPIO_OP_PULSE || cmd->value == 0) {
		gpio_pin_set_dt(&specs[cmd->pin], 0);     /* zero-width no-op */
		return;
	}

	/* PULSE: raise now, drop when the per-pin timer expires (never blocks) */
	gpio_pin_set_dt(&specs[cmd->pin], 1);
	k_timer_start(&pulse_fall[cmd->pin], K_USEC(cmd->value), K_NO_WAIT);
}

int box_gpio_poll_di(const box_config_t *c, box_di_event_t *out)
{
	uint64_t now = now_us();

	/* which pins ride the FIFO can change with any debounce edit; the
	 * poller refreshes the ISR's view each pass (30 loads, single core) */
	for (int i = 0; i < BOX_NPINS; i++) {
		di_use_fifo[i] = (c->debounce_ms[i] == 0) && !box_gpio_reserved(i)
			&& (i != sync_pin);
	}

	/* count-parity completion from a merged same-stamp pair (see the FIFO
	 * comment): emit before anything newer so order is preserved */
	for (int i = 0; i < BOX_NPINS; i++) {
		if (di_pair_pending[i]) {
			di_pair_pending[i] = 0;
			di_pub_level[i] = di_pair_lvl[i];
			out->pin = (uint8_t) i;
			out->level = di_pair_lvl[i];
			out->t_us = di_pair_t[i];
			return 1;
		}
	}

	/* return-edge completion of a swallowed pulse on the DEBOUNCED path
	 * (queued below where the settle sample equals the published level).
	 * This drain was MISSING through +32: the away edge went out, the
	 * queued return never did, and the published level stayed desynced --
	 * a quick tap inside the settle window left the button stuck pressed
	 * host-side until the next slow press realigned it (office-stim
	 * 2026-08-04, "misses some responses"). Same order rule as the pair
	 * drain: emit before anything newer. */
	for (int i = 0; i < BOX_NPINS; i++) {
		if (di_synth_pending[i]) {
			di_synth_pending[i] = 0;
			di_pub_level[i] = di_synth_lvl[i];
			out->pin = (uint8_t) i;
			out->level = di_synth_lvl[i];
			out->t_us = di_synth_t[i];
			return 1;
		}
	}

	/* the FIFO: exact edges, in order, whatever the stall was */
	while (di_fifo_tail != di_fifo_head) {
		di_edge_t e = di_fifo[di_fifo_tail];

		di_fifo_tail = (uint16_t) ((di_fifo_tail + 1) % DI_FIFO_DEPTH);
		if (e.level == di_pub_level[e.pin]) {
			/* two edges merged inside one IRQ latency: restore count
			 * parity as a pair at the same (honest) stamp */
			out->pin = e.pin;
			out->level = (uint8_t) !e.level;
			out->t_us = e.t_us;
			di_pair_pending[e.pin] = 1;
			di_pair_lvl[e.pin] = e.level;
			di_pair_t[e.pin] = e.t_us;
			return 1;
		}
		di_pub_level[e.pin] = e.level;
		out->pin = e.pin;
		out->level = e.level;
		out->t_us = e.t_us;
		return 1;
	}

	/* legacy latch path: debounced pins only (collapse is the point there) */
	for (int i = 0; i < BOX_NPINS; i++) {
		if (di_use_fifo[i]) {
			continue;
		}
		unsigned int k = irq_lock();
		uint8_t  uns   = di_unsettled[i];
		uint64_t last  = di_last_edge_us[i];
		uint64_t first = di_first_edge_us[i];
		irq_unlock(k);

		if (!uns || box_gpio_reserved(i)) {
			continue;
		}
		uint64_t win = (uint64_t) c->debounce_ms[i] * 1000u;
		if (now - last < win) {
			continue;                        /* still bouncing */
		}
		uint8_t lvl = (uint8_t) gpio_pin_get_dt(&specs[i]);
		k = irq_lock();
		di_unsettled[i] = 0;
		irq_unlock(k);
		if (lvl != di_pub_level[i]) {
			di_pub_level[i] = lvl;
			out->pin = (uint8_t) i;
			out->level = lvl;
			out->t_us = first;
			return 1;
		}
		/* pin left the published level and came back inside the poll
		 * gap: a swallowed pulse. Emit the away edge now (at first),
		 * queue the return edge (at last) for the next call. */
		out->pin = (uint8_t) i;
		out->level = (uint8_t) !di_pub_level[i];
		out->t_us = first;
		di_synth_pending[i] = 1;
		di_synth_lvl[i] = di_pub_level[i];
		di_synth_t[i] = (last > first) ? last : first;
		return 1;
	}
	return 0;
}

uint64_t box_gpio_now_us(void)
{
	return now_us();
}

uint32_t box_gpio_di_fifo_drops(void)
{
	return di_fifo_drops;
}

uint32_t box_gpio_do_refused(void)
{
	return do_refused;
}

/* The BOOT HEARTBEAT, deliberately NOT subject to the pin-mode gate.
 *
 * box_gpio_exec now refuses a `do` on a pin the config does not call an output,
 * which is right for host commands and wrong for this: the boot blink is the
 * firmware's own sign of life, the one thing a box with no network and no
 * console still tells you. It also cannot satisfy the gate on any box that has
 * ever been saved -- box_persist_decode memcpy's the WHOLE config struct, so a
 * saved blob overwrites main's `pin_mode[LED_PIN] = 1` demo default with zero,
 * and the LED would simply stop blinking on exactly the boxes in service. It
 * blinked before this change (the old auto-configure did it), so keeping it is
 * preserving behaviour, not carving an exception.
 *
 * Bounded and one-shot by construction: the caller drives it three times at
 * boot and never again, so this cannot become a back door for anything else. */
void box_gpio_boot_pulse(int pin, uint32_t us)
{
	if (pin < 0 || pin >= BOX_NPINS || box_gpio_reserved(pin)) {
		return;
	}
	gpio_pin_configure_dt(&specs[pin], GPIO_OUTPUT_INACTIVE);
	gpio_pin_set_dt(&specs[pin], 1);
	k_busy_wait(us);
	gpio_pin_set_dt(&specs[pin], 0);
}

/* ---- TEMP diagnostics for the one-DI-event-then-silence hunt ----
 * Raw IGPIO interrupt state for the box port, read live: IMR (is the pin
 * still unmasked?), ISR (pending flag stuck?), ICR1, EDGE_SEL (both-edge
 * armed?), PSR (the actual pad level -- proves the jumper conducts without a
 * meter). Register map: DR 0x00, GDIR 0x04, PSR 0x08, ICR1 0x0C, ICR2 0x10,
 * IMR 0x14, ISR 0x18, EDGE_SEL 0x1C. */
uint32_t box_gpio_di_isr_count(void)
{
	return di_isr_n;
}

#if defined(CONFIG_SOC_SERIES_IMXRT10XX)
void box_gpio_dbg_regs(uint32_t out[5])    /* imr, isr, icr1, edge_sel, psr */
{
	volatile uint32_t *b =
		(volatile uint32_t *) DT_REG_ADDR(DT_ALIAS(box_gpio_port));

	out[0] = b[0x14 / 4];
	out[1] = b[0x18 / 4];
	out[2] = b[0x0C / 4];
	out[3] = b[0x1C / 4];
	out[4] = b[0x08 / 4];
}
#endif

void box_gpio_read_di_levels(const box_config_t *c, uint8_t levels[BOX_NPINS])
{
	for (int i = 0; i < BOX_NPINS; i++) {
		levels[i] = 0;
		/* DI modes only (in / in_pullup); outputs and off read 0.
		 *
		 * THE RESERVED CHECK IS LOAD-BEARING, not defensive tidiness. With a
		 * sparse pin map an unmapped index has specs[i].port == NULL, and
		 * gpio_pin_get_dt() on it dereferences NULL->api and jumps to garbage.
		 * A persisted config naming a pin this board does not map is the
		 * ordinary case after a renumbering, so this WILL be hit: it hard
		 * faulted on the bench the first time, "Bus fault on vector table
		 * read", from a stale pin_mode[26]=in_pullup. The old dense map made
		 * every index valid and hid the whole class. */
		if (box_gpio_reserved(i)) {
			continue;
		}
		if (c->pin_mode[i] == 2 || c->pin_mode[i] == 3) {
			int raw = gpio_pin_get_dt(&specs[i]);

			if (raw >= 0) {
				levels[i] = (uint8_t) di_logical(c, i, raw);
			}
		}
	}
}

void box_gpio_obs_mirror(const box_config_t *c, int obs)
{
	if (!obs_mirror_enabled(c)) {
		return;
	}
	int p = obs_mirror_pin(c);
	if (p >= 0 && p < BOX_NPINS && !box_gpio_reserved(p)) {
		gpio_pin_set_dt(&specs[p], obs ? 1 : 0);
	}
}
