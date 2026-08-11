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
 * STAGE 0 exposes only the BRING-UP INSTRUMENT: run the pipeline for a bounded
 * window against the live config's channels, count everything, tear down, and
 * report -- the ADC-side twin of cmd/ota/flashtest. The production sampler
 * (box_ain's polled loop) is untouched; the polled driver reprograms trigger 0
 * and its command slots on every adc_read, so it self-heals whatever this
 * test leaves behind.
 *
 * MCXN947-only (CONFIG_BOX_ADC_STREAM): needs the CTIMER->ADC trigger route
 * (INPUTMUX Ctimer0M3ToAdc0Trigger) and the FIFO-A DMA request line (source
 * 21 on DMA0 via the eDMA per-channel mux). Both verified in the SDK headers
 * before a line of this was written.
 */
#ifndef BOX_ADC_STREAM_H
#define BOX_ADC_STREAM_H

#include <stdint.h>
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

#endif /* BOX_ADC_STREAM_H */
