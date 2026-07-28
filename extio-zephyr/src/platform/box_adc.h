/*
 * box_adc.h -- analog input, one SWEEP at a time.
 *
 * The platform half of the box's analog path. The portable half above it owns
 * groups, cadence, decimation and the state/ain/<label> contract; this owns
 * exactly one thing: turn a channel mask into a set of counts, with a timestamp.
 * Same split as box_ota_flash.h under box_ota.h, and for the same reason -- the
 * portable side must never learn which converter is fitted.
 *
 * THE INTERFACE IS SWEEP-SHAPED ON PURPOSE, and that is the one decision here
 * that would be expensive to reverse. A per-channel read() would work today (the
 * MCP3204 needs one SPI transaction per channel regardless) and would quietly
 * forbid every path that makes 8 channels cheap: the TLA2518's auto-sequence
 * mode clocks a whole scan in one transaction, and DMA moves it without CPU per
 * frame. Asking for the sweep keeps those inside the driver, invisible from
 * above. We are NOT building them now -- at 1 kHz the arithmetic says a 4-channel
 * sweep is ~100 us, about 10% of one thread, and DMA on this SoC brings cache
 * coherency problems worth more than the 3% it would save.
 *
 * IMPLEMENTED OVER ZEPHYR'S ADC API, not over SPI. Zephyr ships adc_mcp320x.c
 * with a `microchip,mcp3204` binding, and adc_sequence.channels is already a
 * bitmask that fills one buffer -- the sweep, exactly. So there is no driver for
 * us to write, and adding the ADC 20 Click later is a devicetree change plus a
 * Kconfig line rather than new C. (Zephyr also ships adc_tla2528.c, the I2C
 * sibling of the ADC 20's TLA2518, which is the obvious starting point if the
 * SPI part turns out to need its own driver.)
 *
 * NOT THREAD-SAFE, and deliberately blocking. adc_read() hands the work to the
 * driver's acquisition thread and waits, so a sweep parks the CALLER for its
 * whole duration. Call this from the sampling thread and nowhere else -- never
 * from the service loop, which is the one place a 100 us stall would be both
 * invisible (dbg/loop_max_us is published BY the loop, so it cannot report a
 * stall that stops it publishing) and harmful.
 */
#ifndef BOX_ADC_H
#define BOX_ADC_H

#include <stdint.h>

#define BOX_ADC_MAX_CH 8          /* the widest part we intend to fit */

/* Bring the converter up. 0 on success, or a negative errno; -ENODEV means no
 * ADC is fitted on this board, which is not an error -- most boxes have none. */
int box_adc_init(void);

/* 1 once init() has succeeded. Everything below returns -ENODEV until then. */
int box_adc_ready(void);

/* What is actually fitted, straight from the devicetree/driver rather than from
 * a build-time assumption -- a box that reports 4 channels while an 8-channel
 * part is fitted is the same class of lie as a config field that does nothing. */
const char *box_adc_name(void);      /* devicetree node name, e.g. "adc@0" */
uint8_t     box_adc_channels(void);  /* 4 (MCP3204) / 8 (ADC 20) / 0 if absent */
uint8_t     box_adc_bits(void);      /* resolution, 12 for both parts above */
uint16_t    box_adc_vref_mv(void);   /* full-scale in mV, for counts -> volts */

/* ONE SWEEP.
 *
 * Reads every channel set in `mask` (bit n = channel n) into out[], packed
 * densely in ascending channel order -- so a mask of 0b1010 yields out[0]=ch1,
 * out[1]=ch3. Returns the number of samples written, or a negative errno.
 *
 * `when_us`, if non-NULL, receives the box-clock timestamp taken immediately
 * after the sweep completes. That stamp is the point of this whole exercise: it
 * is on dserv's timeline (PTP keeps the box within well under a microsecond of
 * the host), so scheduling jitter in WHEN a sweep ran is recorded rather than
 * lost, and the host can place every sample exactly instead of assuming a
 * perfect cadence. Removing the jitter would cost far more than measuring it.
 */
int box_adc_sweep(uint8_t mask, uint16_t *out, uint8_t max, uint64_t *when_us);

/* Worst-case sweep duration in microseconds since the last reset, and the count.
 * The number that decides whether the sampling cadence is affordable -- and the
 * honest one, measured with k_cycle_get_32() around the blocking call rather
 * than inferred from the SPI clock. */
void box_adc_stats(uint32_t *sweep_max_us, uint32_t *sweep_n);
void box_adc_stats_reset(void);

#endif /* BOX_ADC_H */
