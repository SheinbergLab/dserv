/*
 * box_cmp.c -- on-chip comparator as a light detector. See box_cmp.h.
 *
 * PROTOTYPE (2026-11-24). What is assumed rather than measured is marked
 * ASSUMED below; `cmp` on the console shows the numbers needed to check it.
 *
 * LEARN, THEN LOCK. The threshold is set ONCE, at the midpoint of the dark and
 * light levels the ADC saw, and then held -- like a pot, but set by the box
 * and reaching both rails. It moves again only when asked (`cmp relearn`,
 * cmd/cmp/relearn), and never during an obs. A wandering threshold would give
 * each edge its own offset; a locked one gives the whole session one. After
 * locking the box keeps watching the levels, only to report how far their
 * midpoint has DRIFTED from the threshold (state/cmp `drift`, % of the step),
 * so a host can tell when a relearn is worth it.
 */
#include "box_cmp.h"
#include "box_gpio.h"
#include "box_console.h"
#include "box_obs.h"

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <stdlib.h>
#include <string.h>

#if defined(BOX_HAVE_ADC) && (defined(CONFIG_SOC_MCXN947) || defined(CONFIG_SOC_MIMXRT1062))

#include "box_adc.h"
#include <fsl_clock.h>

/* ==== THE HARDWARE: everything below this block is board-independent ====
 *
 * Each board says which box pin carries the line, which ain channel feeds it,
 * how many DAC levels it has, and supplies the handful of hw_* register
 * accessors. The learning, locking, drift, level reports and console are
 * shared. CMP_DAC_LEVELS sets the threshold's resolution, so a coarse DAC
 * needs a bigger minimum step (CMP_MIN_STEP) for its midpoint to mean much. */

#if defined(CONFIG_SOC_MCXN947)
/* FRDM-MCXN947, Arduino A3 = P0_22, which is both ADC0_A14 (ain ch3) and
 * CMP1_IN2, in the same pad mode (MUX 0). A5 = P0_23 is CMP2_IN2 but is also
 * SW2 with a pull-up and is MCUboot's button, so it is not offered. Box pin 15
 * is A3 in this board's box-gpios map (boards/frdm_mcxn947_mcxn947_cpu0.overlay),
 * so the line and the analog are the same pad; its ain channel is looked up. */
#define CMP_BOX_PIN    15
#define CMP_PIN_NAME   "A3"
#define CMP_AIN_CH     (-1)           /* the channel whose pad is CMP_BOX_PIN */
#define CMP_DAC_LEVELS 256            /* LPCMP: Vref * (code + 1) / 256 */
#define CMP_MIN_STEP   100            /* ~80 mV; the DAC steps 16 counts */
#define CMP_VREF_DEF   0              /* DCR.VRSEL: VREFH0 / VREFH1 -- 0 matched the ADC on the bench */
#define CMP_BASE       CMP1
#define CMP_PSEL       2u             /* IN2 */
#define CMP_MSEL_DAC   7u             /* minus input = the internal 8-bit DAC */
#define CMP_IRQN       HSCMP1_IRQn    /* CMP1's vector, named HSCMP1 in the SDK */
#define CMP_IRQ_PRIO   DT_IRQ(DT_NODELABEL(gpio0), priority)  /* same as DI edges */

static inline int hw_cout(void) { return (CMP_BASE->CSR & LPCMP_CSR_COUT_MASK) ? 1 : 0; }
/* clear the edge flags (w1c); returns the output as it was at the read */
static inline int hw_flags_take(void)
{
	uint32_t csr = CMP_BASE->CSR;

	CMP_BASE->CSR = csr & (LPCMP_CSR_CFR_MASK | LPCMP_CSR_CFF_MASK);
	return (csr & LPCMP_CSR_COUT_MASK) ? 1 : 0;
}
static inline bool hw_dac_on(void) { return (CMP_BASE->DCR & LPCMP_DCR_DAC_EN_MASK) != 0; }
static inline void hw_dac_set(uint8_t code, uint8_t vref)
{
	CMP_BASE->DCR = LPCMP_DCR_VRSEL(vref) | LPCMP_DCR_DAC_DATA(code) | LPCMP_DCR_DAC_EN_MASK;
}
static inline void hw_irq_on(void)
{
	CMP_BASE->CSR = LPCMP_CSR_CFR_MASK | LPCMP_CSR_CFF_MASK;
	CMP_BASE->IER = LPCMP_IER_CFR_IE_MASK | LPCMP_IER_CFF_IE_MASK;
}
static inline void hw_irq_off(void)
{
	CMP_BASE->IER = 0;
	CMP_BASE->CSR = LPCMP_CSR_CFR_MASK | LPCMP_CSR_CFF_MASK;
}
static inline void hw_hyst(uint8_t h)
{
	CMP_BASE->CCR2 = (CMP_BASE->CCR2 & ~LPCMP_CCR2_HYSTCTR_MASK) | LPCMP_CCR2_HYSTCTR(h);
}
static void hw_clock_init(void)
{
	/* CMP1 has no AHB gate on this part (LPCMP_CLOCKS lists kCLOCK_None for
	 * CMP0/1); it needs its functional clock and its analog power. */
	CLOCK_SetClkDiv(kCLOCK_DivCmp1FClk, 1U);
	CLOCK_AttachClk(kFRO12M_to_CMP1F);
	SPC0->ACTIVE_CFG1 |= SPC_ACTIVE_CFG1_SOC_CNTRL((1UL << 17) | (1UL << 21)); /* CMP1, CMP1_DAC */
	k_busy_wait(50);
}
/* comparator configured (input, DAC as minus, hysteresis), disabled, no IRQ */
static void hw_config(uint8_t h)
{
	CMP_BASE->CCR0 = 0;
	CMP_BASE->IER = 0;
	CMP_BASE->CCR1 = 0;                   /* filter bypassed, no pin out */
	CMP_BASE->CCR2 = LPCMP_CCR2_CMP_HPMD(1) | LPCMP_CCR2_HYSTCTR(h) |
			 LPCMP_CCR2_PSEL(CMP_PSEL) | LPCMP_CCR2_MSEL(CMP_MSEL_DAC);
	CMP_BASE->DCR = 0;
}
static inline void hw_enable(void)
{
	CMP_BASE->CSR = LPCMP_CSR_CFR_MASK | LPCMP_CSR_CFF_MASK;
	CMP_BASE->CCR0 = LPCMP_CCR0_CMP_EN_MASK;
}
static void hw_off(void)
{
	CMP_BASE->IER = 0;
	CMP_BASE->CCR0 = 0;
	CMP_BASE->DCR = 0;
}

#elif defined(CONFIG_SOC_MIMXRT1062)
/* Teensy 4.0 / 4.1 (i.MX RT1062). Its comparators are the Kinetis-style CMP
 * (CR0/CR1/FPR/SCR/DACCR/MUXCR, 8-bit registers) with a 6-bit DAC. Nearly
 * every ADC pad is also a comparator input; the one wired here is Teensy A0
 * (T14) = GPIO_AD_B1_02 = ADC1_IN7 (ain ch 7 in boards/teensy4x.overlay) =
 * CMP1_IN3. On this board the analog pads are NOT box pins (box pins are
 * gpio2 bits), so the line needs a box pin of its own: box pin 15 =
 * GPIO_B0_15, which is not bonded out on either Teensy (its silk is "") --
 * the same number as the MCXN947's, so `pin 15 mode cmp` means the same
 * thing on both. Its pad is set disconnected in cmp mode like any cmp pin.
 * Other comparator-capable ain channels on the 4.0's edge pins, for later:
 * ch6 A4/T18 (CMPx_IN0), ch5 A5/T19 (CMP4_IN2), ch0 A7/T21 (CMP1_IN6). */
#define CMP_BOX_PIN    15
#define CMP_PIN_NAME   "A0 (T14)"
#define CMP_AIN_CH     7
#define CMP_DAC_LEVELS 64             /* VOSEL: Vin * (code + 1) / 64, ~52 mV a step */
#define CMP_MIN_STEP   192            /* three DAC steps: a midpoint off by <= 32 counts still sits well inside */
/* ASSUMED: VRSEL 1 = Vin2 = the 3.3 V supply, as on the Kinetis parts this
 * block comes from; the reference manual's chip-specific CMP section was not
 * at hand. A nonzero mismatch count says otherwise -- `cmp vref 0` tries Vin1. */
#define CMP_VREF_DEF   1
#define CMP_BASE       CMP1
#define CMP_PSEL       3u             /* IN3 */
#define CMP_MSEL_DAC   7u             /* IN7 = the internal 6-bit DAC */
#define CMP_IRQN       ACMP1_IRQn
#define CMP_IRQ_PRIO   DT_IRQ_BY_IDX(DT_NODELABEL(gpio2), 0, priority)  /* as DI edges */

static inline int hw_cout(void) { return (CMP_BASE->SCR & CMP_SCR_COUT_MASK) ? 1 : 0; }
/* SCR holds both the w1c edge flags and the interrupt enables: write back the
 * enables as they are, with 1s in the flags that were set */
static inline int hw_flags_take(void)
{
	uint8_t scr = CMP_BASE->SCR;

	CMP_BASE->SCR = (uint8_t) ((scr & (CMP_SCR_IER_MASK | CMP_SCR_IEF_MASK)) |
				   (scr & (CMP_SCR_CFR_MASK | CMP_SCR_CFF_MASK)));
	return (scr & CMP_SCR_COUT_MASK) ? 1 : 0;
}
static inline bool hw_dac_on(void) { return (CMP_BASE->DACCR & CMP_DACCR_DACEN_MASK) != 0; }
static inline void hw_dac_set(uint8_t code, uint8_t vref)
{
	CMP_BASE->DACCR = (uint8_t) (CMP_DACCR_DACEN_MASK | CMP_DACCR_VRSEL(vref) |
				     CMP_DACCR_VOSEL(code));
}
static inline void hw_irq_on(void)
{
	CMP_BASE->SCR = CMP_SCR_CFR_MASK | CMP_SCR_CFF_MASK | CMP_SCR_IER_MASK | CMP_SCR_IEF_MASK;
}
static inline void hw_irq_off(void)
{
	CMP_BASE->SCR = CMP_SCR_CFR_MASK | CMP_SCR_CFF_MASK;
}
static inline void hw_hyst(uint8_t h)
{
	CMP_BASE->CR0 = (uint8_t) ((CMP_BASE->CR0 & ~CMP_CR0_HYSTCTR_MASK) | CMP_CR0_HYSTCTR(h));
}
static void hw_clock_init(void)
{
	CLOCK_EnableClock(kCLOCK_Acmp1);      /* CCGR3 CG10: the IPG clock is all it needs */
	k_busy_wait(10);
}
static void hw_config(uint8_t h)
{
	CMP_BASE->CR1 = 0;                    /* off while it is set up */
	CMP_BASE->SCR = CMP_SCR_CFR_MASK | CMP_SCR_CFF_MASK;
	CMP_BASE->CR0 = (uint8_t) CMP_CR0_HYSTCTR(h);   /* FILTER_CNT 0: filter bypassed */
	CMP_BASE->FPR = 0;
	CMP_BASE->MUXCR = (uint8_t) (CMP_MUXCR_PSEL(CMP_PSEL) | CMP_MUXCR_MSEL(CMP_MSEL_DAC));
	CMP_BASE->DACCR = 0;
}
static inline void hw_enable(void)
{
	CMP_BASE->SCR = CMP_SCR_CFR_MASK | CMP_SCR_CFF_MASK;
	CMP_BASE->CR1 = CMP_CR1_EN_MASK | CMP_CR1_PMODE_MASK;   /* high-speed, no pin out */
}
static void hw_off(void)
{
	CMP_BASE->SCR = 0;
	CMP_BASE->CR1 = 0;
	CMP_BASE->DACCR = 0;
}
#endif

/* state/cmp's `dac` is an 8-bit-equivalent code, so the host's threshold
 * (dac + 1) * 4096 / 256 holds on every board */
#define CMP_DAC_TO_8BIT(c) ((uint8_t) (((c) + 1) * (256 / CMP_DAC_LEVELS) - 1))

/* ---- the plateau tracker (ADC counts, 12-bit) ----
 *
 * A PLATEAU is the signal still -- every sample within the band of the run's
 * level -- for TRK_SETTLE_US; its value is the run's mean. Learning needs
 * three: level A, then B at least TRK_MIN_STEP away, then A again (the RETURN
 * is what says A and B are the two states of the patch and not a hand passing
 * over). Readings in between the two levels are ignored throughout. */
#define TRK_BAND      32     /* "still": within this of the run's level (~26 mV) while learning */
#define TRK_SETTLE_US 3000   /* still this long = a plateau */
#define TRK_MIN_STEP  CMP_MIN_STEP  /* two levels this far apart = dark and light (per board, above) */
#define TRK_NEAR_DIV  3      /* a reading within step/3 of a level IS that level */
#define TRK_EMA_SHIFT 3      /* locked: a level moves 1/8 of the way to each reading of it */

enum cmp_state { ST_OFF = 0, ST_UNSUPPORTED, ST_NO_ADC, ST_LEARNING, ST_ARMED };
static const char *const st_name[] = { "off", "unsupported pin", "no samples", "learning", "armed" };
static const char *const st_word[] = { "off", "unsupported", "no_samples", "learning", "armed" };

static struct k_spinlock lock;
static enum cmp_state state;
static int      cmp_pin = -1;        /* box pin in cmp mode, -1 none */
static int      ain_ch = -1;         /* its ain channel */
static int      bad_pin = -1;        /* a cmp pin this board cannot do */
static bool     hw_up;

/* runtime knobs (console only, not persisted) */
static uint8_t  hyst = 2;            /* HYSTCTR level: 0/10/20/30 mV */
static uint8_t  vref_sel = CMP_VREF_DEF;  /* the DAC's reference select (per board, above) */
static int      dac_fixed = -1;      /* >= 0: fixed DAC code, armed at once */

/* tracker */
static bool     run_on;
static int32_t  run_ref;
static uint64_t run_t0;
static int32_t  run_sum, run_n;
static int      n_seen;              /* learning: 0 none, 1 = A, 2 = A then B */
static int32_t  lev_a, lev_b;        /* learning: first level, and the other one */
static int32_t  lev_lo, lev_hi;      /* armed: the levels the threshold was set from, then tracked */
static int32_t  thr_counts;          /* armed: the locked threshold, ADC counts */
static uint8_t  dac_code;
static int64_t  last_sample_ms = -1; /* uptime of the last scan that carried the channel */
static bool     relearn_req;         /* asked for; applied outside an obs */

/* counters */
static volatile uint32_t n_isr;
static uint32_t n_plateaus, n_mismatch, n_dac_writes, n_relearn;

static inline int cout_now(void)
{
	return hw_cout();
}

static void cmp_isr(const void *arg)
{
	ARG_UNUSED(arg);
	uint64_t t = box_gpio_now_us();       /* stamp first, like the GPIO ISR */
	int out = hw_flags_take();

	n_isr++;
	/* Level read AFTER the flags: two edges inside one ISR latency leave the
	 * output where it started, and box_gpio's FIFO turns "same level as
	 * published" into a same-stamp pair -- the GPIO path's rule, unchanged. */
	box_gpio_cmp_edge(cmp_pin, out, t);
}

/* ADC counts -> DAC code. The DAC gives Vref * (code + 1) / CMP_DAC_LEVELS,
 * and its reference is taken to be the ADC's 3.3 V full scale; the plateau
 * check counts disagreements (n_mismatch) if it is not -- 0 over a 90 s
 * MCXN947 bench run, 2026-11-24. */
static uint8_t counts_to_code(int32_t counts)
{
	int bits = box_adc_bits() ? box_adc_bits() : 12;
	int32_t code = (int32_t) (((int64_t) counts * CMP_DAC_LEVELS + (1 << (bits - 1))) >> bits) - 1;

	return (uint8_t) CLAMP(code, 0, CMP_DAC_LEVELS - 1);
}

static int32_t code_to_counts(uint8_t code)
{
	int bits = box_adc_bits() ? box_adc_bits() : 12;

	return (int32_t) (((int64_t) (code + 1) << bits) / CMP_DAC_LEVELS);
}

static void dac_write(uint8_t code)
{
	if (code == dac_code && hw_dac_on()) {
		return;
	}
	dac_code = code;
	hw_dac_set(code, vref_sel);
	n_dac_writes++;
}

static void irq_arm(void)
{
	k_busy_wait(5);                       /* let the DAC settle before trusting COUT */
	box_gpio_cmp_resync(cmp_pin, cout_now());
	hw_irq_on();                          /* clears the flags, then enables */
	state = ST_ARMED;
}

static void irq_disarm(void)
{
	hw_irq_off();
}

static void tracker_reset(void)
{
	run_on = false;
	run_sum = run_n = 0;
	n_seen = 0;
}

/* Back to learning: edges stop until a new threshold is locked. */
static void relearn_now(void)
{
	irq_disarm();
	tracker_reset();
	state = ST_LEARNING;
	n_relearn++;
	if (dac_fixed >= 0) {
		dac_write((uint8_t) dac_fixed);
		irq_arm();
	}
}

static void hw_init_once(void)
{
	if (hw_up) {
		return;
	}
	hw_clock_init();
	IRQ_CONNECT(CMP_IRQN, CMP_IRQ_PRIO, cmp_isr, NULL, 0);
	hw_up = true;
}

static void hw_start(void)
{
	hw_init_once();
	irq_disable(CMP_IRQN);
	hw_config(hyst);
	dac_code = 0;
	dac_write(CMP_DAC_LEVELS / 2);        /* parked mid-rail; no edges reported yet */
	hw_enable();
	irq_enable(CMP_IRQN);
	tracker_reset();
	relearn_req = false;
	state = ST_LEARNING;
	if (dac_fixed >= 0) {
		dac_write((uint8_t) dac_fixed);
		irq_arm();
	}
}

static void hw_stop(void)
{
	if (!hw_up) {
		return;
	}
	irq_disable(CMP_IRQN);
	hw_off();
}

/* the ain channel that feeds the comparator: fixed by the board, or the one
 * whose pad is the cmp box pin (MCXN947: pin 15 = A3 = ch 3) */
static int cmp_ain_ch(void)
{
	if (CMP_AIN_CH >= 0) {
		return CMP_AIN_CH < box_adc_channels() ? CMP_AIN_CH : -1;
	}
	for (uint8_t ch = 0; ch < AIN_MAX_CH; ch++) {
		if (box_adc_pin_of(ch) == CMP_BOX_PIN) {
			return ch;
		}
	}
	return -1;
}

uint8_t box_cmp_ain_mask(const box_config_t *c)
{
	if (!c || c->pin_mode[CMP_BOX_PIN] != PIN_MODE_CMP) {
		return 0;
	}
	int ch = cmp_ain_ch();

	return (ch >= 0 && ch < 8) ? (uint8_t) (1u << ch) : 0;
}

void box_cmp_apply(const box_config_t *c)
{
	int want = -1;

	bad_pin = -1;
	for (int i = 0; i < BOX_NPINS; i++) {
		if (c->pin_mode[i] != PIN_MODE_CMP) {
			continue;
		}
		if (i == CMP_BOX_PIN) {
			want = i;
		} else if (bad_pin < 0) {
			bad_pin = i;
		}
	}

	k_spinlock_key_t k = k_spin_lock(&lock);

	if (want == cmp_pin && state >= ST_NO_ADC) {
		k_spin_unlock(&lock, k);          /* unrelated config edit: keep the lock */
		return;
	}
	cmp_pin = want;
	if (want < 0) {
		hw_stop();
		state = bad_pin >= 0 ? ST_UNSUPPORTED : ST_OFF;
		ain_ch = -1;
		k_spin_unlock(&lock, k);
		return;
	}
	ain_ch = cmp_ain_ch();
	hw_start();
	k_spin_unlock(&lock, k);
}

int box_cmp_level(void)
{
	return (hw_up && cmp_pin >= 0) ? cout_now() : 0;
}

void box_cmp_relearn(void)
{
	k_spinlock_key_t k = k_spin_lock(&lock);

	relearn_req = (cmp_pin >= 0);      /* applied by the feed, outside an obs */
	k_spin_unlock(&lock, k);
}

/* Lock the threshold at the midpoint of lo..hi and arm. */
static void lock_at(int32_t lo, int32_t hi)
{
	lev_lo = lo;
	lev_hi = hi;
	dac_write(counts_to_code((lo + hi) / 2));
	thr_counts = code_to_counts(dac_code);
	irq_arm();
}

/* A plateau at level p. Learning: A, B, then A again locks the threshold.
 * Armed: check the comparator agrees, and track the levels for `drift` only. */
static void plateau(int32_t p)
{
	n_plateaus++;

	if (state == ST_ARMED) {
		if (dac_fixed >= 0) {
			return;
		}
		/* does the comparator agree with the ADC about which side p is on?
		 * A steady disagreement means the DAC's reference is not the ADC's. */
		if (abs(p - thr_counts) > TRK_MIN_STEP / 2 && (p > thr_counts) != cout_now()) {
			n_mismatch++;
		}
		int32_t near = (lev_hi - lev_lo) / TRK_NEAR_DIV;

		if (abs(p - lev_lo) <= near) {
			lev_lo += (p - lev_lo) >> TRK_EMA_SHIFT;
		} else if (abs(p - lev_hi) <= near) {
			lev_hi += (p - lev_hi) >> TRK_EMA_SHIFT;
		}
		/* anything else -- in between, or beyond both -- is not the patch */
		return;
	}

	/* learning */
	if (n_seen == 0) {
		lev_a = p;
		n_seen = 1;
		return;
	}
	if (n_seen == 1) {
		if (abs(p - lev_a) < TRK_MIN_STEP) {
			lev_a = p;                    /* still A, drifting */
		} else {
			lev_b = p;
			n_seen = 2;
		}
		return;
	}
	/* n_seen == 2: waiting for the return to A */
	int32_t step = abs(lev_b - lev_a);
	int32_t near = step / TRK_NEAR_DIV;

	if (abs(p - lev_a) <= near) {
		lock_at(MIN(p, lev_b), MAX(p, lev_b));
	} else if (abs(p - lev_b) <= near) {
		lev_b = p;                            /* still B */
	} else if ((p - lev_a) * (lev_b - lev_a) > 0 && abs(p - lev_a) > step) {
		lev_b = p;                            /* beyond B, away from A: B was a way-station */
	} else if (abs(p - lev_b) >= TRK_MIN_STEP) {
		/* in between, or beyond A: A is not coming back. It may have been a
		 * level from before the sensor was placed (a floating pin, a hand
		 * moving it), and waiting for it would leave the box learning
		 * forever. The last two levels are the pair now: B, then p, and a
		 * return to B locks. */
		lev_a = lev_b;
		lev_b = p;
	}
}

/* state/cmp/level: each NEW settled level, so the host sees the light in
 * counts without the stream -- one value per still period, about one per
 * patch change (photo_test reads it for dark/light/step). "New" = more than
 * TRK_BAND from the last one reported, so a level that stays put is
 * reported once and a fade's way-stations each replace the pending one.
 * Called with the lock held. */
static int32_t  lvl_last = INT32_MIN;  /* the newest level taken (reported or pending) */
static int32_t  lvl_val;
static uint64_t lvl_t_us;
static bool     lvl_due;
static int64_t  lvl_ms;

static void level_seen(int32_t p, uint64_t t_us)
{
	if (lvl_last != INT32_MIN && abs(p - lvl_last) <= TRK_BAND) {
		return;
	}
	lvl_last = p;
	lvl_val = p;
	lvl_t_us = t_us;                      /* when the light got there: the run's start */
	lvl_due = true;
}

int box_cmp_settled(int32_t *counts, uint64_t *t_us)
{
	int64_t now = k_uptime_get();

	if (!lvl_due || now - lvl_ms < 50) {   /* at most 20/s, the newest wins */
		return 0;
	}
	k_spinlock_key_t k = k_spin_lock(&lock);

	*counts = lvl_val;
	*t_us = lvl_t_us;
	lvl_due = false;
	k_spin_unlock(&lock, k);
	lvl_ms = now;
	return 1;
}

void box_cmp_feed(const int16_t *scan, uint8_t mask, uint64_t t_us)
{
	if (cmp_pin < 0) {
		return;
	}
	if (ain_ch < 0) {
		/* At boot box_cmp_apply() runs from box_gpio_apply_config(), BEFORE
		 * box_ain_init() brings the converter up, so a channel checked
		 * against box_adc_channels() (Teensy) resolved to none. Scans
		 * arriving means the converter is up: resolve it now. */
		k_spinlock_key_t k0 = k_spin_lock(&lock);

		if (cmp_pin >= 0 && ain_ch < 0) {
			ain_ch = cmp_ain_ch();
		}
		k_spin_unlock(&lock, k0);
	}
	if (ain_ch < 0 || !((mask >> ain_ch) & 1u)) {
		return;
	}

	k_spinlock_key_t k = k_spin_lock(&lock);

	if (cmp_pin < 0) {
		k_spin_unlock(&lock, k);
		return;
	}
	if (state == ST_NO_ADC) {
		state = ST_LEARNING;
	}
	last_sample_ms = k_uptime_get();
	if (relearn_req && !box_obs_active()) {
		relearn_req = false;
		relearn_now();
	}

	int32_t x = scan[ain_ch];
	/* armed, the band scales with the step, so ripple well inside it (a PWM
	 * backlight, room light) does not stop the drift readings */
	int32_t band = (state == ST_ARMED) ? MAX(TRK_BAND, (lev_hi - lev_lo) / 6) : TRK_BAND;

	if (!run_on || abs(x - run_ref) > band) {
		run_on = true;
		run_ref = x;
		run_t0 = t_us;
		run_sum = 0;
		run_n = 0;
	}
	run_sum += x;
	run_n++;
	if (t_us - run_t0 >= TRK_SETTLE_US && run_n > 0) {
		int32_t p = run_sum / run_n;

		plateau(p);
		level_seen(p, run_t0);
		run_ref = p;                      /* follow slow drift within a plateau */
		run_t0 = t_us;
		run_sum = 0;
		run_n = 0;
	}
	k_spin_unlock(&lock, k);
}

/* The state as reported. Learning with no scan of the channel in the last
 * half second is "no samples": with ain off (the sampler's master switch;
 * the cmp channel itself needs no group, box_cmp_ain_mask) box_cmp_feed()
 * is never called at all, so the feed cannot say so itself. */
static int eff_state(void)
{
	if (state == ST_LEARNING &&
	    (last_sample_ms < 0 || k_uptime_get() - last_sample_ms > 500)) {
		return ST_NO_ADC;
	}
	return (int) state;
}

/* How far the tracked levels' midpoint has moved from the locked threshold,
 * in % of the step (signed; + = the midpoint is now above the threshold). */
static int drift_pct(void)
{
	int32_t step = lev_hi - lev_lo;

	if (state != ST_ARMED || dac_fixed >= 0 || step <= 0) {
		return 0;
	}
	return (int) (((lev_lo + lev_hi) / 2 - thr_counts) * 100 / step);
}

/* what was last published as state/cmp */
static bool     pub_ever;
static int      pub_state = -1;
static int      pub_dac = -1;
static int      pub_drift;
static uint32_t pub_mismatch, pub_relearn;
static int64_t  pub_ms;

int box_cmp_status(char *buf, int sz, int force)
{
	int64_t now = k_uptime_get();
	int st = eff_state();
	int dac = (st == ST_ARMED) ? CMP_DAC_TO_8BIT(dac_code) : -1;
	int drift = drift_pct();
	bool due;

	if (!pub_ever && st == ST_OFF) {
		return 0;                     /* a box that never had a cmp pin stays silent */
	}
	/* The announce (a host that just (re)connected) gets the light's level
	 * again too: state/cmp/level is otherwise sent only when the light moves,
	 * so a restarted dserv would not have it until the next patch change. */
	if (force && lvl_last != INT32_MIN) {
		lvl_due = true;
		lvl_ms = 0;
	}
	if (force || st != pub_state || dac != pub_dac || n_mismatch != pub_mismatch ||
	    n_relearn != pub_relearn) {
		due = true;
	} else {
		/* drift is a slow thing: report a 5-point move, at most every 10 s */
		due = abs(drift - pub_drift) >= 5 && now - pub_ms >= 10000;
	}
	if (!due) {
		return 0;
	}

	k_spinlock_key_t k = k_spin_lock(&lock);
	int32_t lo, hi;

	if (st == ST_ARMED) {
		lo = lev_lo;
		hi = lev_hi;
	} else {
		lo = n_seen >= 2 ? MIN(lev_a, lev_b) : (n_seen ? lev_a : 0);
		hi = n_seen >= 2 ? MAX(lev_a, lev_b) : (n_seen ? lev_a : 0);
	}
	k_spin_unlock(&lock, k);

	if (st <= ST_UNSUPPORTED) {
		snprintf(buf, sz, "state %s", st_word[st]);
	} else {
		/* A 128-byte frame holds name + value <= 109, and a 16-char box name
		 * leaves ~77 here: the console-only knobs appear only when changed.
		 * The threshold is (dac + 1) * 4096 / 256 counts. */
		int n = snprintf(buf, sz, "state %s lo %d hi %d dac %u drift %d mismatch %u",
				 st_word[st], (int) lo, (int) hi, CMP_DAC_TO_8BIT(dac_code), drift, n_mismatch);

		if (n > 0 && n < sz && (vref_sel != 0 || hyst != 2)) {
			snprintf(buf + n, sz - n, " vref %u hyst %u", vref_sel, hyst);
		}
	}
	pub_ever = true;
	pub_state = st;
	pub_dac = dac;
	pub_drift = drift;
	pub_mismatch = n_mismatch;
	pub_relearn = n_relearn;
	pub_ms = now;
	return 1;
}

void box_cmp_console(const char *args)
{
	while (*args == ' ') {
		args++;
	}
	if (!strncmp(args, "hyst ", 5)) {
		hyst = (uint8_t) CLAMP(atoi(args + 5), 0, 3);
		if (hw_up && cmp_pin >= 0) {
			hw_hyst(hyst);
		}
	} else if (!strncmp(args, "vref ", 5)) {
		vref_sel = (uint8_t) (atoi(args + 5) ? 1 : 0);
		if (hw_up && cmp_pin >= 0) {
			uint8_t c = dac_code;

			dac_code = (uint8_t) ~c;  /* force the rewrite */
			dac_write(c);
		}
	} else if (!strcmp(args, "relearn")) {
		box_cmp_relearn();
		if (box_obs_active()) {
			box_console_printf("cmp: relearn at the end of this obs\n");
		}
		k_busy_wait(1000);                /* let the feed take it before showing */
	} else if (!strncmp(args, "dac ", 4)) {
		k_spinlock_key_t k = k_spin_lock(&lock);

		if (!strncmp(args + 4, "auto", 4)) {
			dac_fixed = -1;
			relearn_req = (cmp_pin >= 0);
		} else {
			dac_fixed = CLAMP(atoi(args + 4), 0, CMP_DAC_LEVELS - 1);
			if (hw_up && cmp_pin >= 0) {
				dac_write((uint8_t) dac_fixed);
				thr_counts = code_to_counts(dac_code);
				if (state != ST_ARMED) {
					irq_arm();
				}
			}
		}
		k_spin_unlock(&lock, k);
	} else if (*args) {
		box_console_printf("cmp: relearn | hyst <0-3> | vref <0|1> | dac <0-%d>|auto\n",
				   CMP_DAC_LEVELS - 1);
		return;
	}

	int st = eff_state();

	box_console_printf("cmp  %s  pin %d  ain ch %d%s%s\n", st_name[st], cmp_pin, ain_ch,
			   relearn_req ? "  (relearn pending)" : "",
			   bad_pin >= 0 ? "  (a cmp pin this board cannot do is set)" : "");
	if (bad_pin >= 0) {
		box_console_printf("     pin %d: only box pin %d (%s) reaches a comparator here\n",
				   bad_pin, CMP_BOX_PIN, CMP_PIN_NAME);
	}
	if (cmp_pin < 0) {
		return;
	}
	if (st == ST_ARMED) {
		box_console_printf("     locked at %d counts (dac %u%s)  levels lo %d hi %d  drift %d%%"
				   "  vref%u  hyst %u\n",
				   (int) code_to_counts(dac_code), dac_code,
				   dac_fixed >= 0 ? ", fixed" : "", (int) lev_lo, (int) lev_hi,
				   drift_pct(), vref_sel, hyst);
	} else {
		if (n_seen == 0) {
			box_console_printf("     learning: no level yet  vref%u  hyst %u\n", vref_sel, hyst);
		} else if (n_seen == 1) {
			box_console_printf("     learning: A %d, waiting for the other level"
					   "  vref%u  hyst %u\n", (int) lev_a, vref_sel, hyst);
		} else {
			box_console_printf("     learning: A %d, B %d, waiting for the return to A"
					   "  vref%u  hyst %u\n", (int) lev_a, (int) lev_b, vref_sel, hyst);
		}
	}
	box_console_printf("     out %d  edges %u  plateaus %u  dac writes %u  relearns %u  mismatches %u\n",
			   cout_now(), n_isr, n_plateaus, n_dac_writes, n_relearn, n_mismatch);
	if (st == ST_NO_ADC) {
		box_console_printf("     ain ch %d is not being sampled: `ain enable 1` so the "
				   "levels can be learned\n", ain_ch);
	}
}

#else  /* ---- no comparator on this board ---- */

void box_cmp_apply(const box_config_t *c) { ARG_UNUSED(c); }
uint8_t box_cmp_ain_mask(const box_config_t *c) { ARG_UNUSED(c); return 0; }
int  box_cmp_level(void) { return 0; }
void box_cmp_relearn(void) { }
void box_cmp_feed(const int16_t *scan, uint8_t mask, uint64_t t_us)
{
	ARG_UNUSED(scan); ARG_UNUSED(mask); ARG_UNUSED(t_us);
}
int box_cmp_status(char *buf, int sz, int force)
{
	ARG_UNUSED(buf); ARG_UNUSED(sz); ARG_UNUSED(force);
	return 0;
}
int box_cmp_settled(int32_t *counts, uint64_t *t_us)
{
	ARG_UNUSED(counts); ARG_UNUSED(t_us);
	return 0;
}
void box_cmp_console(const char *args)
{
	ARG_UNUSED(args);
	box_console_printf("cmp: no comparator support on this board (cmp pins act as ain)\n");
}

#endif
