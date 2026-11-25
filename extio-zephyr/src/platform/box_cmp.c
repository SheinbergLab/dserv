/*
 * box_cmp.c -- on-chip comparator as a light detector. See box_cmp.h.
 *
 * PROTOTYPE (2026-11-24). What is assumed rather than measured is marked
 * ASSUMED below; `cmp` on the console shows the numbers needed to check it.
 */
#include "box_cmp.h"
#include "box_gpio.h"
#include "box_console.h"

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_SOC_MCXN947) && defined(BOX_HAVE_ADC)

#include "box_adc.h"
#include <fsl_clock.h>

/* ---- the wiring: which box pin reaches which comparator input ----
 *
 * FRDM-MCXN947, Arduino A3 = P0_22, which is both ADC0_A14 (ain ch3) and
 * CMP1_IN2, in the same pad mode (MUX 0). A5 = P0_23 is CMP2_IN2 but is also
 * SW2 with a pull-up and is MCUboot's button, so it is not offered. Box pin 15
 * is A3 in this board's box-gpios map (boards/frdm_mcxn947_mcxn947_cpu0.overlay). */
#define CMP_BOX_PIN   15
#define CMP_BASE      CMP1
#define CMP_PSEL      2u              /* IN2 */
#define CMP_MSEL_DAC  7u              /* minus input = the internal 8-bit DAC */
#define CMP_IRQN      HSCMP1_IRQn     /* CMP1's vector, named HSCMP1 in the SDK */
#define CMP_IRQ_PRIO  DT_IRQ(DT_NODELABEL(gpio0), priority)  /* same as DI edges */

/* ---- the plateau tracker (ADC counts, 12-bit) ---- */
#define TRK_BAND      32     /* a sample within this of the run's level is "still" (~26 mV) */
#define TRK_SETTLE_US 3000   /* still this long = a plateau */
#define TRK_MIN_STEP  100    /* two plateaus this far apart = dark and light (~80 mV) */

enum cmp_state { ST_OFF = 0, ST_UNSUPPORTED, ST_NO_ADC, ST_LEARNING, ST_ARMED };
static const char *const st_name[] = { "off", "unsupported pin", "no samples", "learning", "armed" };

static struct k_spinlock lock;
static enum cmp_state state;
static int      cmp_pin = -1;        /* box pin in cmp mode, -1 none */
static int      ain_ch = -1;         /* its ain channel */
static int      bad_pin = -1;        /* a cmp pin this board cannot do */
static bool     hw_up;

/* runtime knobs (console only, not persisted) */
static uint8_t  hyst = 2;            /* HYSTCTR level: 0/10/20/30 mV */
static uint8_t  vref_sel;            /* DCR.VRSEL: VREFH0 / VREFH1 -- ASSUMED 0 = 3.3 V */
static int      dac_fixed = -1;      /* >= 0: fixed DAC code, armed at once */

/* tracker */
static bool     run_on;
static int32_t  run_ref;
static uint64_t run_t0;
static int32_t  run_sum, run_n;
static bool     have_a, have_pair;
static int32_t  lev_a, lev_lo, lev_hi;
static uint8_t  dac_code;
static uint64_t last_sample_us;
static int64_t  last_sample_ms = -1;  /* uptime of the last scan that carried the channel */

/* counters */
static volatile uint32_t n_isr;
static uint32_t n_plateaus, n_mismatch, n_dac_writes;

static inline int cout_now(void)
{
	return (CMP_BASE->CSR & LPCMP_CSR_COUT_MASK) ? 1 : 0;
}

static void cmp_isr(const void *arg)
{
	ARG_UNUSED(arg);
	uint64_t t = box_gpio_now_us();       /* stamp first, like the GPIO ISR */
	uint32_t csr = CMP_BASE->CSR;

	CMP_BASE->CSR = csr & (LPCMP_CSR_CFR_MASK | LPCMP_CSR_CFF_MASK);   /* w1c */
	n_isr++;
	/* Level read AFTER the flags: two edges inside one ISR latency leave the
	 * output where it started, and box_gpio's FIFO turns "same level as
	 * published" into a same-stamp pair -- the GPIO path's rule, unchanged. */
	box_gpio_cmp_edge(cmp_pin, (csr & LPCMP_CSR_COUT_MASK) ? 1 : 0, t);
}

/* ADC counts -> DAC code. The LPCMP DAC gives Vref * (code + 1) / 256.
 * ASSUMED: the DAC's reference (VREFH0) is the ADC's 3.3 V full scale. The
 * plateau check below counts disagreements (n_mismatch) if it is not. */
static uint8_t counts_to_code(int32_t counts)
{
	int bits = box_adc_bits() ? box_adc_bits() : 12;
	int32_t code = (int32_t) (((int64_t) counts * 256 + (1 << (bits - 1))) >> bits) - 1;

	return (uint8_t) CLAMP(code, 0, 255);
}

static int32_t code_to_counts(uint8_t code)
{
	int bits = box_adc_bits() ? box_adc_bits() : 12;

	return (int32_t) (((int64_t) (code + 1) << bits) / 256);
}

static void dac_write(uint8_t code)
{
	if (code == dac_code && (CMP_BASE->DCR & LPCMP_DCR_DAC_EN_MASK)) {
		return;
	}
	dac_code = code;
	CMP_BASE->DCR = LPCMP_DCR_VRSEL(vref_sel) | LPCMP_DCR_DAC_DATA(code) |
			LPCMP_DCR_DAC_EN_MASK;
	n_dac_writes++;
}

static void irq_arm(void)
{
	k_busy_wait(5);                       /* let the DAC settle before trusting COUT */
	CMP_BASE->CSR = LPCMP_CSR_CFR_MASK | LPCMP_CSR_CFF_MASK;
	box_gpio_cmp_resync(cmp_pin, cout_now());
	CMP_BASE->IER = LPCMP_IER_CFR_IE_MASK | LPCMP_IER_CFF_IE_MASK;
	state = ST_ARMED;
}

static void irq_disarm(void)
{
	CMP_BASE->IER = 0;
	CMP_BASE->CSR = LPCMP_CSR_CFR_MASK | LPCMP_CSR_CFF_MASK;
}

static void tracker_reset(void)
{
	run_on = false;
	have_a = have_pair = false;
	run_sum = run_n = 0;
}

static void hw_init_once(void)
{
	if (hw_up) {
		return;
	}
	/* CMP1 has no AHB gate on this part (LPCMP_CLOCKS lists kCLOCK_None for
	 * CMP0/1); it needs its functional clock and its analog power. */
	CLOCK_SetClkDiv(kCLOCK_DivCmp1FClk, 1U);
	CLOCK_AttachClk(kFRO12M_to_CMP1F);
	SPC0->ACTIVE_CFG1 |= SPC_ACTIVE_CFG1_SOC_CNTRL((1UL << 17) | (1UL << 21)); /* CMP1, CMP1_DAC */
	k_busy_wait(50);

	IRQ_CONNECT(CMP_IRQN, CMP_IRQ_PRIO, cmp_isr, NULL, 0);
	hw_up = true;
}

static void hw_start(void)
{
	hw_init_once();
	irq_disable(CMP_IRQN);
	CMP_BASE->CCR0 = 0;
	CMP_BASE->IER = 0;
	CMP_BASE->CCR1 = 0;                   /* filter bypassed, no pin out */
	CMP_BASE->CCR2 = LPCMP_CCR2_CMP_HPMD(1) | LPCMP_CCR2_HYSTCTR(hyst) |
			 LPCMP_CCR2_PSEL(CMP_PSEL) | LPCMP_CCR2_MSEL(CMP_MSEL_DAC);
	dac_code = 0;
	CMP_BASE->DCR = 0;
	dac_write(128);                       /* parked mid-rail; no edges reported yet */
	CMP_BASE->CSR = LPCMP_CSR_CFR_MASK | LPCMP_CSR_CFF_MASK;
	CMP_BASE->CCR0 = LPCMP_CCR0_CMP_EN_MASK;
	irq_enable(CMP_IRQN);
	tracker_reset();
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
	CMP_BASE->IER = 0;
	CMP_BASE->CCR0 = 0;
	CMP_BASE->DCR = 0;
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
		k_spin_unlock(&lock, k);          /* unrelated config edit: keep what was learned */
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
	ain_ch = -1;
	for (uint8_t ch = 0; ch < AIN_MAX_CH; ch++) {
		if (box_adc_pin_of(ch) == want) {
			ain_ch = ch;
			break;
		}
	}
	hw_start();
	k_spin_unlock(&lock, k);
}

int box_cmp_level(void)
{
	return (hw_up && cmp_pin >= 0) ? cout_now() : 0;
}

/* A plateau at level p: learn dark/light, check the comparator agrees, and
 * move the DAC to the midpoint. Runs only while the signal is still. */
static void plateau(int32_t p)
{
	n_plateaus++;

	if (!have_pair) {
		if (!have_a) {
			lev_a = p;
			have_a = true;
			return;
		}
		if (abs(p - lev_a) < TRK_MIN_STEP) {
			lev_a = p;                /* same level, drifting */
			return;
		}
		lev_lo = MIN(p, lev_a);
		lev_hi = MAX(p, lev_a);
		have_pair = true;
	} else {
		/* does the comparator agree with the ADC about which side p is on?
		 * A steady disagreement means the DAC's reference is not the ADC's. */
		if (state == ST_ARMED &&
		    abs(p - code_to_counts(dac_code)) > TRK_MIN_STEP / 2 &&
		    (p > code_to_counts(dac_code)) != cout_now()) {
			n_mismatch++;
		}
		if (p >= (lev_lo + lev_hi) / 2) {
			lev_hi = p;
		} else {
			lev_lo = p;
		}
		if (lev_hi - lev_lo < TRK_MIN_STEP) {
			/* the two levels met: nothing to separate any more */
			have_pair = false;
			lev_a = p;
			if (dac_fixed < 0) {
				irq_disarm();
				state = ST_LEARNING;
			}
			return;
		}
	}
	if (dac_fixed >= 0) {
		return;                           /* learning still runs, for the console */
	}
	dac_write(counts_to_code((lev_lo + lev_hi) / 2));
	if (state != ST_ARMED) {
		irq_arm();
	}
}

void box_cmp_feed(const int16_t *scan, uint8_t mask, uint64_t t_us)
{
	if (cmp_pin < 0 || ain_ch < 0) {
		return;
	}
	if (!((mask >> ain_ch) & 1u)) {
		if (state == ST_LEARNING) {
			state = ST_NO_ADC;
		}
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
	last_sample_us = t_us;
	last_sample_ms = k_uptime_get();

	int32_t x = scan[ain_ch];

	if (!run_on || abs(x - run_ref) > TRK_BAND) {
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
		run_ref = p;                      /* follow slow drift within a plateau */
		run_t0 = t_us;
		run_sum = 0;
		run_n = 0;
	}
	k_spin_unlock(&lock, k);
}

/* The state as reported. Learning with no scan of the channel in the last
 * half second is "no samples": with ain off, or the channel in no group,
 * box_cmp_feed() is never called at all, so the feed cannot say so itself. */
static int eff_state(void)
{
	if (state == ST_LEARNING &&
	    (last_sample_ms < 0 || k_uptime_get() - last_sample_ms > 500)) {
		return ST_NO_ADC;
	}
	return (int) state;
}

/* what was last published as state/cmp */
static bool     pub_ever;
static int      pub_state = -1;
static int      pub_dac = -1;
static uint32_t pub_mismatch;
static int64_t  pub_ms;

int box_cmp_status(char *buf, int sz, int force)
{
	int64_t now = k_uptime_get();
	int st = eff_state();
	int dac = (st == ST_ARMED) ? dac_code : -1;
	bool due;

	if (force) {
		due = pub_ever || st != ST_OFF;
	} else if (st != pub_state || n_mismatch != pub_mismatch) {
		due = pub_ever || st != ST_OFF;   /* a box that never had a cmp pin stays silent */
	} else if (dac != pub_dac && now - pub_ms >= 250) {
		due = abs(dac - pub_dac) >= 2 || now - pub_ms >= 10000;
	} else {
		due = false;
	}
	if (!due) {
		return 0;
	}

	k_spinlock_key_t k = k_spin_lock(&lock);
	bool pair = have_pair;
	int32_t lo = pair ? lev_lo : (have_a ? lev_a : 0);
	int32_t hi = pair ? lev_hi : (have_a ? lev_a : 0);
	k_spin_unlock(&lock, k);

	if (st <= ST_UNSUPPORTED) {
		snprintf(buf, sz, "state %s", st == ST_OFF ? "off" : "unsupported");
	} else {
		/* No `thr`: it is (dac + 1) * 4096 / 256 counts, and a 128-byte
		 * frame holds name + value <= 109 -- a 16-char box name leaves
		 * ~77 here. */
		snprintf(buf, sz, "state %s lo %d hi %d dac %u vref %u hyst %u mismatch %u",
			 st == ST_ARMED ? "armed" : st == ST_NO_ADC ? "no_samples" : "learning",
			 (int) lo, (int) hi, dac_code, vref_sel, hyst, n_mismatch);
	}
	pub_ever = true;
	pub_state = st;
	pub_dac = dac;
	pub_mismatch = n_mismatch;
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
			CMP_BASE->CCR2 = (CMP_BASE->CCR2 & ~LPCMP_CCR2_HYSTCTR_MASK) |
					 LPCMP_CCR2_HYSTCTR(hyst);
		}
	} else if (!strncmp(args, "vref ", 5)) {
		vref_sel = (uint8_t) (atoi(args + 5) ? 1 : 0);
		if (hw_up && cmp_pin >= 0) {
			uint8_t c = dac_code;

			dac_code = (uint8_t) ~c;  /* force the rewrite */
			dac_write(c);
		}
	} else if (!strncmp(args, "dac ", 4)) {
		k_spinlock_key_t k = k_spin_lock(&lock);

		if (!strncmp(args + 4, "auto", 4)) {
			dac_fixed = -1;
			if (hw_up && cmp_pin >= 0) {
				irq_disarm();
				hw_start();       /* relearn from scratch */
			}
		} else {
			dac_fixed = CLAMP(atoi(args + 4), 0, 255);
			if (hw_up && cmp_pin >= 0) {
				dac_write((uint8_t) dac_fixed);
				if (state != ST_ARMED) {
					irq_arm();
				}
			}
		}
		k_spin_unlock(&lock, k);
	} else if (*args) {
		box_console_printf("cmp: hyst <0-3> | vref <0|1> | dac <0-255>|auto\n");
		return;
	}

	int32_t thr = code_to_counts(dac_code);

	box_console_printf("cmp  %s  pin %d  ain ch %d%s\n", st_name[eff_state()], cmp_pin, ain_ch,
			   bad_pin >= 0 ? "  (a cmp pin this board cannot do is set)" : "");
	if (bad_pin >= 0) {
		box_console_printf("     pin %d: only box pin %d (A3) reaches a comparator here\n",
				   bad_pin, CMP_BOX_PIN);
	}
	if (cmp_pin < 0) {
		return;
	}
	box_console_printf("     levels %s lo %d hi %d (counts)  threshold %d  dac %u%s  vref%u  hyst %u\n",
			   have_pair ? "learned" : (have_a ? "one" : "none"),
			   (int) (have_pair ? lev_lo : lev_a), (int) (have_pair ? lev_hi : lev_a),
			   (int) thr, dac_code, dac_fixed >= 0 ? " (fixed)" : "", vref_sel, hyst);
	box_console_printf("     out %d  edges %u  plateaus %u  dac writes %u  mismatches %u\n",
			   cout_now(), n_isr, n_plateaus, n_dac_writes, n_mismatch);
	if (eff_state() == ST_NO_ADC) {
		box_console_printf("     ain ch %d is in no active ain group: add it so the "
				   "threshold can follow the signal\n", ain_ch);
	}
}

#else  /* ---- no comparator on this board ---- */

void box_cmp_apply(const box_config_t *c) { ARG_UNUSED(c); }
int  box_cmp_level(void) { return 0; }
void box_cmp_feed(const int16_t *scan, uint8_t mask, uint64_t t_us)
{
	ARG_UNUSED(scan); ARG_UNUSED(mask); ARG_UNUSED(t_us);
}
int box_cmp_status(char *buf, int sz, int force)
{
	ARG_UNUSED(buf); ARG_UNUSED(sz); ARG_UNUSED(force);
	return 0;
}
void box_cmp_console(const char *args)
{
	ARG_UNUSED(args);
	box_console_printf("cmp: no comparator support on this board (cmp pins act as ain)\n");
}

#endif
