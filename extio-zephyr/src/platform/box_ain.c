/*
 * box_ain.c -- see box_ain.h.
 */
#include "box_ain.h"
#include "box_adc.h"

#include <zephyr/kernel.h>
#include <errno.h>
#include <string.h>

#if defined(BOX_HAVE_ADC)

/* Priority 2: below the service loop (MAIN_THREAD_PRIORITY=0) so a sweep can
 * never delay obs/DI work, and below the mcp320x acquisition thread (pinned to 1
 * in boards/frdm_rw612.conf) so that when we block in adc_read() the driver
 * thread preempts us immediately instead of waiting for a scheduling point.
 * Ordering these three deliberately is the whole reason the ADC thread's default
 * of 0 -- identical to the service loop -- was worth overriding. */
#define AIN_THREAD_PRIO   2
/* 4096, not 1024. CONFIG_HW_STACK_PROTECTION would catch an overflow as a loud
 * fatal rather than silent corruption, so this is insurance, not a fix -- but
 * this thread calls down through adc_read into the SPI stack and 1 KB was a
 * guess. */
#define AIN_STACK_SIZE    4096
#define AIN_QUEUE_DEPTH   8

K_THREAD_STACK_DEFINE(ain_stack, AIN_STACK_SIZE);
static struct k_thread ain_thread;
K_MSGQ_DEFINE(ain_q, sizeof(ain_block_t), AIN_QUEUE_DEPTH, 4);

static const box_config_t *cfg;
static ain_group_rt_t rt[BOX_NAGROUPS];

static atomic_t generation;          /* bumped by box_ain_apply() */
static uint32_t applied_gen;

static uint8_t  union_mask;          /* channels any active group wants */
static uint32_t period_us;

static uint32_t st_sweeps, st_blocks, st_dropped, st_late, st_throttled;
static int64_t  last_block_ms[BOX_NAGROUPS];

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
 * Continuous mode does not need this: batching already packs many scans into one
 * frame, which is what makes a 1 kHz eye feed affordable (2 channels, batch 12 ->
 * ~83 frames/s). This bounds the mode that has no such control.
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

	/* mcp_en is the documented master switch ("mcp_en stays the master switch"
	 * -- dserv_config.h). Honour it: without this, `mcp enable 0` left the
	 * sampler running, so there was no way to stop it short of clearing every
	 * group. */
	if (!cfg->mcp_en) {
		union_mask = 0;
	}

	int rate = dserv_cfg_mcp_rate(cfg);
	period_us = (uint32_t) (1000000 / (rate > 0 ? rate : 50));
}

static void ain_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	struct k_timer tick;
	k_timer_init(&tick, NULL, NULL);

	uint32_t running_period = 0;

	while (1) {
		uint32_t gen = (uint32_t) atomic_get(&generation);

		if (gen != applied_gen || running_period == 0) {
			applied_gen = gen;
			recompute();
			if (period_us != running_period) {
				running_period = period_us;
				k_timer_start(&tick, K_USEC(running_period),
					      K_USEC(running_period));
			}
		}

		/* Drift-free cadence: k_timer tracks ABSOLUTE periods, so a slow
		 * sweep steals from the next interval instead of pushing every
		 * later sample out -- k_sleep(period) would accumulate the sweep
		 * duration forever. The return value is expirations since the last
		 * call, so >1 means we did not keep up; count it rather than
		 * pretending the cadence held. */
		uint32_t fired = k_timer_status_sync(&tick);

		if (fired > 1) {
			st_late += (fired - 1);
		}
		if (!union_mask || !box_adc_ready()) {
			continue;
		}

		uint16_t raw[AIN_MAX_CH];
		uint64_t t_us = 0;
		int n = box_adc_sweep(union_mask, raw, AIN_MAX_CH, &t_us);

		if (n <= 0) {
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

			if (ain_group_feed(&rt[g], cfg, g, scan, t_us,
					   running_period, &blk)) {
				int64_t now_ms = k_uptime_get();

				if (now_ms - last_block_ms[g] < (1000 / AIN_MAX_BLOCKS_PER_S)) {
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
}

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

void box_ain_stats_reset(void)
{ st_sweeps = st_blocks = st_dropped = st_late = st_throttled = 0; }

#else  /* no ADC on this board */

int  box_ain_init(const box_config_t *c) { ARG_UNUSED(c); return -ENODEV; }
void box_ain_apply(void) { }
int  box_ain_pop(ain_block_t *out) { ARG_UNUSED(out); return 0; }
void box_ain_stats(uint32_t *s, uint32_t *b, uint32_t *d, uint32_t *l, uint32_t *t)
{ if (s) *s = 0; if (b) *b = 0; if (d) *d = 0; if (l) *l = 0; if (t) *t = 0; }
void box_ain_stats_reset(void) { }

#endif
