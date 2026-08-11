/*
 * box_ain.c -- see box_ain.h.
 */
#include "box_ain.h"
#include "box_adc.h"

#include <zephyr/kernel.h>
#include <errno.h>
#include <string.h>

#if defined(BOX_HAVE_ADC)

/* COOPERATIVE -2 (+26): preemptible 2 still lost whole periods to the
 * cooperative workqueues (system + UDC run at -1 and are unpreemptible by
 * ANY preemptible thread) -- measured on the PTP Pi rig as occasional
 * 1-2 ms sweep gaps (`late` 1 per slow trial, 8 at the 2 kHz rung).
 * Cooperative -2 outranks both workqueues; blocking in adc_read() still
 * YIELDS (cooperative only bars preemption while runnable, and sweep
 * bodies are sub-ms), so the suspended-converter wedge safety and the
 * driver-preempts-on-wake property both survive -- in fact the driver's
 * completion now resumes us ahead of everything. If `late` persists at
 * -2, the thief is ISR/irq_lock time: see the gap ledger below. */
#define AIN_THREAD_PRIO   (-2)
/* 4096, not 1024. CONFIG_HW_STACK_PROTECTION would catch an overflow as a loud
 * fatal rather than silent corruption, so this is insurance, not a fix -- but
 * this thread calls down through adc_read into the SPI stack and 1 KB was a
 * guess. */
#define AIN_STACK_SIZE    4096
/* 64, not 8: a busy uplink now DEFERS block delivery (bounded by this
 * queue, ~4 KB) instead of shedding it; st_dropped stays as the honest
 * counter for the truly pathological case. Same directive as the lattice:
 * the record beats the deadline. */
#define AIN_QUEUE_DEPTH   64

K_THREAD_STACK_DEFINE(ain_stack, AIN_STACK_SIZE);
static struct k_thread ain_thread;
K_MSGQ_DEFINE(ain_q, sizeof(ain_block_t), AIN_QUEUE_DEPTH, 4);

/* Limit 1, deliberately. The sampler only ever needs to know THAT something
 * changed, never how many times -- a higher limit would just queue redundant
 * wakeups for a thread whose first action on waking is to re-read the world. */
K_SEM_DEFINE(ain_wake, 0, 1);

static const box_config_t *cfg;
static ain_group_rt_t rt[BOX_NAGROUPS];

static atomic_t generation;          /* bumped by box_ain_apply() */
static uint32_t applied_gen;

/* Wall-clock deadline for box_ain_hold(), in k_uptime_get_32() milliseconds.
 *
 * 32-BIT AND COMPARED BY DIFFERENCE, which is not laziness. atomic_t is 32 bits
 * on this target, so storing a k_uptime_get() (int64) ms value here would
 * silently truncate -- the exact defect that meant `at_abs` had never once
 * fired. A uint32 ms counter wraps every ~49.7 days, and `(int32_t)(until -
 * now) > 0` stays correct across that wrap; a plain `until > now` would not. */
static atomic_t hold_until_ms;

/* Is the sampler live? Written ONLY by the sampling thread, read by everyone.
 *
 * atomic_t rather than a plain uint8_t because box_ain_borrow() SPINS on it from
 * another thread. `running` is file-static and its address is never taken, so a
 * compiler is entitled to prove that k_msleep() cannot touch it and hoist the
 * load straight out of that loop -- turning a bounded wait into an infinite one,
 * at whatever optimisation level happened to notice. */
static atomic_t running;

/* Is the converter currently LENT to someone else (box_ain_borrow)?
 *
 * Needed because the hold alone is not enough, which cost a bench cycle to
 * learn. `hold` makes the thread not WANT the device -- but "not wanted" is
 * exactly the condition that makes it SUSPEND the device, so the borrower's
 * resume was being undone from under it and every sweep returned -EAGAIN
 * (`suspends` 2->3 while `resumes` 1->2, visible in ain/dbg). While lent, the
 * thread must touch the device in NEITHER direction.
 *
 * Cleared automatically when the hold expires, so a borrower that dies without
 * returning loses the converter for its stated duration and no longer -- the
 * same self-expiring property as the hold itself. */
static atomic_t borrowed;

static uint8_t  union_mask;          /* channels any active group wants */
static uint32_t period_us;

static uint32_t st_sweeps, st_blocks, st_dropped, st_late, st_throttled;
static uint32_t sat_run;   /* consecutive cadence syncs that never parked */
/* +26 stall ledger: how LONG each starvation was, not just that one
 * happened -- (fired-1)*period is the floor of the gap. A 1-2 ms gap
 * indicts thread-level theft; sub-period gaps never count; if gaps
 * persist at cooperative priority the thief is ISR/irq_lock time. */
static uint32_t st_late_gap_last_us, st_late_gap_max_us;
static uint32_t st_holds;
static int64_t  last_block_ms[BOX_NAGROUPS];

/* Milliseconds left on the current hold, 0 if none. */
static uint32_t hold_left_ms(void)
{
	uint32_t until = (uint32_t) atomic_get(&hold_until_ms);
	int32_t  left  = (int32_t) (until - k_uptime_get_32());

	return (left > 0) ? (uint32_t) left : 0u;
}

/* Ceiling on how often ONE group may publish.
 *
 * Not a preference -- a safety limit. In on-change mode the publish rate is set
 * by INPUT NOISE, not by config: a 12-bit joystick wanders well past a deadband
 * of 8, so at a 1 kHz base rate a resting stick can emit ~1000 blocks/s. Each is
 * a 128-byte frame costing 275-864 us of CPU to send (measured, see
 * CMakeLists.txt), i.e. 0.28-0.86 SECONDS of CPU per second: the service loop
 * saturates, the watchdog stops advancing and the net stack starves. That is not
 * a crash and it does not look like one -- box3 simply went silent on
 * 2026-07-28, and with no published stats there was nothing to see.
 *
 * Continuous mode is EXEMPT: its rate is declared by the operator (rate/
 * decimate/batch), not by noise, and batching is the intended tool for keeping
 * the frame rate affordable (2 channels @ 1 kHz, batch 12 -> ~83 frames/s).
 * Gating it too didn't cap -- it BEAT: a 250 Hz batch=1 eye feed (4 ms cadence)
 * against the 5 ms gate published every OTHER block, silently halving a
 * deliberate feed to 125 Hz (officepi, 2026-08-06). The ceiling binds only the
 * mode whose rate nobody chose.
 *
 * Blocks refused here are COUNTED (ain/dbg/throttled), never silently lost. */
#define AIN_MAX_BLOCKS_PER_S  200

/* Derive what the sampler needs from the live config.
 *
 * Reads cfg directly rather than holding a copy: box_config_t is ~1 kB and the
 * fields used here are single bytes/shorts, so a torn read is not a real hazard
 * on this core -- and box_ain_apply() resets the packing state anyway, so the
 * worst case is one malformed scan at the moment of an edit. Same approach the
 * RP2350 took reading g_cfg from core 0. */
static void recompute(void)
{
	union_mask = 0;
	for (int g = 0; g < BOX_NAGROUPS; g++) {
		union_mask |= cfg->ain_group_chans[g];
		ain_group_reset(&rt[g]);
	}
	/* Only sample channels the part actually has -- a mask bit for a channel
	 * that is not fitted would make every sweep fail with -EINVAL rather than
	 * quietly sampling fewer, which is right, but there is no reason to ask. */
	uint8_t have = box_adc_channels();
	if (have < 8) {
		union_mask &= (uint8_t) ((1u << have) - 1u);
	}

	/* ...and only channels that are ANALOG RIGHT NOW. A channel sharing a box
	 * pin is samplable only while that pin is in `ain` mode; sweeping it while
	 * the pad is muxed to GPIO would convert a disconnected input and publish
	 * the result as if it meant something. The block header carries mask and
	 * nchan, so a consumer sees exactly which channels it got. */
	union_mask &= box_adc_active_mask(cfg);

	/* ain_en is the documented master switch ("ain_en stays the master switch"
	 * -- dserv_config.h). Honour it: without this, `ain enable 0` left the
	 * sampler running, so there was no way to stop it short of clearing every
	 * group. */
	if (!cfg->ain_en) {
		union_mask = 0;
	}

	int rate = dserv_cfg_ain_rate(cfg);
	period_us = (uint32_t) (1000000 / (rate > 0 ? rate : 50));
}

static void ain_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	struct k_timer tick;
	k_timer_init(&tick, NULL, NULL);

	uint32_t running_period = 0;
	uint8_t  warmup = 0;

	while (1) {
		uint32_t gen = (uint32_t) atomic_get(&generation);

		if (gen != applied_gen) {
			applied_gen = gen;
			recompute();
			box_adc_set_oversample(cfg->ain_ovs);   /* v24 hw averaging */
			running_period = 0;       /* force the re-arm below */
		}

		uint32_t hold = hold_left_ms();

		/* The borrow outlived its hold: take the device back rather than
		 * leaving analog dead because a borrower never called return(). */
		if (!hold && atomic_get(&borrowed)) {
			atomic_set(&borrowed, 0);
		}

		if (atomic_get(&borrowed)) {
			/* LENT. Stop sampling, but do NOT suspend -- the borrower has
			 * the device powered for its own use, and suspending here is
			 * precisely the bug this flag exists to prevent. */
			if (atomic_get(&running)) {
				k_timer_stop(&tick);
				atomic_set(&running, 0);
				running_period = 0;
			}
			k_sem_take(&ain_wake, K_MSEC(hold));
			continue;
		}

		int want = box_adc_ready() && union_mask && !hold;

		/* ---- the only place the converter is switched ---- */
		if (want && !atomic_get(&running)) {
			int rc = box_adc_resume();

			if (rc != 0 && rc != -ENOTSUP) {
				/* Could not power it. Do NOT fall through and
				 * "sample" -- back off and retry, so the failure is a
				 * gap in the data rather than a stream of zeros that
				 * reads like a flat signal. */
				k_sem_take(&ain_wake, K_MSEC(500));
				continue;
			}
			/* Discard the first sweep after a resume. LPADC_Enable()
			 * re-powers the analog front end and its reference buffer,
			 * and whether the very first conversion after that is
			 * trustworthy is UNMEASURED -- `adccal` with the DAC->ADC
			 * jumper is how to settle it. One sample at >=50 Hz costs at
			 * most 20 ms, which is cheaper than carrying the question. */
			warmup = 1;
			atomic_set(&running, 1);
			running_period = 0;
		} else if (!want && (atomic_get(&running) || box_adc_powered())) {
			/* `|| powered` covers the boot case: box_adc_init() leaves the
			 * converter ACTIVE, so a box that comes up with analog disabled
			 * has a device to switch off even though the sampler never
			 * started. Without it the LPADC would sit enabled forever on
			 * exactly the boxes that asked for no analog at all. */
			k_timer_stop(&tick);
			(void) box_adc_suspend();   /* -ENOTSUP where there is no PM hook */
			atomic_set(&running, 0);
			running_period = 0;
		}

		if (!atomic_get(&running)) {
			/* IDLE MEANS IDLE. This used to keep arming the timer and
			 * waking every period to do nothing -- at ain_rate 1000 that
			 * was 1000 timer interrupts and 1000 wakeups per second on a
			 * box with analog switched OFF, against a 10 us system tick.
			 *
			 * Waiting on the hold rather than on a flag is what makes the
			 * hold self-expiring: nothing has to come along and release
			 * us, the timeout IS the release. */
			k_sem_take(&ain_wake, hold ? K_MSEC(hold) : K_FOREVER);
			continue;
		}

		if (running_period != period_us) {
			running_period = period_us;
			k_timer_start(&tick, K_USEC(running_period),
				      K_USEC(running_period));
		}

		/* Drift-free cadence: k_timer tracks ABSOLUTE periods, so a slow
		 * sweep steals from the next interval instead of pushing every
		 * later sample out -- k_sleep(period) would accumulate the sweep
		 * duration forever. The return value is expirations since the last
		 * call, so >1 means we did not keep up; count it rather than
		 * pretending the cadence held. */
		uint32_t sync_cyc = k_cycle_get_32();
		uint32_t fired = k_timer_status_sync(&tick);

		if (fired > 1) {
			st_late += (fired - 1);
			st_late_gap_last_us = (fired - 1) * running_period;
			if (st_late_gap_last_us > st_late_gap_max_us) {
				st_late_gap_max_us = st_late_gap_last_us;
			}
		}

		/* SATURATION GUARD (2026-08-11, the wedged bench box). This thread
		 * is COOPERATIVE: once one sweep costs >= the period, status_sync
		 * returns WITHOUT BLOCKING on every pass -- often with fired == 1,
		 * so the late counter alone cannot see it -- and a -2 thread that
		 * never blocks owns the machine outright: console, net, the service
		 * loop and every preemptible thread starve, only ISRs run, and the
		 * box seizes (10 kHz x 6-ch LPADC, +59 trial boot; recovered only
		 * by power-cycle + MCUboot revert). A sync that returns in under
		 * 2 us never parked (a parked one costs a context switch; the only
		 * false hit is arriving within 2 us of the period boundary, which
		 * is saturation to the tolerance that matters). After 4 in a row,
		 * sleep one period: the rest of the system keeps ~20% of the CPU,
		 * sampling degrades to "as fast as possible, still breathing", and
		 * st_late says so. A paced sampler parks every pass and never takes
		 * this branch. */
		if (k_cyc_to_us_floor32(k_cycle_get_32() - sync_cyc) < 2) {
			if (fired <= 1) {
				st_late++;      /* exact saturation: cadence missed */
			}
			if (++sat_run >= 4) {
				sat_run = 0;
				k_sleep(K_USEC(running_period ? running_period : 100));
			}
		} else {
			sat_run = 0;
		}

		/* A hold or a config edit can land while we are parked in
		 * status_sync above. Re-check before touching the device: the
		 * suspend/resume decision is made at the top of the loop, so going
		 * on to sweep here would read a converter we are about to switch
		 * off -- and on a suspended LPADC that call never returns. */
		if (hold_left_ms() || (uint32_t) atomic_get(&generation) != applied_gen) {
			continue;
		}

		uint16_t raw[AIN_MAX_CH];
		uint64_t t_us = 0;
		int n = box_adc_sweep(union_mask, raw, AIN_MAX_CH, &t_us);

		if (n <= 0) {
			continue;
		}
		if (warmup) {
			warmup = 0;
			continue;
		}
		st_sweeps++;

		/* box_adc packs densely (mask order); the group machine wants a scan
		 * indexed BY CHANNEL. Scatter once here so every group downstream
		 * can use plain channel numbers. */
		int16_t scan[AIN_MAX_CH];
		memset(scan, 0, sizeof scan);
		int i = 0;
		for (int ch = 0; ch < AIN_MAX_CH && i < n; ch++) {
			if (union_mask & BIT(ch)) {
				scan[ch] = (int16_t) raw[i++];
			}
		}

		for (int g = 0; g < BOX_NAGROUPS; g++) {
			ain_block_t blk;

			if (ain_group_feed(&rt[g], cfg, g, union_mask, scan, t_us,
					   running_period, &blk)) {
				int64_t now_ms = k_uptime_get();

				if (!cfg->ain_group_mode[g] &&
				    now_ms - last_block_ms[g] < (1000 / AIN_MAX_BLOCKS_PER_S)) {
					st_throttled++;
					continue;
				}
				last_block_ms[g] = now_ms;
				if (k_msgq_put(&ain_q, &blk, K_NO_WAIT) != 0) {
					/* The service loop is not draining. Drop the
					 * NEWEST rather than block: sampling must not
					 * stall behind the uplink, and a stalled
					 * sampler would corrupt the cadence for every
					 * group, not just this one. */
					st_dropped++;
				} else {
					st_blocks++;
				}
			}
		}
	}
}

int box_ain_init(const box_config_t *c)
{
	if (!c) {
		return -EINVAL;
	}
	cfg = c;

	int rc = box_adc_init();

	if (rc != 0) {
		return rc;                    /* -ENODEV = no ADC fitted; not an error */
	}

	recompute();
	box_adc_set_oversample(cfg->ain_ovs);   /* persisted value, before any sweep */
	atomic_set(&generation, 1);
	applied_gen = 1;

	k_thread_create(&ain_thread, ain_stack, K_THREAD_STACK_SIZEOF(ain_stack),
			ain_thread_fn, NULL, NULL, NULL,
			AIN_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&ain_thread, "extio_ain");
	return 0;
}

void box_ain_apply(void)
{
	atomic_inc(&generation);
	/* Wake a STOPPED sampler. Bumping the generation alone was enough while the
	 * thread ticked forever; now that it parks on K_FOREVER when there is
	 * nothing to sample, `ain enable 1` would set a counter nobody was awake to
	 * read -- and the box would look exactly like one that needs a reboot. */
	k_sem_give(&ain_wake);
}

void box_ain_hold(uint32_t ms)
{
	uint32_t until = k_uptime_get_32() + ms;
	uint32_t cur   = (uint32_t) atomic_get(&hold_until_ms);

	/* EXTEND ONLY, wrap-safely. Two callers holding for different durations
	 * must not be able to cut each other short -- the shorter one would
	 * release the converter back into the middle of the longer one's work. */
	if (hold_left_ms() == 0 || (int32_t) (until - cur) > 0) {
		atomic_set(&hold_until_ms, (atomic_val_t) until);
		st_holds++;
	}
}

/* Generous on purpose. The sampler can be parked in k_timer_status_sync for a
 * full period, so this has to exceed 1/ain_rate at the slowest rate anyone would
 * set. 1.5 s covers everything down to ~1 Hz, and it is paid once by a command a
 * human typed -- against the alternative of reporting -EBUSY on a box that was
 * merely sampling slowly. */
#define AIN_BORROW_WAIT_MS  1500

int box_ain_borrow(uint32_t ms)
{
	if (!box_adc_ready()) {
		return -ENODEV;
	}
	/* ORDER MATTERS: claim it BEFORE waking the thread, or the thread wakes,
	 * sees an unwanted device and suspends it out from under us. */
	atomic_set(&borrowed, 1);
	box_ain_hold(ms);
	k_sem_give(&ain_wake);

	for (int i = 0; i < AIN_BORROW_WAIT_MS && atomic_get(&running); i++) {
		k_msleep(1);
	}
	if (atomic_get(&running)) {
		atomic_set(&borrowed, 0);
		return -EBUSY;         /* caller must not touch the device */
	}
	/* Ours now, and the sampler will not come back for it while the hold
	 * stands -- so no lock is needed, only this ordering. */
	int rc = box_adc_resume();

	if (rc != 0 && rc != -ENOTSUP) {
		atomic_set(&borrowed, 0);
		box_ain_return();
		return rc;
	}
	return 0;
}

void box_ain_return(void)
{
	/* Expire the hold NOW rather than suspending here. Whether the converter
	 * should end up on or off is the sampler's decision from the live config,
	 * and duplicating that judgement in every borrower is how the two drift
	 * apart. */
	atomic_set(&borrowed, 0);
	atomic_set(&hold_until_ms, (atomic_val_t) k_uptime_get_32());
	k_sem_give(&ain_wake);
}

int      box_ain_running(void) { return (int) atomic_get(&running); }
uint32_t box_ain_holds(void)   { return st_holds; }

int box_ain_pop(ain_block_t *out)
{
	if (!out) {
		return 0;
	}
	return k_msgq_get(&ain_q, out, K_NO_WAIT) == 0 ? 1 : 0;
}

void box_ain_stats(uint32_t *sweeps, uint32_t *blocks, uint32_t *dropped,
		   uint32_t *late, uint32_t *throttled)
{
	if (sweeps)    *sweeps    = st_sweeps;
	if (blocks)    *blocks    = st_blocks;
	if (dropped)   *dropped   = st_dropped;
	if (late)      *late      = st_late;
	if (throttled) *throttled = st_throttled;
}

void box_ain_late_gaps(uint32_t *last_us, uint32_t *max_us)
{
	if (last_us) *last_us = st_late_gap_last_us;
	if (max_us)  *max_us  = st_late_gap_max_us;
}

void box_ain_stats_reset(void)
{
	st_sweeps = st_blocks = st_dropped = st_late = st_throttled = 0;
	/* The gaps are part of the late story -- leaving them behind would report
	 * a 32 ms worst gap against a late count of zero. */
	st_late_gap_last_us = st_late_gap_max_us = 0;
}

#else  /* no ADC on this board */

int  box_ain_init(const box_config_t *c) { ARG_UNUSED(c); return -ENODEV; }
void box_ain_apply(void) { }
void box_ain_hold(uint32_t ms) { ARG_UNUSED(ms); }
int  box_ain_borrow(uint32_t ms) { ARG_UNUSED(ms); return -ENODEV; }
void box_ain_return(void) { }
int  box_ain_running(void) { return 0; }
uint32_t box_ain_holds(void) { return 0; }
int  box_ain_pop(ain_block_t *out) { ARG_UNUSED(out); return 0; }
void box_ain_stats(uint32_t *s, uint32_t *b, uint32_t *d, uint32_t *l, uint32_t *t)
{ if (s) *s = 0; if (b) *b = 0; if (d) *d = 0; if (l) *l = 0; if (t) *t = 0; }
void box_ain_late_gaps(uint32_t *last_us, uint32_t *max_us)
{
	if (last_us) *last_us = st_late_gap_last_us;
	if (max_us)  *max_us  = st_late_gap_max_us;
}

void box_ain_stats_reset(void) { }

#endif
