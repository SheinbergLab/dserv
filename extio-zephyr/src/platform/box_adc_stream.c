/* box_adc_stream -- see box_adc_stream.h. Stage 0: the bring-up instrument.
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

static uint32_t stream_majors;      /* consumed TCDs (HALF-rings) in loop mode */
static uint8_t  stream_reload_half; /* which half to re-queue next (ping-pong) */

static void stream_dma_cb(const struct device *dev, void *ud,
			  uint32_t channel, int status)
{
	ARG_UNUSED(ud);
	if (status != 0) {
		return;
	}
	stream_majors++;
	/* LOOP-MODE CONTRACT: each callback is one TCD consumed, and the APP
	 * re-queues it (uart_mcux_lpuart's rx ring does exactly this). Skip
	 * the reload and the two-deep pool drains in two callbacks and the
	 * mover halts while conversions continue -- run 6: words frozen at 3
	 * halves, FIFO full (fcount 16), FOF latched in STAT, box otherwise
	 * healthy. The consumed half alternates by construction. */
	(void) dma_reload(dev, channel,
			  (uint32_t) &ADC0->RESFIFO[0],
			  (uint32_t) &stream_ring[(uint32_t) stream_reload_half *
						  (STREAM_RING_WORDS / 2u)],
			  (STREAM_RING_WORDS / 2u) * 4u);
	stream_reload_half ^= 1u;
}

static uint8_t avgs_mode_of(uint32_t count)
{
	uint8_t e = 0;

	while ((1u << e) < count && e < 7) {
		e++;
	}
	return e;   /* kLPADC_HardwareAverageCount1..128 are 0..7 in order */
}

int box_adc_stream_test(const box_config_t *cfg, uint32_t trig_hz,
			uint32_t avgs_count, uint32_t ms,
			box_adc_stream_result_t *out)
{
	const struct device *dma = DEVICE_DT_GET(DT_NODELABEL(edma0));
	uint32_t mask, nch = 0;

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

	/* Same channels the polled sweeps use; nothing configured = nothing
	 * to stream. (The caller held analog and we resume the converter
	 * ourselves: the hold may have PM-suspended it.) */
	mask = box_adc_active_mask(cfg);
	if (mask == 0) {
		mask = 0x3;             /* bare-bench fallback: ch0+ch1 */
	}
	(void) box_adc_resume();

	for (uint32_t m = mask; m; m >>= 1) {
		nch += (m & 1u);
	}
	if (nch > 6) {
		out->rc = -3; out->stage = 1;
		return -3;
	}
	out->nch = nch;

	/* ---- 2: LPADC command chain in high slots, FIFO reset ----------
	 * FIRST: take the ADC interrupt away from the Zephyr driver. Its ISR
	 * (mcux_lpadc_isr) assumes an adc_read sequence is in flight and
	 * drains the FIFO through its saved buffer pointer -- run our
	 * hardware-triggered phase with that ISR live and the watermark IRQ
	 * fires into stale state: a wild write and a bus fault IN INTERRUPT
	 * CONTEXT. That single mechanism was every fault of the first
	 * bring-up night (+66 ethrx with ADC words in the registers, +67 at
	 * ~9 s, +68 at streamtest: pc = mcux_lpadc_isr+52 both banners).
	 * The DMA path needs no CPU interrupt from the ADC at all -- the
	 * watermark drives DMA REQUESTS, not IRQs. */
	out->stage = 2;
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

			LPADC_GetDefaultConvCommandConfig(&cc);
			cc.channelNumber       = ch;
			cc.hardwareAverageMode =
				(lpadc_hardware_average_mode_t) avgs_mode_of(out->avgs);
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
	out->stage = 3;
	{
		lpadc_conv_trigger_config_t tc;

		LPADC_GetDefaultConvTriggerConfig(&tc);
		tc.targetCommandId       = STREAM_CMD_BASE;
		tc.enableHardwareTrigger = true;
		LPADC_SetConvTriggerConfig(ADC0, 0, &tc);
	}

	/* ---- 4: INPUTMUX: timer->trigger route + DMA request enable ----- */
	out->stage = 4;
	INPUTMUX_Init(INPUTMUX0);
	INPUTMUX_AttachSignal(INPUTMUX0, 0, kINPUTMUX_Ctimer0M3ToAdc0Trigger);
	INPUTMUX_EnableSignal(INPUTMUX0, kINPUTMUX_Adc0FifoARequestToDma0Ch21Ena, true);
	LPADC_EnableFIFO0WatermarkDMA(ADC0, true);   /* watermark 0 = any word */

	/* ---- 5: eDMA TRUE ring off the FIFO ------------------------------
	 * The mcux edma driver has exactly one circular mode: sg-loop, entered
	 * only when the blocks carry source_gather_en/dest_scatter_en AND the
	 * config says cyclic (dma_mcux_edma.c:532,543). Everything else is a
	 * bounded scatter chain that RUNS OFF THE END -- run 4's fault. The
	 * TCD pool is CONFIG_DMA_TCD_QUEUE_SIZE = 2 deep, so the ring is two
	 * chained half-ring blocks; the callback fires per HALF, and in loop
	 * mode the driver keeps the channel busy forever -- exactly what a
	 * ring should be. */
	out->stage = 5;
	stream_majors = 0;
	stream_reload_half = 0;
	for (uint32_t i = STREAM_RING_WORDS; i < 2u * STREAM_RING_WORDS; i++) {
		stream_ring[i] = STREAM_CANARY;       /* arm the spill detector */
	}
	{
		struct dma_block_config blk[2];
		struct dma_config cfgd = { 0 };

		memset(blk, 0, sizeof blk);
		for (int i = 0; i < 2; i++) {
			blk[i].source_address   = (uint32_t) &ADC0->RESFIFO[0];
			blk[i].dest_address     =
				(uint32_t) &stream_ring[(uint32_t) i * (STREAM_RING_WORDS / 2u)];
			blk[i].block_size       = (STREAM_RING_WORDS / 2u) * 4u;
			blk[i].source_addr_adj  = DMA_ADDR_ADJ_NO_CHANGE;
			blk[i].dest_addr_adj    = DMA_ADDR_ADJ_INCREMENT;
			blk[i].source_gather_en = 1;      /* -> the driver's sg_mode */
			blk[i].dest_scatter_en  = 1;
			blk[i].next_block       = (i == 0) ? &blk[1] : NULL;
		}

		cfgd.channel_direction    = PERIPHERAL_TO_MEMORY;
		cfgd.dma_slot             = STREAM_DMA_SLOT;
		cfgd.source_data_size     = 4;
		cfgd.dest_data_size       = 4;
		cfgd.source_burst_length  = 4;
		cfgd.dest_burst_length    = 4;
		cfgd.block_count          = 2;
		cfgd.head_block           = blk;
		cfgd.cyclic               = 1;
		cfgd.dma_callback         = stream_dma_cb;
		cfgd.complete_callback_en = 1;

		if (dma_config(dma, STREAM_DMA_CH, &cfgd) != 0) {
			out->rc = -5;
			goto teardown;
		}
		if (dma_start(dma, STREAM_DMA_CH) != 0) {
			out->rc = -6;
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

	/* ---- 6: CTIMER0 MR3 @ trig_hz -- the metronome ------------------ */
	out->stage = 6;
	/* THREE clocks or nothing happens, silently: the module's AHB gate
	 * (registers write into a gated block and read back zeros -- the
	 * first bring-up run's words==0), the function-clock mux, and its
	 * divider. */
	CLOCK_EnableClock(kCLOCK_Timer0);
	CLOCK_SetClkDiv(kCLOCK_DivCtimer0Clk, 1u);
	CLOCK_AttachClk(kFRO_HF_to_CTIMER0);
	{
		uint32_t src = CLOCK_GetCTimerClkFreq(0);

		if (src == 0) {
			out->rc = -7;
			goto teardown;
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
	}

	/* ---- 7: let it stream ------------------------------------------- */
	out->stage = 7;
	k_msleep(ms);

	/* Words moved: in sg-loop mode the callback fires per HALF-ring, so
	 * majors counts halves; the in-flight partial comes from the DMA's
	 * remaining-length ledger, best-effort (cyclic status semantics are
	 * looser -- the half count is the primary meter). Read BEFORE
	 * stopping anything. */
	{
		struct dma_status st = { 0 };
		uint32_t part = 0;

		if (dma_get_status(dma, STREAM_DMA_CH, &st) == 0 &&
		    (uint32_t) st.pending_length <= (STREAM_RING_WORDS / 2u) * 4u) {
			part = ((STREAM_RING_WORDS / 2u) * 4u -
				(uint32_t) st.pending_length) / 4u;
		}
		out->words = stream_majors * (STREAM_RING_WORDS / 2u) + part;
	}
	for (uint32_t i = STREAM_RING_WORDS; i < 2u * STREAM_RING_WORDS; i++) {
		if (stream_ring[i] != STREAM_CANARY) {
			out->spill++;
		}
	}
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
	/* Order matters: silence the metronome, then the trigger, then the
	 * request path, then the mover -- so nothing re-arms behind us. */
	CTIMER0->TCR = CTIMER_TCR_CRST_MASK;
	{
		lpadc_conv_trigger_config_t tc;

		LPADC_GetDefaultConvTriggerConfig(&tc);   /* HTEN off */
		LPADC_SetConvTriggerConfig(ADC0, 0, &tc);
	}
	LPADC_EnableFIFO0WatermarkDMA(ADC0, false);
	INPUTMUX_EnableSignal(INPUTMUX0, kINPUTMUX_Adc0FifoARequestToDma0Ch21Ena, false);
	(void) dma_stop(dma, STREAM_DMA_CH);
	LPADC_DoResetFIFO0(ADC0);
	/* Hand the interrupt back: clear anything our phases latched, then
	 * re-enable the NVIC line. The polled driver re-enables its own IE
	 * bits and rebuilds trigger 0 + command slots on every adc_read, so
	 * the next ordinary sweep restores its world untouched. */
	LPADC_ClearStatusFlags(ADC0, 0xFFFFFFFFu);
	irq_enable(DT_IRQN(DT_NODELABEL(lpadc0)));
	return out->rc;
}
