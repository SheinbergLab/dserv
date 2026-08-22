/*
 * box_adc.c -- see box_adc.h.
 */
#include "box_adc.h"
#include "box_gpio.h"            /* box_gpio_now_us() -- the box clock */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/pm/device.h>
#include <errno.h>
#include <string.h>

/* The node the overlay declares on the mikroBUS socket. Chosen by LABEL rather
 * than by compatible, so fitting an ADC 20 Click means editing the overlay
 * (compatible + spi-max-frequency) and nothing here. */
#define ADC_NODE      DT_NODELABEL(box_adc)
#define BOX_USER_NODE DT_PATH(zephyr_user)

/* Per-board: which box pin each ain channel shares, 0xff for a dedicated pad.
 * Absent on a board that never switches pins to analog -- every channel is then
 * a dedicated pad and always active, which is the old behaviour exactly. */
#if DT_NODE_HAS_PROP(BOX_USER_NODE, box_ain_pins)
#define BOX_HAVE_AIN_PINMAP 1
static const uint8_t ain_pin_of[] = DT_PROP(BOX_USER_NODE, box_ain_pins);
#endif

#if DT_NODE_HAS_STATUS(ADC_NODE, okay)

static const struct device *adc = DEVICE_DT_GET(ADC_NODE);
static uint8_t  nch, res;
static uint16_t vref_mv;
static uint8_t  ready;
static uint8_t  powered;
static uint32_t sweep_max_us, sweep_n;
static uint32_t pm_suspends, pm_resumes;
static uint8_t  ovs_exp;      /* hw-average exponent (0..7 -> 1x..128x); v24 */

/* ---- which average factors the fitted converter's DRIVER accepts ----
 *
 * `adc_sequence.oversampling` is a request, not a guarantee, and the three
 * drivers this tree has met answer it three different ways:
 *
 *   * adc_mcux_lpadc     maps exp 0..7 to AVGS 1x..128x -- the full grammar;
 *   * adc_mcux_12b1msps_sar (RT1062) maps only 0/2/3/4/5 (1,4,8,16,32) and
 *     returns -ENOTSUP from EVERY adc_read() for the rest -- which the polled
 *     sampler skips silently, i.e. `ain oversample 64` = a live-looking box
 *     that never publishes a sample;
 *   * adc_mcp320x never reads the field at all -- any value "works" and the
 *     silicon averages nothing;
 *   * adc_nrfx_saadc (nRF52840) implements the FULL 1x..256x grammar -- and
 *     then refuses ALL of it with -EINVAL whenever more than one channel is
 *     active in the sequence (get_oversampling(): "Oversampling is supported
 *     for single channel only"). Our sweep is multi-channel by construction,
 *     so the honest menu for this box is 1x, and a part that reads as fully
 *     capable in its switch statement is capable of nothing here. Audited by
 *     reading the driver during the 2026-08-22 scout port, before hardware --
 *     which is the point of keeping this table.
 *
 * Audited per compatible, like ADC_DT_CHANNELS above; a part not listed here
 * claims only 1x until someone reads its driver, because refusing capability
 * a part might have is recoverable and promising capability it lacks is the
 * dead/lying sampler both listed non-defaults actually produce. */
#if DT_NODE_HAS_COMPAT(ADC_NODE, nxp_lpc_lpadc)
#define ADC_OVS_MASK 0xFFu
#elif DT_NODE_HAS_COMPAT(ADC_NODE, nxp_mcux_12b1msps_sar)
#define ADC_OVS_MASK (BIT(0) | BIT(2) | BIT(3) | BIT(4) | BIT(5))
#elif DT_NODE_HAS_COMPAT(ADC_NODE, nordic_nrf_saadc)
/* 1x only -- NOT because the silicon lacks hardware averaging (it does 1x..256x)
 * but because the driver refuses every non-zero value for a multi-channel
 * sequence, which is the only kind this box issues. Listed explicitly rather
 * than left to the default so the next reader learns the constraint instead of
 * assuming the part is simply unaudited. If a single-channel sweep path ever
 * exists, this becomes conditional on it -- not a wider constant. */
#define ADC_OVS_MASK BIT(0)
#else
#define ADC_OVS_MASK BIT(0)
#endif

uint8_t box_adc_ovs_mask(void)
{
	return ADC_OVS_MASK;      /* compile-time fact: valid before init() */
}

void box_adc_set_oversample(uint8_t exp)
{
	exp &= 7;
	/* Clamp DOWN to the nearest factor the driver performs (bit 0 is set in
	 * every mask). Both write paths refuse unsupported values, so this only
	 * fires for a value persisted by older firmware -- and rounding down
	 * trades noise, never the sweep-duration budget the saturation guard
	 * protects, which rounding up could blow. */
	while (exp && !(ADC_OVS_MASK & BIT(exp))) {
		exp--;
	}
	ovs_exp = exp;
}

/* How many channels the fitted part has.
 *
 * FROM THE DEVICETREE, not by probing. The first version walked channels upward
 * until adc_channel_setup() refused one -- authoritative, but it made a NORMAL
 * boot log `E: unsupported channel id '4'` every time, because provoking the
 * error WAS the detection. An expected condition reported as an error is how a
 * real error gets ignored, and this one sat in box3's boot log looking like the
 * cause of a hang it had nothing to do with.
 *
 * The compatible already states the part, so read it there and only fall back to
 * probing for a part not listed. Widening to the ADC 20 means adding its
 * compatible here alongside the overlay change. */
#if DT_NODE_HAS_COMPAT(ADC_NODE, microchip_mcp3204)
#define ADC_DT_CHANNELS 4
#elif DT_NODE_HAS_COMPAT(ADC_NODE, microchip_mcp3208)
#define ADC_DT_CHANNELS 8
#else
#define ADC_DT_CHANNELS 0        /* unknown part -- probe, and accept the noise */
#endif

/* ---- the CHANNEL MAP, for converters where index != input ----
 *
 * Probing works on an MCP3204 because channel n IS input n: one 3-byte
 * transaction, the channel number goes on the wire. That is a property of THAT
 * PART, and it does not generalise. On the MCXN947's on-chip LPADC,
 * `channel_id` only indexes a CMD-register slot and the physical input comes
 * from `input_positive` -- so probing there would "succeed" for every slot the
 * driver has (15 of them) and report a channel count that reflects nothing on
 * the board. A confident, self-consistent, entirely fictional sweep.
 *
 * So: if the devicetree node declares channel@N children, they ARE the map and
 * probing is not used. The box's flat wire index (`ain group G channels 0,1`)
 * stays `channel_id`, which is `reg` -- the contract is untouched; it just
 * resolves to a pad the BOARD chose rather than to an assumption this file made.
 *
 * A part that declares no children keeps the old behaviour exactly.
 */
#if DT_CHILD_NUM_STATUS_OKAY(ADC_NODE) > 0
#define BOX_ADC_DT_MAP 1
#define CH_CFG(node)  ADC_CHANNEL_CFG_DT(node),
#define CH_RES(node)  DT_PROP_OR(node, zephyr_resolution, 12),
#define CH_VREF(node) DT_PROP_OR(node, zephyr_vref_mv, 3300),
static const struct adc_channel_cfg dt_ch[]  = { DT_FOREACH_CHILD_STATUS_OKAY(ADC_NODE, CH_CFG) };
static const uint8_t                dt_res[] = { DT_FOREACH_CHILD_STATUS_OKAY(ADC_NODE, CH_RES) };
static const uint16_t               dt_vrf[] = { DT_FOREACH_CHILD_STATUS_OKAY(ADC_NODE, CH_VREF) };

/* Configure every declared channel. A channel the board DECLARED but the driver
 * refuses is a devicetree error, not a "the part is narrower than we thought" --
 * fail the whole init rather than silently running a shorter sweep, because the
 * short sweep would still publish and still look plausible. */
static uint8_t setup_from_dt(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(dt_ch); i++) {
		if (adc_channel_setup(adc, &dt_ch[i]) != 0) {
			return 0;
		}
	}
	return (uint8_t) ARRAY_SIZE(dt_ch);
}
#endif

#ifndef BOX_ADC_DT_MAP
static uint8_t probe_channels(void)
{
	uint8_t n = 0;
	uint8_t lim = ADC_DT_CHANNELS ? ADC_DT_CHANNELS : BOX_ADC_MAX_CH;

	for (uint8_t ch = 0; ch < lim; ch++) {
		struct adc_channel_cfg cfg = {
			.gain             = ADC_GAIN_1,
			.reference        = ADC_REF_EXTERNAL0,
			.acquisition_time = ADC_ACQ_TIME_DEFAULT,
			.channel_id       = ch,
			.differential     = 0,
		};

		if (adc_channel_setup(adc, &cfg) != 0) {
			break;
		}
		n++;
	}
	return n;
}
#endif /* !BOX_ADC_DT_MAP */

int box_adc_init(void)
{
	if (ready) {
		return 0;
	}
	if (!device_is_ready(adc)) {
		return -ENODEV;
	}

#ifdef BOX_ADC_DT_MAP
	nch = setup_from_dt();
	if (nch == 0) {
		return -EIO;
	}
	/* ONE resolution and ONE vref for the whole sweep -- the block header the
	 * box publishes carries a single scale for all channels, so a board that
	 * mixed them per channel could not be represented on the wire. Taken from
	 * channel 0; declaring channels with differing scales is a board bug this
	 * cannot express, not a case to support. */
	res     = dt_res[0];
	vref_mv = dt_vrf[0];
#else
	nch = probe_channels();
	if (nch == 0) {
		return -EIO;
	}

	/* Both parts we intend to fit are 12-bit. Kept as a variable rather than a
	 * constant so the value published to dserv comes from one place when a
	 * wider part appears. */
	res     = 12;
	vref_mv = DT_PROP_OR(ADC_NODE, zephyr_vref_mv, 3300);
#endif
	ready   = 1;
	/* The device is ACTIVE coming out of init on both paths: without
	 * CONFIG_PM_DEVICE nothing ever suspends it, and with it (and no runtime
	 * PM) pm_device_driver_init() resumes it. Latched rather than queried so
	 * that "powered" means what THIS file did, not what a PM subsystem we do
	 * not otherwise use believes. */
	powered = 1;
	return 0;
}

/* ---- power ----
 *
 * -EALREADY is treated as success on both paths. pm_device_action_run() returns
 * it when the device is already in the requested state, which is the ordinary
 * outcome of a redundant call and not something a caller should have to
 * distinguish -- the postcondition asked for holds either way. */
int box_adc_suspend(void)
{
	if (!ready) {
		return -ENODEV;
	}
#if defined(CONFIG_PM_DEVICE)
	if (!powered) {
		return 0;
	}
	int rc = pm_device_action_run(adc, PM_DEVICE_ACTION_SUSPEND);

	if (rc == 0 || rc == -EALREADY) {
		powered = 0;
		pm_suspends++;
		return 0;
	}
	/* Left POWERED on failure, deliberately: a converter we failed to switch
	 * off is still a converter that answers, and claiming otherwise would make
	 * box_adc_sweep() refuse work it could actually do. */
	return rc;
#else
	return -ENOTSUP;
#endif
}

int box_adc_resume(void)
{
	if (!ready) {
		return -ENODEV;
	}
#if defined(CONFIG_PM_DEVICE)
	if (powered) {
		return 0;
	}
	int rc = pm_device_action_run(adc, PM_DEVICE_ACTION_RESUME);

	if (rc == 0 || rc == -EALREADY) {
		powered = 1;
		pm_resumes++;
		return 0;
	}
	return rc;
#else
	return -ENOTSUP;      /* never suspended, so nothing to undo */
#endif
}

int box_adc_powered(void) { return powered; }

void box_adc_pm_stats(uint32_t *suspends, uint32_t *resumes)
{
	if (suspends) *suspends = pm_suspends;
	if (resumes)  *resumes  = pm_resumes;
}

int box_adc_input_of(uint8_t ch, uint8_t *differential)
{
#ifdef BOX_ADC_DT_MAP
	if (ch < ARRAY_SIZE(dt_ch)) {
		if (differential) {
			*differential = dt_ch[ch].differential ? 1u : 0u;
		}
		/* Gain is reported as "not plain" by refusing, not by scaling:
		 * a caller building its own conversion command cannot honour a
		 * gain it does not know about, and silently converting at 1x
		 * what the board declared at 2x is a wrong number that looks
		 * right. Only ADC_GAIN_1 is claimed here. */
		if (dt_ch[ch].gain != ADC_GAIN_1) {
			return -1;
		}
#if defined(CONFIG_ADC_CONFIGURABLE_INPUTS)
		return (int) dt_ch[ch].input_positive;
#else
		/* No input mux on this converter -- the driver did not select
		 * ADC_CONFIGURABLE_INPUTS, so `input_positive` does not exist
		 * in adc_channel_cfg at all and channel_id IS the physical
		 * input (adc_mcux_12b1msps_sar writes it straight into
		 * ADC_HC[ADCH]; the teensy overlays declare reg accordingly). */
		return (int) dt_ch[ch].channel_id;
#endif
	}
#else
	ARG_UNUSED(ch);
	if (differential) {
		*differential = 0;
	}
#endif
	return -1;
}

int box_adc_pin_of(uint8_t ch)
{
#ifdef BOX_HAVE_AIN_PINMAP
	if (ch < ARRAY_SIZE(ain_pin_of) && ain_pin_of[ch] != 0xff) {
		return (int) ain_pin_of[ch];
	}
#else
	ARG_UNUSED(ch);
#endif
	return -1;
}

uint8_t box_adc_active_mask(const box_config_t *c)
{
	uint8_t m = 0;

	for (uint8_t ch = 0; ch < nch && ch < BOX_ADC_MAX_CH; ch++) {
		int pin = box_adc_pin_of(ch);

		if (pin < 0 || (c && pin_is_ain(c, pin))) {
			m |= BIT(ch);
		}
	}
	return m;
}

int         box_adc_ready(void)    { return ready; }
const char *box_adc_name(void)     { return DT_NODE_FULL_NAME(ADC_NODE); }
uint8_t     box_adc_channels(void) { return nch; }
uint8_t     box_adc_bits(void)     { return res; }
uint16_t    box_adc_vref_mv(void)  { return vref_mv; }

int box_adc_sweep(uint8_t mask, uint16_t *out, uint8_t max, uint64_t *when_us)
{
	if (!ready) {
		return -ENODEV;
	}
	/* NOT a nicety -- see box_adc.h. adc_read() on a suspended LPADC blocks
	 * forever, silently, because adc_context waits with K_FOREVER. */
	if (!powered) {
		return -EAGAIN;
	}
	if (!out || !mask) {
		return -EINVAL;
	}

	/* Refuse a channel the part does not have rather than silently sampling a
	 * narrower sweep -- a short buffer is a caller bug worth surfacing. */
	if (nch < BOX_ADC_MAX_CH && (mask >> nch) != 0) {
		return -EINVAL;
	}

	uint8_t want = 0;
	for (uint8_t ch = 0; ch < BOX_ADC_MAX_CH; ch++) {
		if (mask & BIT(ch)) {
			want++;
		}
	}
	if (want > max) {
		return -ENOSPC;
	}

	/* adc_sequence.channels IS the sweep: one call, one buffer, filled in
	 * ascending channel order. Nothing here loops over channels -- how the scan
	 * is actually clocked (per-channel transactions on the MCP3204, a single
	 * auto-sequence on a TLA2518, with or without DMA) stays the driver's
	 * business, which is the whole point of asking for a sweep. */
	uint16_t buf[BOX_ADC_MAX_CH];
	struct adc_sequence seq = {
		.channels    = mask,
		.buffer      = buf,
		.buffer_size = sizeof buf,
		.resolution  = res,
		/* LPADC hardware averaging (AVGS): 2^ovs_exp conversions per
		 * trigger, one already-averaged result back. Averaging in silicon
		 * instead of oversweeping in software is what keeps the sampler
		 * off its saturation edge -- see dserv_config.h ain_ovs. */
		.oversampling = ovs_exp,
		.calibrate   = false,
	};

	uint32_t c0 = k_cycle_get_32();
	int rc = adc_read(adc, &seq);
	uint32_t d = (uint32_t) k_cyc_to_us_floor32(k_cycle_get_32() - c0);

	/* Stamp AFTER the read, not before: the caller wants to know when these
	 * counts existed, and the conversion happens during the transfer. The
	 * residual (one sweep's duration, reported by box_adc_stats) is a known
	 * constant a caller can subtract, whereas a pre-read stamp would silently
	 * carry the driver's scheduling latency into the sample time. */
	if (when_us) {
		*when_us = box_gpio_now_us();
	}

	if (d > sweep_max_us) {
		sweep_max_us = d;
	}
	sweep_n++;

	if (rc != 0) {
		return rc;
	}
	memcpy(out, buf, (size_t) want * sizeof(uint16_t));
	return want;
}

void box_adc_stats(uint32_t *max_us, uint32_t *n)
{
	if (max_us) *max_us = sweep_max_us;
	if (n)      *n      = sweep_n;
}

void box_adc_stats_reset(void) { sweep_max_us = sweep_n = 0; }

#else  /* no ADC node on this board */

uint8_t     box_adc_ovs_mask(void) { return 0xFFu; }  /* nothing fitted to disagree */
void        box_adc_set_oversample(uint8_t exp) { ARG_UNUSED(exp); }
int         box_adc_init(void)     { return -ENODEV; }
int         box_adc_ready(void)    { return 0; }
int         box_adc_suspend(void)  { return -ENODEV; }
int         box_adc_resume(void)   { return -ENODEV; }
int         box_adc_powered(void)  { return 0; }
void        box_adc_pm_stats(uint32_t *s, uint32_t *r) { if (s) *s = 0; if (r) *r = 0; }
int         box_adc_pin_of(uint8_t ch) { ARG_UNUSED(ch); return -1; }
int         box_adc_input_of(uint8_t ch, uint8_t *d)
{ ARG_UNUSED(ch); if (d) *d = 0; return -1; }
uint8_t     box_adc_active_mask(const box_config_t *c) { ARG_UNUSED(c); return 0; }
const char *box_adc_name(void)     { return "none"; }
uint8_t     box_adc_channels(void) { return 0; }
uint8_t     box_adc_bits(void)     { return 0; }
uint16_t    box_adc_vref_mv(void)  { return 0; }

int box_adc_sweep(uint8_t mask, uint16_t *out, uint8_t max, uint64_t *when_us)
{
	ARG_UNUSED(mask); ARG_UNUSED(out); ARG_UNUSED(max); ARG_UNUSED(when_us);
	return -ENODEV;
}

void box_adc_stats(uint32_t *max_us, uint32_t *n) { if (max_us) *max_us = 0; if (n) *n = 0; }
void box_adc_stats_reset(void) { }

#endif
