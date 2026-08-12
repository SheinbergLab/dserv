/*
 * box_adc_stream -- hardware-paced LPADC acquisition: CTIMER match trigger ->
 * INPUTMUX -> ADC0 command chain -> RESFIFO -> eDMA cyclic ring in SRAM.
 *
 * The CPU is not in the sample path at all: conversions start on timer edges
 * (not thread wakeups), results move by DMA request (not driver polls), and
 * the only software involvement is consuming averaged windows after the fact.
 * This is what makes sample TIMES trustworthy -- each ring slot's time is
 * trigger-edge arithmetic on a hardware clock, mappable to the PTP timeline,
 * instead of "when a Zephyr timer callback happened to run" (adc_context.h
 * runs sequence intervals off a k_timer; the +59 wedge is what thread-paced
 * sampling costs at rate).
 *
 * STAGE 1 shipped the BRING-UP INSTRUMENT (box_adc_stream_test below): run the
 * pipeline for a bounded window, count everything from the hardware's own
 * ledgers, tear down, report. It is still here, still the first thing to reach
 * for when the pipe misbehaves -- a bounded run that prints WHERE it broke is
 * worth more than a service that just goes quiet.
 *
 * STAGE 2 adds the SERVICE API (start/read/stop) that box_ain's sampling thread
 * drives instead of its k_timer + blocking adc_read. The shape of the loop above
 * is unchanged -- a scan and a timestamp arrive, groups get fed -- but WHERE the
 * cadence comes from moves from a software timer to the CTIMER, which is the
 * whole point: `late` stops meaning "samples were never taken" and starts
 * meaning only "the CPU was slow to collect samples the hardware already took".
 *
 * OVERSAMPLE IS PLANNED, NOT DECLARED. The operator asks for a base rate and a
 * total oversample (ain_rate, ain_ovs); box_adc_stream_plan() splits that total
 * into SPACING (equally-spaced triggers across the sample window) and hardware
 * AVGS (back-to-back conversions inside one trigger), preferring spacing because
 * it averages across the FULL aperture instead of a burst at one instant, and
 * falling back to AVGS only when the per-trigger conversion chain would not fit
 * in the trigger spacing. That composition is what the polled path could not do:
 * 128x at 6 channels is ~1.5 ms of back-to-back conversion, which does not fit a
 * 1 kHz sample, while 16 spaced triggers x 4x AVGS is 64x averaging with every
 * trigger's chain comfortably inside its 62.5 us slot.
 *
 * MCXN947-only (CONFIG_BOX_ADC_STREAM): needs the CTIMER->ADC trigger route
 * (INPUTMUX Ctimer0M3ToAdc0Trigger) and the FIFO-A DMA request line (source
 * 21 on DMA0 via the eDMA per-channel mux). Both verified in the SDK headers
 * before a line of this was written.
 */
#ifndef BOX_ADC_STREAM_H
#define BOX_ADC_STREAM_H

#include <stdint.h>
#include <zephyr/kernel.h>          /* k_timeout_t for the service's wait      */
#include "../core/dserv_config.h"   /* box_config_t: channel set comes from pin modes */

typedef struct {
	int32_t  rc;            /* 0 ok; <0 = setup/run failure (stage below)   */
	uint32_t stage;         /* how far setup got before rc (1..7)           */
	uint32_t trig_hz;       /* what was asked                               */
	uint32_t avgs;          /* hardware average count actually programmed   */
	uint32_t nch;           /* channels in the chain                        */
	uint32_t expected;      /* trig_hz * ms/1000 * nch = words that SHOULD land */
	uint32_t words;         /* words the DMA actually moved                 */
	uint32_t majors;        /* full ring wraps (DMA major-loop completions) */
	uint32_t adc_stat;      /* raw LPADC STAT at teardown (FOF bits etc.)   */
	uint32_t ring0[6];      /* first words of the last ring head (raw RESFIFO
	                         * words: value + TSRC/CMDSRC bits -- host decodes) */
	/* Self-debug: enough register echoes to convict a dead stage from the
	 * datapoints alone, because the bench box has no debugger attached and
	 * the whole point of the instrument is remote iteration. */
	uint32_t tc_end;        /* CTIMER0->TC after the run: 0 = timer never ran */
	uint32_t mr3;           /* CTIMER0->MR[3] echo: 0 = writes didn't land    */
	uint32_t tctrl0;        /* ADC0->TCTRL[0] echo: HTEN + TCMD as programmed */
	uint32_t fcount;        /* FIFO0 residue before teardown drain            */
	/* The software-trigger probe: one chain execution through the SAME
	 * commands and the SAME DMA before the timer phase. Bisects the pipe:
	 * sw_words == nch with words == 0 convicts the trigger ROUTING alone;
	 * sw_words == 0 convicts the command chain / FIFO / DMA leg. */
	uint32_t sw_words;      /* words DMA moved within ~3 ms of the sw trigger */
	uint32_t sw_stat;       /* LPADC STAT right after the probe (TCOMP?)      */
	uint32_t imux0;         /* INPUTMUX ADC0-trigger-0 select readback        */
	uint32_t emr;           /* CTIMER0->EMR echo (EMC3 toggle configured?)    */
	uint32_t spill;         /* canary words disturbed PAST the ring: any
	                         * nonzero = a DMA config marching out of bounds
	                         * (the +66 fault class), caught in owned padding */
} box_adc_stream_result_t;

/* Run the pipeline for `ms` milliseconds at `trig_hz` trigger rate with
 * `avgs_count` hardware averages per trigger (1,2,4,...,128; 0/1 = off).
 * Sweeps the SAME channels the polled sampler would (from cfg's pin modes).
 * BLOCKS the caller for the duration (streamtest is a bench instrument, not
 * a service); the caller must hold analog first so the polled loop stays
 * out of the ADC. */
int box_adc_stream_test(const box_config_t *cfg, uint32_t trig_hz,
			uint32_t avgs_count, uint32_t ms,
			box_adc_stream_result_t *out);

/* ---- STAGE 2: the service ------------------------------------------------ */

/* How a requested (rate, oversample) was actually realised in hardware. Every
 * field is published under ain/dbg rather than inferred, because the split is a
 * DERIVED thing and a box quietly averaging 8x when 64x was asked for is the
 * config-field-that-lies shape this tree keeps meeting. */
typedef struct {
	uint32_t trig_hz;    /* CTIMER trigger rate = rate * spacing          */
	uint16_t spacing;    /* triggers averaged into one delivered sample   */
	uint8_t  avgs_exp;   /* hardware AVGS per trigger, as 2^exp           */
	uint8_t  nch;        /* channels in the chain                         */
	uint8_t  mask;       /* which channels (bit c)                        */
	uint8_t  nblocks;    /* ring depth, in delivered-sample windows       */
	uint8_t  clamped;    /* 1 = the requested oversample did NOT fit and
	                      * was reduced; the operator asked for averaging
	                      * the converter cannot deliver at this rate     */
	uint32_t chain_us;   /* estimated per-trigger conversion chain        */
	uint32_t slot_us;    /* trigger spacing -- chain_us must fit inside   */
} box_adc_stream_plan_t;

/* Split `ovs_exp` (total oversample, 2^exp) into spacing x AVGS for `nch`
 * channels at `rate` Hz, and report the geometry that results. Pure arithmetic,
 * no hardware touched -- so the console can show what a setting WOULD do, and
 * box_ain can publish the plan even when the converter is held. */
void box_adc_stream_plan(uint8_t mask, uint32_t rate, uint8_t ovs_exp,
			 box_adc_stream_plan_t *out);

/* Start hardware-paced acquisition at `rate` delivered samples/s with total
 * oversample 2^ovs_exp over the channels in `mask`. 0 on success (plan filled
 * in), or a negative errno.
 *
 * THE CALLER PASSES THE MASK rather than this deriving it from cfg, so that the
 * two pacings sweep exactly the same channels. box_ain's union mask is already
 * box_adc_active_mask() narrowed to what some group actually wants; deriving the
 * wider set here would have made the stream convert channels nobody reads, and
 * charged their conversion time to every sample's budget.
 *
 * CALL FROM THE SAMPLING THREAD ONLY, same as every other box_adc_* entry: this
 * takes the LPADC's interrupt away from the Zephyr driver for the duration (its
 * ISR drains the FIFO through a sequence pointer that is stale outside
 * adc_read -- the fault that cost the entire first bring-up night), so a polled
 * sweep MUST NOT be attempted while a stream is running. box_ain stops the
 * stream before it hands the converter to anyone (`ain hold`, `borrow`). */
int  box_adc_stream_start(uint8_t mask, uint32_t rate, uint8_t ovs_exp,
			  int16_t clk_ppm, box_adc_stream_plan_t *plan);

/* The CTIMER source's measured deviation from the frequency the SDK reports,
 * in signed ppm. Returns 1 once a run has measured it (needs two epochs), 0
 * before that -- so a caller can refuse to calibrate from a value that is still
 * the seed rather than a measurement. Back-solved from the programmed divider
 * and the observed period, so it is independent of any correction already in
 * force: calibrating an already-calibrated box returns the same number. */
int  box_adc_stream_clk_meas(int32_t *ppm);

/* Stop the metronome and hand the ADC interrupt back. Idempotent. */
void box_adc_stream_stop(void);

int  box_adc_stream_running(void);

/* Block until at least one window has been delivered, or `timeout` expires.
 * Returns 1 if something is ready, 0 on timeout. */
int  box_adc_stream_wait(k_timeout_t timeout);

/* Take ONE delivered sample: average the oldest unread window and scatter it
 * into `scan` BY CHANNEL (scan[c] valid where plan.mask has bit c). Returns the
 * channel count, 0 when nothing is ready, or:
 *   -EILSEQ  the window's first result did not come from the first command in
 *            our chain, i.e. the FIFO slipped phase and channel identity can no
 *            longer be trusted. The caller must restart the stream and must NOT
 *            publish the window -- a channel-swapped sample is worse than a gap,
 *            because nothing downstream can tell it happened.
 * Non-blocking; drain in a loop until it returns 0. */
int  box_adc_stream_read(int16_t *scan, uint8_t max, uint64_t *t_us);

/* delivered -- windows the DMA completed (the hardware's cadence)
 * overruns  -- windows the ring wrapped over before the CPU read them: samples
 *              the hardware TOOK and software lost. The stream-mode twin of
 *              box_ain's `late`, and a strictly better failure: the cadence
 *              itself never slipped.
 * slips     -- phase losses (see -EILSEQ above); any non-zero needs a look
 * fifo_ovf  -- LPADC FIFO overflow latched: the mover fell behind the converter
 * spill     -- canary words disturbed past the ring (a DMA marching out of
 *              bounds, the +66 fault class caught in owned padding) */
void box_adc_stream_stats(uint32_t *delivered, uint32_t *overruns,
			  uint32_t *slips, uint32_t *fifo_ovf, uint32_t *spill);
void box_adc_stream_stats_reset(void);

/* ---- STAGE 3: what the timing model is actually doing --------------------
 *
 * trig_ns      the trigger period ACTUALLY programmed (from MR3 and the source
 *              clock, not from the requested rate -- they differ whenever the
 *              source does not divide evenly, and modelling from the request
 *              would bank that difference as drift)
 * deliver_us   the delivered-sample period on the same trigger clock; what a
 *              block's interval_us should carry
 * resid_us     how late the LAST window's interrupt was against the model --
 *              i.e. the jitter this pipeline removes from the sample time. The
 *              polled path baked exactly this into every stamp.
 * resid_max_us the worst of those since the last stats reset
 *
 * A residual that is small and stable means the model is tracking; one that
 * climbs steadily means the trigger clock and the box clock are drifting apart
 * faster than the anchor's EMA follows.
 * rate_ppm     how far the MEASURED rate sits from the nominal one. Not an
 *              error to fix -- it is the FRO's real tolerance, and the reason
 *              the period is measured at all. Expect hundreds of ppm.
 */
void box_adc_stream_timing(uint32_t *trig_ns, uint32_t *deliver_us,
			   uint32_t *resid_us, uint32_t *resid_max_us,
			   int32_t *rate_ppm);

#endif /* BOX_ADC_STREAM_H */
