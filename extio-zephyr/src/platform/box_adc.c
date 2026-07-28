/*
 * box_adc.c -- see box_adc.h.
 */
#include "box_adc.h"
#include "box_gpio.h"            /* box_gpio_now_us() -- the box clock */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <errno.h>
#include <string.h>

/* The node the overlay declares on the mikroBUS socket. Chosen by LABEL rather
 * than by compatible, so fitting an ADC 20 Click means editing the overlay
 * (compatible + spi-max-frequency) and nothing here. */
#define ADC_NODE DT_NODELABEL(box_adc)

#if DT_NODE_HAS_STATUS(ADC_NODE, okay)

static const struct device *adc = DEVICE_DT_GET(ADC_NODE);
static uint8_t  nch, res;
static uint16_t vref_mv;
static uint8_t  ready;
static uint32_t sweep_max_us, sweep_n;

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

int box_adc_init(void)
{
	if (ready) {
		return 0;
	}
	if (!device_is_ready(adc)) {
		return -ENODEV;
	}

	nch = probe_channels();
	if (nch == 0) {
		return -EIO;
	}

	/* Both parts we intend to fit are 12-bit. Kept as a variable rather than a
	 * constant so the value published to dserv comes from one place when a
	 * wider part appears. */
	res     = 12;
	vref_mv = DT_PROP_OR(ADC_NODE, zephyr_vref_mv, 3300);
	ready   = 1;
	return 0;
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
		.oversampling = 0,
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

int         box_adc_init(void)     { return -ENODEV; }
int         box_adc_ready(void)    { return 0; }
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
