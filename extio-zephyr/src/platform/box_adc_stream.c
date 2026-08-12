/* box_adc_stream -- see box_adc_stream.h. Stage 1: the bring-up instrument.
 * Stage 2: the service box_ain's sampling thread runs on.
 *
 * Pipeline: CTIMER0 MR3 match (reset-on-match => periodic, no interrupt) ->
 * INPUTMUX -> ADC0 trigger 0 (hardware-trigger enabled, targeting a command
 * chain in HIGH command slots 8..8+n so the polled driver's slots 1..N are
 * never shared) -> RESFIFO0 -> eDMA (request source 21 = FIFO-A watermark,
 * gated by the INPUTMUX request-enable) -> cyclic SRAM ring.
 *
 * Everything is counted from the hardware's own ledgers at teardown: words
 * moved from the DMA's remaining-length, overflow from LPADC STAT. A run is
 * healthy when words == expected and no FOF bit is set; anything else says
 * WHERE the pipe leaks. */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>

#include <fsl_clock.h>
#include <fsl_lpadc.h>
#include <fsl_inputmux.h>
#include <fsl_inputmux_connections.h>

#include "box_adc_stream.h"
#include "box_adc.h"
#include "box_gpio.h"               /* box_gpio_now_us() -- the box clock */
#include "../core/dserv_config.h"

/* Ring: 16 windows of 16 triggers x up to 6 channels, one u32 RESFIFO word
 * per sample. 6 KB of SRAM, DMA-reachable, no cache on this core.
 *
 * ALLOCATED DOUBLE, and the second half is a CANARY FIELD, not spare room.
 * Run 4 of the bring-up (v0.4.0+66): a mis-configured "cyclic" transfer
 * marched one full ring-length PAST the buffer -- 6 KB of ADC sample words
 * through the ethrx thread's stack, and the box died with PC = a RESFIFO
 * word (usage fault, EXC_RETURN). The instrument must be unable to do that
 * again no matter how wrong the next DMA config is: overruns land in owned
 * padding and get COUNTED (stream/spill) instead of executed. */
/* SIZED TO THE WORST CASE THE PLANNER CAN ASK FOR, which is 4 blocks x 16
 * triggers x 8 channels = 512 words. It used to be 1536 usable -- 4x what any
 * configuration could reach -- with a SECOND 1536 words of canary behind it,
 * 12 KB in total on a box whose RAM was 95% full.
 *
 * The canary is now 256 bytes rather than a full ring length. It was sized to
 * absorb the +66 fault (a mis-configured cyclic transfer marching one whole
 * ring past the end), but DETECTING that needs only the first words past the
 * boundary: an overrun writes contiguously from where the ring stops, so it
 * cannot step over a 64-word tripwire on its way out. Same detector, 6 KB
 * cheaper. */
#define STREAM_RING_WORDS   512u
#define STREAM_CANARY_WORDS  64u
#define STREAM_CANARY      0xDEADBEEFu
static uint32_t stream_ring[STREAM_RING_WORDS + STREAM_CANARY_WORDS];

/* ADC0 FIFO-A's eDMA request source on MCXN947 (fsl_inputmux_connections.h:
 * kINPUTMUX_Adc0FifoARequestToDma0Ch21Ena -- the "Ch21" is the SOURCE LINE
 * number; the eDMA per-channel mux lets any channel listen to it). */
#define STREAM_DMA_SLOT    21u
/* An eDMA channel nothing else in this build claims (no async serial/SPI). */
#define STREAM_DMA_CH      8u

/* Command slots 8.. for our chain: the polled driver builds its chain in
 * slots 1..CONFIG_LPADC_CHANNEL_COUNT on every adc_read, so high slots are
 * free real estate -- and because it reprograms trigger 0 per read too, the
 * polled path self-heals after our teardown. */
#define STREAM_CMD_BASE    8u

/* Ring depth in delivered-sample windows. Depth-1 windows of slack between
 * the mover and the collector: at depth 4 the CPU may be a whole 3 sample
 * periods late without losing anything, and when it IS later than that the
 * loss is counted (overruns) rather than silently overwritten. Bounded by
 * CONFIG_DMA_TCD_QUEUE_SIZE -- the driver's TCD pool is what actually holds
 * these, so raising one without the other just fails dma_config(). */
#define STREAM_MAX_BLOCKS  4u

/* Per-conversion time, microseconds, at the command chain's default sample
 * time -- MEASURED during the bring-up (6 channels x 4x AVGS = ~48 us against
 * a 62.5 us slot at 16 kHz), not read off a datasheet. Used only to decide
 * whether a requested oversample can fit; the hardware's own FOF and trigger
 * -overrun flags are the ledger that actually convicts a too-fast setting. */
#define STREAM_CONV_US     2u
/* Leave a fifth of every trigger slot unspent. The margin absorbs the sample
 * -time variation this constant papers over; exceeding it is not a cliff,
 * it is where FIFO overflow starts becoming likely. */
#define STREAM_DUTY_PCT    80u
/* Trigger-rate ceiling. 16 kHz is the validated point; 100 kHz is where the
 * arithmetic stops being credible for a 1-channel chain. */
#define STREAM_TRIG_MAX    100000u
/* Spacing ceiling. 16 points equally spaced across the sample window is
 * already a boxcar to the accuracy that matters here; averaging past that is
 * cheaper in silicon (AVGS, one trigger, no extra FIFO/DMA traffic) than in
 * trigger rate -- and it keeps trig_hz at the end of the envelope the bench
 * actually proved. */
#define STREAM_SPACING_MAX 16u

/* ---- geometry currently programmed into the mover ---- */
static uint32_t g_block_words;      /* words per DMA block = one window       */
static uint8_t  g_nblocks;
static uint32_t stream_majors;      /* consumed TCDs (blocks) in loop mode    */
static uint8_t  stream_reload_idx;  /* which block to re-queue next           */

/* ---- STAGE 3: sample times from the TRIGGER CLOCK, not from the ISR --------
 *
 * The cadence is exact hardware and the observation of it is not. Every window
 * the DMA completes is stamped in the callback, and that stamp carries the
 * conversion chain, the FIFO-to-DMA hand-off and the interrupt latency on top of
 * the instant we actually want. Those terms are all POSITIVE -- a stamp can be
 * late, never early -- which is the whole lever: the minimum observed error over
 * many windows IS the fixed part, and everything above it is jitter to discard.
 *
 * So sample times are generated, not measured:
 *
 *     t(window n) = anchor + (n*spacing + (spacing-1)/2) * trig_period
 *
 * evenly spaced by construction, with the ISR only ever correcting the anchor.
 * The centre of the aperture is the right instant for an AVERAGED window: the
 * value is the mean of `spacing` conversions spread across it, so its time is
 * their midpoint, not the last one. At 16x spacing that is a 2 ms correction at
 * 250 Hz -- larger than every jitter term we have been chasing.
 *
 * TRIG PERIOD COMES FROM THE REGISTER, NOT THE REQUEST. MR3 is an integer, so
 * the rate actually programmed is src/(2*(MR3+1)) and only equals the asked-for
 * rate when it divides evenly. Modelling from the request would accumulate that
 * difference as unbounded drift -- at 250 Hz a 0.1% error is 3.6 s/hour.
 *
 * The anchor tracks FAST DOWN, SLOW UP. A new minimum is certainly better
 * information (latency cannot be negative, so a lower offset is a truer one) and
 * is taken immediately; upward movement is real clock drift between the CTIMER's
 * FRO and the core counter, and is followed with a slow EMA rather than chased
 * into the jitter. Same shape that the TTL sync input's drift wander vindicated
 * (see the adaptive EMA there). */
#define STREAM_DRIFT_SHIFT 12   /* ~4096 windows: 16 s at 250 Hz */

/* THE TRIGGER CLOCK IS REGULAR BUT ITS RATE IS NOT KNOWN. CTIMER0 runs from
 * FRO_HF -- an on-chip RC oscillator, not a crystal -- so the period the
 * register arithmetic predicts is off by the FRO's tolerance. Measured on boxa
 * 2026-08-11: sample times modelled from the nominal frequency walked off a
 * 4000 us grid by ~11.5 us EVERY WINDOW, a smooth 0.29% rate error (and the
 * same error, read the other way, is why a "250 Hz" feed delivered 15042
 * samples in 60 s = 250.7/s). Nothing about that is jitter; it is the FRO
 * being an FRO.
 *
 * So the period is MEASURED against the box clock rather than computed. Within
 * an epoch we keep the LEAST-LATE observation (latency is one-sided, so the
 * minimum is the truest point); across epochs, two such points a long baseline
 * apart give the slope, with the latency common-moded out. The model is
 * piecewise linear and re-based at each epoch using the OLD parameters, so a
 * rate update changes the slope going forward without stepping the timeline --
 * and keeping (n - base) small is also what keeps the picosecond arithmetic
 * clear of overflow on a run of any length.
 *
 * Period is carried in PICOseconds: at 4 ms a 1 ns quantum is 0.25 ppm, which
 * would be ~0.9 ms/hour of avoidable drift. */
#define STREAM_EPOCH_WIN   2048u   /* ~8 s at 250 Hz; long enough that 19 us of
                                    * residual noise is ~2 ppm of slope */

static uint64_t g_trig_ns;      /* nominal trigger period, from MR3 + src clock */
static uint64_t g_deliver_ps;   /* MEASURED delivered-window period, picoseconds */
static int64_t  g_anchor_ns;    /* box-clock ns of the model's base point        */
static uint64_t g_base_n;       /* window index the anchor refers to             */
static uint8_t  g_anchor_valid;
/* current epoch's least-late observation */
static uint64_t g_ep_n;  static int64_t g_ep_ns;  static int64_t g_ep_off; static uint8_t g_ep_have;
/* previous epoch's, for the baseline */
static uint64_t g_pv_n;  static int64_t g_pv_ns;  static uint8_t g_pv_have;
static uint32_t st_resid_us, st_resid_max_us;
static int32_t  st_rate_ppm;    /* measured minus nominal, signed parts-per-million */
/* ---- one-shot rate calibration ----
 * g_clk_ppm is what we APPLY (from config); st_clk_meas_ppm is what we MEASURE
 * for the CTIMER's source against the frequency the SDK reports. They are
 * independent by construction: the measurement back-solves the true source from
 * MR3 and the observed period, so it does not depend on the correction already
 * in force and cannot wind itself up over repeated calibrations. */
static int16_t  g_clk_ppm;
static uint32_t g_src_nom_hz;   /* what CLOCK_GetCTimerClkFreq reported     */
static uint32_t g_mr3_plus1;    /* the divider actually programmed          */
static int32_t  st_clk_meas_ppm;
static int32_t  st_clk_prev_ppm;
static uint8_t  st_clk_n;
static uint8_t  st_clk_meas_valid;
/* A measurement counts as SETTLED when two successive epoch estimates agree
 * this closely. Presence is not enough: the slope is blended a quarter-weight
 * per epoch, so the first estimate is part seed and calibrating from it adopts
 * a number still on its way somewhere. Measured on the bench -- the estimate
 * walked 1793 -> 2343 -> 2553 -> ~2600 ppm over ~90 s, and calibrating at the
 * first valid point left 257 ppm on the table and needed a second pass. */
#define STREAM_CLK_SETTLE_PPM 50
#define STREAM_CLK_MIN_EST    3

/* ---- service state ---- */
static const struct device *g_dma;
static box_adc_stream_plan_t g_plan;
static uint8_t  g_running;
/* SPSC window ring. `prod` is written ONLY by the DMA callback, `cons` ONLY by
 * the sampling thread, both 32-bit aligned single words -- so neither needs a
 * lock and neither may ever be written by the other side. An earlier sketch had
 * the callback advance `cons` on overrun; that is two writers on one word, and
 * the bug it produces (a lost sample reported as a fresh one) is exactly the
 * kind that survives a bench test. */
static volatile uint32_t s_prod;
static uint32_t s_cons;
static volatile uint64_t s_ts[STREAM_MAX_BLOCKS];
K_SEM_DEFINE(s_ready, 0, 1);

static uint32_t st_delivered, st_overruns, st_slips, st_fifo_ovf, st_spill;

static void stream_dma_cb(const struct device *dev, void *ud,
			  uint32_t channel, int status)
{
	ARG_UNUSED(ud);
	if (status != 0) {
		return;
	}
	uint32_t idx = stream_reload_idx;

	stream_majors++;
	/* Stamp at DELIVERY, in the ISR. Stage 3 replaces this with trigger-edge
	 * arithmetic (the sample times are already exact in hardware -- this
	 * stamp carries ISR latency the triggers themselves did not have); until
	 * then it is still strictly better than the polled path's stamp, which
	 * carried the sampling thread's wakeup jitter on top. */
	s_ts[idx] = box_gpio_now_us();
	s_prod++;

	/* LOOP-MODE CONTRACT: each callback is one TCD consumed, and the APP
	 * re-queues it (uart_mcux_lpuart's rx ring does exactly this). Skip
	 * the reload and the pool drains in as many callbacks as it is deep and
	 * the mover halts while conversions continue -- run 6 of the bring-up:
	 * words frozen at 3 halves, FIFO full (fcount 16), FOF latched in STAT,
	 * box otherwise healthy. Blocks are consumed in queue order, so the
	 * rotation below tracks which one just completed. */
	(void) dma_reload(dev, channel,
			  (uint32_t) &ADC0->RESFIFO[0],
			  (uint32_t) &stream_ring[idx * g_block_words],
			  g_block_words * 4u);
	stream_reload_idx = (uint8_t) ((idx + 1u) % g_nblocks);

	k_sem_give(&s_ready);
}

static uint8_t avgs_mode_of(uint32_t count)
{
	uint8_t e = 0;

	while ((1u << e) < count && e < 7) {
		e++;
	}
	return e;   /* kLPADC_HardwareAverageCount1..128 are 0..7 in order */
}

static uint8_t mask_popcount(uint8_t mask)
{
	uint8_t n = 0;

	for (uint8_t m = mask; m; m >>= 1) {
		n += (m & 1u);
	}
	return n;
}

/* ---- ARM: commands, trigger, INPUTMUX, eDMA ring. Everything except the
 * metronome, so the instrument can slip its software-trigger probe in between
 * the two and bisect the pipe. ---- */
static int stream_arm(uint8_t mask, uint8_t nch, uint8_t avgs_exp,
		      uint32_t block_words, uint8_t nblocks, uint32_t *stage)
{
	/* FIRST: take the ADC interrupt away from the Zephyr driver. Its ISR
	 * (mcux_lpadc_isr) assumes an adc_read sequence is in flight and
	 * drains the FIFO through its saved buffer pointer -- run our
	 * hardware-triggered phase with that ISR live and the watermark IRQ
	 * fires into stale state: a wild write and a bus fault IN INTERRUPT
	 * CONTEXT. That single mechanism was every fault of the first
	 * bring-up night (+66 ethrx with ADC words in the registers, +67 at
	 * ~9 s, +68 at streamtest: pc = mcux_lpadc_isr+52 both banners).
	 * The DMA path needs no CPU interrupt from the ADC at all -- the
	 * watermark drives DMA REQUESTS, not IRQs. */
	*stage = 2;
	irq_disable(DT_IRQN(DT_NODELABEL(lpadc0)));
	LPADC_DisableInterrupts(ADC0, 0xFFFFFFFFu);
	LPADC_DoResetFIFO0(ADC0);
	{
		uint32_t slot = STREAM_CMD_BASE, prev = 0, ch;

		for (ch = 0; ch < 32 && slot <= STREAM_CMD_BASE + nch; ch++) {
			if (!(mask & (1u << ch))) {
				continue;
			}
			lpadc_conv_command_config_t cc;
			uint8_t diff = 0;
			int in = box_adc_input_of(ch, &diff);

			/* THE BOX'S CHANNEL INDEX IS NOT THE LPADC CHANNEL
			 * NUMBER. It is a CMD-slot index; the board picks the
			 * pad with `zephyr,input-positive`, whose low 4 bits are
			 * the LPADC channel and whose bit 5 selects side B
			 * (adc_mcux_lpadc.c:214-217). On this board ain channel
			 * 1 is CH0B -- channel ZERO, side B -- so the obvious
			 * `channelNumber = ch` read ADC0_A1 instead: an
			 * unconnected pad that reported a joystick axis at half
			 * range with an offset, because it was picking the
			 * signal up by coupling. Plausible, self-consistent and
			 * wrong, which is the whole reason box_adc.h now
			 * publishes the map instead of leaving each caller to
			 * assume one. Found on boxa 2026-08-11 by moving the
			 * stick and watching one axis behave and one not.
			 *
			 * Refuse what this cannot represent rather than convert
			 * it approximately -- a differential channel or a
			 * non-unity gain needs a command shape we do not build,
			 * and the polled path is right there. */
			if (in < 0 || diff) {
				return -9;
			}

			LPADC_GetDefaultConvCommandConfig(&cc);
			/* ADC_CMDL_ADCH's own mask, not a hand-written one: the
			 * field is FIVE bits (0x1F -- channels run to 31), while
			 * the driver's comment beside the same line says "lower
			 * 4 bits". A hand-rolled 0xF agrees with the prose and
			 * would silently fold channels 16-31 onto 0-15. Side B
			 * is bit 5, clear of the field either way. */
			cc.channelNumber       = ADC_CMDL_ADCH((uint32_t) in);
#if !(defined(FSL_FEATURE_LPADC_HAS_B_SIDE_CHANNELS) && \
	(FSL_FEATURE_LPADC_HAS_B_SIDE_CHANNELS == 0U))
			if ((uint32_t) in & 0x20u) {
				cc.sampleChannelMode = kLPADC_SampleChannelSingleEndSideB;
			}
#endif
			cc.hardwareAverageMode =
				(lpadc_hardware_average_mode_t) avgs_exp;
			cc.chainedNextCommandNumber = 0;   /* patched below */
			LPADC_SetConvCommandConfig(ADC0, slot, &cc);
			if (prev) {
				/* chain prev -> this: rewrite prev with next set */
				ADC0->CMD[prev - 1].CMDH =
					(ADC0->CMD[prev - 1].CMDH & ~ADC_CMDH_NEXT_MASK) |
					ADC_CMDH_NEXT(slot);
			}
			prev = slot;
			slot++;
		}
	}

	/* ---- 3: trigger 0 = hardware, target our chain ----------------- */
	*stage = 3;
	{
		lpadc_conv_trigger_config_t tc;

		LPADC_GetDefaultConvTriggerConfig(&tc);
		tc.targetCommandId       = STREAM_CMD_BASE;
		tc.enableHardwareTrigger = true;
		LPADC_SetConvTriggerConfig(ADC0, 0, &tc);
	}

	/* ---- 4: INPUTMUX: timer->trigger route + DMA request enable ----- */
	*stage = 4;
	INPUTMUX_Init(INPUTMUX0);
	INPUTMUX_AttachSignal(INPUTMUX0, 0, kINPUTMUX_Ctimer0M3ToAdc0Trigger);
	INPUTMUX_EnableSignal(INPUTMUX0, kINPUTMUX_Adc0FifoARequestToDma0Ch21Ena, true);
	LPADC_EnableFIFO0WatermarkDMA(ADC0, true);   /* watermark 0 = any word */

	/* ---- 5: eDMA TRUE ring off the FIFO ------------------------------
	 * The mcux edma driver has exactly one circular mode: sg-loop, entered
	 * only when the blocks carry source_gather_en/dest_scatter_en AND the
	 * config says cyclic (dma_mcux_edma.c:532,543). Everything else is a
	 * bounded scatter chain that RUNS OFF THE END -- run 4's fault. Blocks
	 * are chained in a circle and the callback fires per block; in loop mode
	 * the driver keeps the channel busy forever -- exactly what a ring
	 * should be. */
	*stage = 5;
	g_block_words     = block_words;
	g_nblocks         = nblocks;
	stream_majors     = 0;
	stream_reload_idx = 0;
	s_prod = s_cons   = 0;
	/* The timing model is per-RUN: the anchor is the box-clock instant of THIS
	 * stream's trigger 0, so it must not survive a restart (a config edit, a
	 * hold, a resync). Carrying it over would place every sample of the new run
	 * against the old run's zero. */
	g_anchor_valid = 0;
	g_anchor_ns    = 0;
	g_base_n       = 0;
	g_trig_ns      = 0;
	g_deliver_ps   = 0;
	g_ep_have = g_pv_have = 0;
	st_resid_us = st_resid_max_us = 0;
	st_rate_ppm = 0;
	st_clk_meas_valid = 0;
	st_clk_n = 0;
	st_clk_prev_ppm = 0;
	for (uint32_t i = STREAM_RING_WORDS;
	     i < STREAM_RING_WORDS + STREAM_CANARY_WORDS; i++) {
		stream_ring[i] = STREAM_CANARY;       /* arm the spill detector */
	}
	k_sem_reset(&s_ready);
	{
		struct dma_block_config blk[STREAM_MAX_BLOCKS];
		struct dma_config cfgd = { 0 };

		memset(blk, 0, sizeof blk);
		for (uint8_t i = 0; i < nblocks; i++) {
			blk[i].source_address   = (uint32_t) &ADC0->RESFIFO[0];
			blk[i].dest_address     =
				(uint32_t) &stream_ring[(uint32_t) i * block_words];
			blk[i].block_size       = block_words * 4u;
			blk[i].source_addr_adj  = DMA_ADDR_ADJ_NO_CHANGE;
			blk[i].dest_addr_adj    = DMA_ADDR_ADJ_INCREMENT;
			blk[i].source_gather_en = 1;      /* -> the driver's sg_mode */
			blk[i].dest_scatter_en  = 1;
			blk[i].next_block       = (i + 1u < nblocks) ? &blk[i + 1u] : NULL;
		}

		cfgd.channel_direction    = PERIPHERAL_TO_MEMORY;
		cfgd.dma_slot             = STREAM_DMA_SLOT;
		cfgd.source_data_size     = 4;
		cfgd.dest_data_size       = 4;
		cfgd.source_burst_length  = 4;
		cfgd.dest_burst_length    = 4;
		cfgd.block_count          = nblocks;
		cfgd.head_block           = blk;
		cfgd.cyclic               = 1;
		cfgd.dma_callback         = stream_dma_cb;
		cfgd.complete_callback_en = 1;

		if (dma_config(g_dma, STREAM_DMA_CH, &cfgd) != 0) {
			return -5;
		}
		if (dma_start(g_dma, STREAM_DMA_CH) != 0) {
			return -6;
		}
	}
	return 0;
}

/* ---- 6: CTIMER0 MR3 @ trig_hz -- the metronome ---- */
static int stream_timer_start(uint32_t trig_hz)
{
	/* THREE clocks or nothing happens, silently: the module's AHB gate
	 * (registers write into a gated block and read back zeros -- the
	 * first bring-up run's words==0), the function-clock mux, and its
	 * divider. */
	CLOCK_EnableClock(kCLOCK_Timer0);
	CLOCK_SetClkDiv(kCLOCK_DivCtimer0Clk, 1u);
	CLOCK_AttachClk(kFRO_HF_to_CTIMER0);

	uint32_t src = CLOCK_GetCTimerClkFreq(0);

	if (src == 0) {
		return -7;
	}
	g_src_nom_hz = src;
	/* APPLY THE CALIBRATION, ONCE, HERE. `src` is what the SDK believes FRO_HF
	 * runs at; g_clk_ppm is how far the silicon actually is from that. Folding
	 * it in before the divide is what makes a requested rate the delivered one
	 * -- and doing it only at start is deliberate: the rate is then right from
	 * the first sample and constant for the whole run, rather than stepping
	 * around mid-recording as a control loop chased temperature. */
	if (g_clk_ppm) {
		int64_t adj = (int64_t) src * (int64_t) g_clk_ppm / 1000000;

		src = (uint32_t) ((int64_t) src + adj);
	}
	CTIMER0->TCR = CTIMER_TCR_CRST_MASK;      /* hold in reset  */
	CTIMER0->PR  = 0;
	/* THE SIGNAL INPUTMUX ROUTES IS THE MATCH *OUTPUT*, NOT THE MATCH.
	 * Run 2 of the bring-up: timer running (tc advancing), MR3 landed,
	 * TCTRL0 = HTEN|TCMD8 -- and zero conversions, zero TEXC. The match
	 * event resets TC but MAT3 only MOVES if EMR says so, and an
	 * unmoving line has no edges for the ADC's rising-edge trigger.
	 * EMC3=toggle gives one edge per match -- HALF of them rising --
	 * so the timer runs at 2x and the ADC sees exactly trig_hz. */
	CTIMER0->MR[3] = (src / (2u * trig_hz)) - 1u;
	g_mr3_plus1 = (uint32_t) CTIMER0->MR[3] + 1u;
	/* The period we ACTUALLY got, in ns, straight from the register we just
	 * wrote -- MR3 is an integer, so this equals 1e9/trig_hz only when the
	 * source divides evenly. Stage 3 models sample times from this; using
	 * the requested rate instead would bank the rounding error as drift. */
	g_trig_ns = (2ull * ((uint64_t) CTIMER0->MR[3] + 1ull) * 1000000000ull) / src;
	CTIMER0->EMR = (CTIMER0->EMR & ~(CTIMER_EMR_EM3_MASK |
					 CTIMER_EMR_EMC3_MASK)) |
		       CTIMER_EMR_EMC3(3);        /* toggle on match */
	CTIMER0->MCR = (CTIMER0->MCR & ~(CTIMER_MCR_MR3I_MASK |
					 CTIMER_MCR_MR3S_MASK)) |
		       CTIMER_MCR_MR3R_MASK;      /* reset on match */
	CTIMER0->TCR = CTIMER_TCR_CEN_MASK;       /* run            */
	return 0;
}

/* Order matters: silence the metronome, then the trigger, then the request
 * path, then the mover -- so nothing re-arms behind us. */
static void stream_disarm(void)
{
	CTIMER0->TCR = CTIMER_TCR_CRST_MASK;
	{
		lpadc_conv_trigger_config_t tc;

		LPADC_GetDefaultConvTriggerConfig(&tc);   /* HTEN off */
		LPADC_SetConvTriggerConfig(ADC0, 0, &tc);
	}
	LPADC_EnableFIFO0WatermarkDMA(ADC0, false);
	INPUTMUX_EnableSignal(INPUTMUX0, kINPUTMUX_Adc0FifoARequestToDma0Ch21Ena, false);
	(void) dma_stop(g_dma, STREAM_DMA_CH);
	LPADC_DoResetFIFO0(ADC0);
	/* Hand the interrupt back -- BOTH halves of it.
	 *
	 * The NVIC line is the easy half. The hard half is the LPADC's own
	 * watermark ENABLE bit, which stage 1 assumed the polled driver would
	 * re-arm on its next adc_read. It does not: adc_mcux_lpadc.c sets
	 * kLPADC_FIFO0WatermarkInterruptEnable exactly ONCE, in its init
	 * (adc_mcux_lpadc.c:964), and adc_read only rebuilds trigger 0 and the
	 * command slots. So stream_arm's blanket DisableInterrupts was
	 * PERMANENT, and the first polled sweep after any stream session waited
	 * on a completion interrupt that could never fire -- parked forever,
	 * because adc_context's completion timeout is K_FOREVER and this driver
	 * does not override it. Exactly the wedge box_adc.h warns about, reached
	 * from a direction that file did not anticipate.
	 *
	 * It presents as a sampler that simply stops: `running` 1, `powered` 1,
	 * `sweeps` frozen, box otherwise healthy and answering. Measured on boxa
	 * (0.4.0+72) the moment `ain pace stream` was switched back to polled.
	 * Latent in the stage-1 commit too -- cmd/ain/streamtest left the ADC
	 * deaf the same way, and only escaped notice because the bring-up box
	 * had analog disabled. */
	LPADC_ClearStatusFlags(ADC0, 0xFFFFFFFFu);
#if (defined(FSL_FEATURE_LPADC_FIFO_COUNT) && (FSL_FEATURE_LPADC_FIFO_COUNT == 2U))
	LPADC_EnableInterrupts(ADC0, kLPADC_FIFO0WatermarkInterruptEnable);
#else
	LPADC_EnableInterrupts(ADC0, kLPADC_FIFOWatermarkInterruptEnable);
#endif
	irq_enable(DT_IRQN(DT_NODELABEL(lpadc0)));
}

static uint32_t stream_count_spill(void)
{
	uint32_t n = 0;

	for (uint32_t i = STREAM_RING_WORDS;
	     i < STREAM_RING_WORDS + STREAM_CANARY_WORDS; i++) {
		if (stream_ring[i] != STREAM_CANARY) {
			n++;
		}
	}
	return n;
}

int box_adc_stream_test(const box_config_t *cfg, uint32_t trig_hz,
			uint32_t avgs_count, uint32_t ms,
			box_adc_stream_result_t *out)
{
	const struct device *dma = DEVICE_DT_GET(DT_NODELABEL(edma0));
	uint32_t mask, nch;

	memset(out, 0, sizeof *out);
	out->trig_hz = trig_hz;
	out->avgs    = avgs_count ? avgs_count : 1;

	if (trig_hz < 100 || trig_hz > 100000 || ms < 10 || ms > 5000) {
		out->rc = -1; out->stage = 1;
		return -1;
	}
	if (!device_is_ready(dma)) {
		out->rc = -2; out->stage = 1;
		return -2;
	}
	if (g_running) {
		/* The service owns the hardware. Refuse rather than tear its
		 * geometry out from under it -- the caller holds analog first
		 * (main.c does), which stops the service, so reaching this means
		 * the hold did not take. */
		out->rc = -8; out->stage = 1;
		return -8;
	}
	g_dma = dma;

	/* Same channels the polled sweeps use; nothing configured = nothing
	 * to stream. (The caller held analog and we resume the converter
	 * ourselves: the hold may have PM-suspended it.) */
	mask = box_adc_active_mask(cfg);
	if (mask == 0) {
		mask = 0x3;             /* bare-bench fallback: ch0+ch1 */
	}
	(void) box_adc_resume();

	nch = mask_popcount((uint8_t) mask);
	if (nch > 6) {
		out->rc = -3; out->stage = 1;
		return -3;
	}
	out->nch = nch;

	/* The instrument keeps the geometry it was validated with: two blocks
	 * of half the ring. It is not measuring delivery cadence -- it is
	 * measuring whether words move at all, and changing what it measures
	 * would cost us the baseline every future run is compared against. */
	{
		int rc = stream_arm((uint8_t) mask, (uint8_t) nch,
				    avgs_mode_of(out->avgs),
				    STREAM_RING_WORDS / 2u, 2u, &out->stage);

		if (rc != 0) {
			out->rc = rc;
			goto teardown;
		}
	}

	/* ---- 5b: SOFTWARE-TRIGGER PROBE through the same chain + DMA -----
	 * One chain execution with the trigger in software mode, before the
	 * timer phase: if these words move, commands + FIFO + request line +
	 * eDMA are ALL proven and only the timer->trigger edge remains; if
	 * they don't, the fault is in this leg and the timer is innocent. */
	{
		lpadc_conv_trigger_config_t tc;

		LPADC_GetDefaultConvTriggerConfig(&tc);
		tc.targetCommandId       = STREAM_CMD_BASE;
		tc.enableHardwareTrigger = false;
		LPADC_SetConvTriggerConfig(ADC0, 0, &tc);
		LPADC_DoSoftwareTrigger(ADC0, 1u);        /* trigger 0 */
		k_busy_wait(3000);
		out->sw_stat = ADC0->STAT;
		{
			struct dma_status st = { 0 };

			if (dma_get_status(dma, STREAM_DMA_CH, &st) == 0) {
				/* From what the transfer was CONFIGURED for, not
				 * sizeof(stream_ring) -- the array carries the
				 * canary too, so the whole-array form over-counted
				 * by the padding. */
				uint32_t total = g_block_words * g_nblocks * 4u;

				out->sw_words = (total >= (uint32_t) st.pending_length)
					? (total - (uint32_t) st.pending_length) / 4u : 0u;
			}
		}
		/* back to hardware mode for the real run */
		tc.enableHardwareTrigger = true;
		LPADC_SetConvTriggerConfig(ADC0, 0, &tc);
	}

	out->stage = 6;
	if (stream_timer_start(trig_hz) != 0) {
		out->rc = -7;
		goto teardown;
	}

	/* ---- 7: let it stream ------------------------------------------- */
	out->stage = 7;
	k_msleep(ms);

	/* Words moved: the callback fires per BLOCK, so majors counts blocks;
	 * the in-flight partial comes from the DMA's remaining-length ledger,
	 * best-effort (cyclic status semantics are looser -- the block count is
	 * the primary meter). Read BEFORE stopping anything. */
	{
		struct dma_status st = { 0 };
		uint32_t part = 0;

		if (dma_get_status(dma, STREAM_DMA_CH, &st) == 0 &&
		    (uint32_t) st.pending_length <= g_block_words * 4u) {
			part = (g_block_words * 4u - (uint32_t) st.pending_length) / 4u;
		}
		out->words = stream_majors * g_block_words + part;
	}
	out->spill    = stream_count_spill();
	out->majors   = stream_majors;
	out->expected = (uint32_t) (((uint64_t) trig_hz * ms / 1000u) * nch);
	out->adc_stat = ADC0->STAT;
	out->tc_end   = CTIMER0->TC;
	out->mr3      = CTIMER0->MR[3];
	out->tctrl0   = ADC0->TCTRL[0];
	out->fcount   = LPADC_GetConvResultCount(ADC0, 0);
	out->emr      = CTIMER0->EMR;
	/* INPUTMUX select readback via the same encoding AttachSignal wrote:
	 * connection enum = value + (register byte offset << PMUX_SHIFT). */
	out->imux0    = *(volatile uint32_t *)
		((uint32_t) INPUTMUX0 +
		 ((uint32_t) kINPUTMUX_Ctimer0M3ToAdc0Trigger >> PMUX_SHIFT));
	memcpy(out->ring0, stream_ring, sizeof out->ring0);
	out->rc = 0;

teardown:
	stream_disarm();
	return out->rc;
}

/* ---- STAGE 2: the service ------------------------------------------------ */

void box_adc_stream_plan(uint8_t mask, uint32_t rate, uint8_t ovs_exp,
			 box_adc_stream_plan_t *p)
{
	uint8_t nch = mask_popcount(mask);

	memset(p, 0, sizeof *p);
	p->mask = mask;
	p->nch  = nch;
	if (nch == 0 || rate == 0) {
		return;
	}

	uint32_t period_us = 1000000u / rate;
	uint32_t budget_us = period_us * STREAM_DUTY_PCT / 100u;
	uint32_t total     = 1u << (ovs_exp & 7u);

	/* 1. TOTAL oversample, not the split, is what decides feasibility -- and
	 * that is the whole argument for this pipeline. Conversion work per
	 * delivered sample is nch * total * CONV_US no matter how the total is
	 * divided between spacing and AVGS, so if it does not fit the sample
	 * period, no arrangement makes it fit and the honest move is to average
	 * less and SAY SO. (The polled path hit this wall as a wedge instead:
	 * 128x at 6 channels is ~1.5 ms of back-to-back conversion inside a 1 ms
	 * period, and a cooperative sampler that never parks owns the machine.) */
	while (total > 1u && (uint32_t) nch * total * STREAM_CONV_US > budget_us) {
		total >>= 1;
		p->clamped = 1;
	}
	if ((uint32_t) nch * total * STREAM_CONV_US > budget_us) {
		p->clamped = 1;   /* even 1x is over: the RATE is the problem */
	}

	/* 2. Split. Spacing first, because it averages across the whole sample
	 * window instead of a burst at one instant -- a real boxcar aperture,
	 * which is anti-aliasing rather than just noise reduction. Then give
	 * back to AVGS whatever spacing cannot carry. */
	uint32_t spacing = total, avgs = 1u;

	while (spacing > STREAM_SPACING_MAX) {
		spacing >>= 1; avgs <<= 1;
	}
	while (spacing > 1u && rate * spacing > STREAM_TRIG_MAX) {
		spacing >>= 1; avgs <<= 1;
	}
	/* ...and whatever the ring cannot hold: a window is one DMA block, and
	 * two blocks is the shallowest ring that is still a ring. */
	while (spacing > 1u && spacing * nch > STREAM_RING_WORDS / 2u) {
		spacing >>= 1; avgs <<= 1;
	}

	p->spacing  = (uint16_t) spacing;
	p->avgs_exp = avgs_mode_of(avgs);
	p->trig_hz  = rate * spacing;
	p->slot_us  = 1000000u / p->trig_hz;
	p->chain_us = (uint32_t) nch * avgs * STREAM_CONV_US;

	uint32_t bw = spacing * nch;

	p->nblocks = (uint8_t) (STREAM_RING_WORDS / bw);
	if (p->nblocks > STREAM_MAX_BLOCKS) {
		p->nblocks = STREAM_MAX_BLOCKS;
	}
	if (p->nblocks < 2u) {
		p->nblocks = 2u;
	}
}

int box_adc_stream_start(uint8_t mask, uint32_t rate, uint8_t ovs_exp,
			 int16_t clk_ppm, box_adc_stream_plan_t *plan)
{
	const struct device *dma = DEVICE_DT_GET(DT_NODELABEL(edma0));
	uint32_t stage = 0;

	if (g_running) {
		return -EALREADY;
	}
	if (!device_is_ready(dma)) {
		return -ENODEV;
	}
	g_dma = dma;

	if (mask == 0 || rate == 0) {
		return -EINVAL;
	}
	g_clk_ppm = clk_ppm;
	box_adc_stream_plan(mask, rate, ovs_exp, &g_plan);
	if (g_plan.nch == 0 || g_plan.nch > 6) {
		return -EINVAL;
	}

	int rc = stream_arm(mask, g_plan.nch, g_plan.avgs_exp,
			    (uint32_t) g_plan.spacing * g_plan.nch,
			    g_plan.nblocks, &stage);

	if (rc != 0) {
		stream_disarm();
		return -EIO;
	}
	if (stream_timer_start(g_plan.trig_hz) != 0) {
		stream_disarm();
		return -EIO;
	}
	/* Seed the model with the NOMINAL period. It is the right starting guess
	 * and the wrong final answer -- FRO_HF is an RC oscillator, so the epoch
	 * estimator above will walk this to the real rate within a few epochs.
	 * ain/dbg/rate_ppm reports how far it had to go. */
	g_deliver_ps = g_trig_ns * (uint64_t) g_plan.spacing * 1000ull;
	g_running = 1;
	if (plan) {
		*plan = g_plan;
	}
	return 0;
}

void box_adc_stream_stop(void)
{
	if (!g_running) {
		return;
	}
	g_running = 0;
	stream_disarm();
	/* Fold the hardware's verdict on the run just ended into the counters
	 * before the flags are cleared for the next one. */
	st_spill = stream_count_spill();
}

int box_adc_stream_running(void) { return g_running; }

int box_adc_stream_wait(k_timeout_t timeout)
{
	if (!g_running) {
		return 0;
	}
	if (s_prod != s_cons) {
		return 1;               /* already behind; don't wait to be told */
	}
	return k_sem_take(&s_ready, timeout) == 0 ? 1 : 0;
}

int box_adc_stream_read(int16_t *scan, uint8_t max, uint64_t *t_us)
{
	if (!g_running || !scan) {
		return 0;
	}

	uint32_t prod = s_prod;      /* one read: the ISR may advance it under us */

	if (prod == s_cons) {
		return 0;
	}
	/* Fell behind by more than the ring holds: the windows between `cons`
	 * and here have been overwritten by the mover. Skip to the oldest one
	 * that is still intact and COUNT what was lost -- the alternative,
	 * reading a block being written, publishes a sample that is half one
	 * instant and half another. */
	if (prod - s_cons > (uint32_t) g_nblocks - 1u) {
		uint32_t lost = (prod - s_cons) - ((uint32_t) g_nblocks - 1u);

		st_overruns += lost;
		s_cons = prod - ((uint32_t) g_nblocks - 1u);
	}

	uint32_t idx  = s_cons % g_nblocks;
	const uint32_t *w = &stream_ring[idx * g_block_words];
	uint64_t stamp = s_ts[idx];
	/* `s_cons` IS the model's window index: it starts at 0 with the stream and
	 * jumps forward on an overrun, which is right -- the skipped windows really
	 * did happen, and the trigger clock kept counting them. */
	uint64_t n = s_cons;

	s_cons++;

	/* PHASE CHECK. Every RESFIFO word carries the command slot that produced
	 * it, so the first word of a window must come from the first link of our
	 * chain. If it does not, the FIFO slipped -- an overflow dropped a word
	 * and every channel in this window is now attributed to its neighbour.
	 * That is silent corruption of the worst kind (plausible numbers, wrong
	 * signals), so it ends the window AND the stream: the caller restarts
	 * from a reset FIFO rather than publishing a guess. */
	if (((w[0] & ADC_RESFIFO_CMDSRC_MASK) >> ADC_RESFIFO_CMDSRC_SHIFT) !=
	    STREAM_CMD_BASE) {
		st_slips++;
		return -EILSEQ;
	}

	uint32_t acc[BOX_ADC_MAX_CH] = { 0 };
	uint32_t spacing = g_plan.spacing;
	uint8_t  nch     = g_plan.nch;

	for (uint32_t s = 0; s < spacing; s++) {
		for (uint8_t j = 0; j < nch; j++) {
			/* Same arithmetic the Zephyr driver applies to a polled
			 * result (adc_mcux_lpadc.c: (conv_result >> 3) & 0xFFF --
			 * a 12-bit result sits left-aligned in the 16-bit D
			 * field). Matching it EXACTLY is what keeps stream and
			 * polled numerically interchangeable, which is what lets
			 * an existing eye calibration survive the switch. */
			acc[j] += (w[s * nch + j] >> 3) & 0xFFFu;
		}
	}

	uint8_t out = 0;

	for (uint8_t ch = 0; ch < BOX_ADC_MAX_CH && out < nch; ch++) {
		if (g_plan.mask & BIT(ch)) {
			if (ch >= max) {
				break;
			}
			scan[ch] = (int16_t) (acc[out] / spacing);
			out++;
		}
	}

	/* ---- STAGE 3: correct the anchor, then GENERATE the sample time ----
	 *
	 * The stamp observed in the callback belongs to the LAST trigger of this
	 * window plus a positive, mostly-constant delay. Model that trigger, take
	 * the difference as this window's offset estimate, and filter it fast-down
	 * / slow-up (see STREAM_DRIFT_SHIFT above). */
	if (g_deliver_ps) {
		int64_t stamp_ns = (int64_t) (stamp * 1000ull);
		/* Where the model says this window's LAST trigger was -- the instant
		 * the ISR observation belongs to (the DMA cannot deliver before the
		 * final conversion of the window has landed). */
		int64_t model_ns = g_anchor_ns +
			(int64_t) (((n - g_base_n) * g_deliver_ps) / 1000ull);
		int64_t off      = stamp_ns - model_ns;

		if (!g_anchor_valid) {
			g_anchor_ns    = stamp_ns;
			g_base_n       = n;
			g_anchor_valid = 1;
			off            = 0;
		} else if (off < 0) {
			g_anchor_ns += off;         /* earlier than modelled: truer */
		} else {
			g_anchor_ns += off >> STREAM_DRIFT_SHIFT;
		}

		st_resid_us = (uint32_t) (off > 0 ? off / 1000 : 0);
		if (st_resid_us > st_resid_max_us) {
			st_resid_max_us = st_resid_us;
		}

		/* ---- rate measurement: keep this epoch's LEAST-LATE point ---- */
		if (!g_ep_have || off < g_ep_off) {
			g_ep_have = 1; g_ep_off = off; g_ep_n = n; g_ep_ns = stamp_ns;
		}
		if (g_ep_have && (n - g_base_n) >= STREAM_EPOCH_WIN) {
			if (g_pv_have && g_ep_n > g_pv_n) {
				uint64_t dn = g_ep_n - g_pv_n;
				int64_t  dt = g_ep_ns - g_pv_ns;      /* ns over dn windows */

				if (dt > 0) {
					uint64_t est_ps = ((uint64_t) dt * 1000ull) / dn;

					/* Sanity-bound the estimate before trusting it:
					 * a wild one can only come from a stamp pair
					 * that straddled something we do not model (a
					 * hold, a missed overrun). Half to double the
					 * nominal is generous and still excludes
					 * nonsense. */
					uint64_t nom_ps = g_trig_ns * spacing * 1000ull;

					if (est_ps > nom_ps / 2ull && est_ps < nom_ps * 2ull) {
						/* Blend, don't jump: each epoch's slope
						 * carries ~2 ppm of noise from the
						 * residual, and averaging beats trusting
						 * any single baseline. */
						g_deliver_ps += ((int64_t) est_ps -
								 (int64_t) g_deliver_ps) / 4;
						st_rate_ppm = (int32_t)
							(((int64_t) g_deliver_ps - (int64_t) nom_ps)
							 * 1000000ll / (int64_t) nom_ps);

						/* BACK-SOLVE THE SOURCE CLOCK. The divider is an
						 * integer we chose and the period is now measured,
						 * so the true frequency follows directly:
						 *   F = 2*(MR3+1) / T
						 * Stated against the SDK's nominal, this is the
						 * per-box constant `ain clkppm` wants -- and
						 * because it comes from MR3 rather than from the
						 * frequency we assumed, calibrating a box that is
						 * ALREADY calibrated yields the same answer
						 * instead of compounding. */
						uint64_t trig_ps = g_deliver_ps / spacing;

						if (trig_ps && g_src_nom_hz) {
							uint64_t f_true = (2ull * g_mr3_plus1 *
									   1000000000000ull) / trig_ps;

							int32_t now_ppm = (int32_t)
								(((int64_t) f_true - (int64_t) g_src_nom_hz)
								 * 1000000ll / (int64_t) g_src_nom_hz);
							int32_t d = now_ppm - st_clk_prev_ppm;

							if (d < 0) {
								d = -d;
							}
							if (st_clk_n < 255u) {
								st_clk_n++;
							}
							if (st_clk_n >= STREAM_CLK_MIN_EST &&
							    d <= STREAM_CLK_SETTLE_PPM) {
								st_clk_meas_valid = 1;
							}
							st_clk_prev_ppm = now_ppm;
							st_clk_meas_ppm = now_ppm;
						}
					}
				}
			}
			/* Re-base with the OLD parameters so the timeline stays
			 * continuous across the rate update, and (n - base) stays
			 * small enough that the picosecond product cannot overflow. */
			g_anchor_ns += (int64_t) (((g_ep_n - g_base_n) * g_deliver_ps) / 1000ull);
			g_base_n     = g_ep_n;
			g_pv_n = g_ep_n; g_pv_ns = g_ep_ns; g_pv_have = 1;
			g_ep_have = 0;
		}

		/* The APERTURE CENTRE, not the last trigger: the value is the mean of
		 * `spacing` conversions spread evenly across the window, so its
		 * instant is their midpoint -- (spacing-1)/2 trigger periods earlier.
		 * At 16x spacing that is a 2 ms correction, larger than every jitter
		 * term this pipeline was built to remove. */
		int64_t centre_ns = g_anchor_ns +
			(int64_t) (((n - g_base_n) * g_deliver_ps) / 1000ull) -
			(int64_t) (((uint64_t) (spacing - 1u) * g_deliver_ps) /
				   (2ull * spacing * 1000ull));

		stamp = (uint64_t) (centre_ns / 1000);
	}

	if (t_us) {
		*t_us = stamp;
	}
	st_delivered++;
	if (ADC0->STAT & ADC_STAT_FOF0_MASK) {
		st_fifo_ovf++;
		LPADC_ClearStatusFlags(ADC0, ADC_STAT_FOF0_MASK);
	}
	return nch;
}

void box_adc_stream_stats(uint32_t *delivered, uint32_t *overruns,
			  uint32_t *slips, uint32_t *fifo_ovf, uint32_t *spill)
{
	if (delivered) *delivered = st_delivered;
	if (overruns)  *overruns  = st_overruns;
	if (slips)     *slips     = st_slips;
	if (fifo_ovf)  *fifo_ovf  = st_fifo_ovf;
	if (spill)     *spill     = g_running ? stream_count_spill() : st_spill;
}

int box_adc_stream_clk_meas(int32_t *ppm)
{
	if (ppm) {
		*ppm = st_clk_meas_ppm;
	}
	return st_clk_meas_valid;
}

void box_adc_stream_stats_reset(void)
{
	st_delivered = st_overruns = st_slips = st_fifo_ovf = st_spill = 0;
	st_resid_max_us = 0;
}

void box_adc_stream_timing(uint32_t *trig_ns, uint32_t *deliver_us,
			   uint32_t *resid_us, uint32_t *resid_max_us,
			   int32_t *rate_ppm)
{
	if (trig_ns)  *trig_ns  = (uint32_t) g_trig_ns;
	/* The MEASURED delivered-sample period: what the block's interval_us
	 * should say, rather than 1000000/rate, so a host reshaping a batch steps
	 * sample k on the same timebase its t0 came from. Rounded to the nearest
	 * microsecond, which is all the block format carries -- the precision
	 * lives in t0. */
	if (deliver_us) {
		*deliver_us = g_deliver_ps ?
			(uint32_t) ((g_deliver_ps + 500000ull) / 1000000ull) : 0;
	}
	if (resid_us)     *resid_us     = st_resid_us;
	if (resid_max_us) *resid_max_us = st_resid_max_us;
	if (rate_ppm)     *rate_ppm     = st_rate_ppm;
}
