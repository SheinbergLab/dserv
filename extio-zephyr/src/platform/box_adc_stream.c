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
#define STREAM_RING_WORDS  1536u
#define STREAM_CANARY      0xDEADBEEFu
static uint32_t stream_ring[STREAM_RING_WORDS * 2u];

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
	for (uint32_t i = STREAM_RING_WORDS; i < 2u * STREAM_RING_WORDS; i++) {
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

	for (uint32_t i = STREAM_RING_WORDS; i < 2u * STREAM_RING_WORDS; i++) {
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
				out->sw_words =
					(sizeof stream_ring - (uint32_t) st.pending_length) / 4u;
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

int box_adc_stream_start(uint8_t mask, uint32_t rate,
			 uint8_t ovs_exp, box_adc_stream_plan_t *plan)
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

void box_adc_stream_stats_reset(void)
{
	st_delivered = st_overruns = st_slips = st_fifo_ovf = st_spill = 0;
}
