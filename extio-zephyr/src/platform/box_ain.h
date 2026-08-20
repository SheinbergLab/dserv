/*
 * box_ain.h -- the analog sampling thread.
 *
 * Sits between box_adc.h (one sweep) and box_ain_group.h (the packing state
 * machine), and owns the one thing neither of them can: a clock. Scans the union
 * of every active group's channels at the box-wide base rate (config ain_rate),
 * feeds each group, and queues whatever blocks come out.
 *
 * THE THREAD OWNS THE CONVERTER. Every box_adc_* call that touches hardware --
 * init, suspend, resume, sweep -- happens on this thread and nowhere else, so
 * there is no lock and no window in which the service loop could suspend a
 * device mid-sweep. Callers express what they want (box_ain_apply,
 * box_ain_hold) and the thread decides when to act on it.
 *
 * WHY A THREAD AND NOT THE SERVICE LOOP. adc_read() parks its caller for the
 * whole sweep -- ~100 us for four channels, because the MCP3204 is capped near
 * 1 MHz SPI at 3.3 V. Putting that on the service loop would spend 10% of it at
 * 1 kHz, and worse, invisibly: dbg/loop_max_us is published BY the loop, so it
 * cannot report a stall that stops it publishing. That is the same trap OTA
 * step 2 fell into.
 *
 * THE THREAD NEVER TOUCHES THE UPLINK. Finished blocks go into a k_msgq and the
 * service loop drains them with box_ain_pop() and sends. Exactly one thread ever
 * writes the wire -- the same split as the RP2350's core0 -> core1 queue, for
 * the same reason, and it means a slow uplink can never stall sampling.
 *
 * WHAT IT DOES NOT DO: remove jitter. Every block carries the box-clock stamp of
 * its first sample, which PTP holds within well under a microsecond of dserv's
 * timeline, so a late sweep is RECORDED rather than hidden. box_ain_stats()
 * reports how often the cadence slipped; a run whose `late` count is non-zero
 * has samples that are still correctly placed in time, just unevenly spaced.
 */
#ifndef BOX_AIN_H
#define BOX_AIN_H

#include <stdint.h>
#include "dserv_config.h"
#include "box_ain_group.h"

/* Start the sampler. `cfg` must outlive it (main.c's static config). Returns 0,
 * -ENODEV when no ADC is fitted (the normal case for most boxes), or a negative
 * errno.
 *
 * Safe to call when ain_en is clear: the thread starts, finds nothing to sample,
 * suspends the converter and parks. It costs one thread's stack and nothing
 * else -- no timer, no wakeups. It used to keep ticking at the configured rate
 * to do nothing, which at 1 kHz was a thousand pointless wakeups a second on a
 * box that had analog switched off. */
int box_ain_init(const box_config_t *cfg);

/* Re-read rate/channels/policy after a config change, and reset every group's
 * packing state. Call after `ain`/`pin ... mode ain` edits or a persist load.
 * Cheap; the sampler picks it up on its next tick rather than being restarted.
 *
 * ALSO WAKES A STOPPED SAMPLER, which is what makes `ain enable 1` take effect
 * live instead of at the next reboot. */
void box_ain_apply(void);

/* Keep analog OFF for the next `ms` milliseconds -- converter suspended, thread
 * not ticking, nothing published.
 *
 * A DEADLINE, NOT A FLAG, and that is the whole design. A `box_ain_pause()` /
 * `box_ain_unpause()` pair would leave analog dead forever the first time a host
 * vanished mid-OTA, and it would present exactly like working hardware that has
 * stopped producing data -- the shape that cost us box3. A deadline the caller
 * refreshes cannot do that: stop refreshing, for any reason including crashing,
 * and analog comes back by itself.
 *
 * Extends rather than replaces, so overlapping callers cannot shorten each
 * other's hold. Cheap enough to call per OTA chunk, which is how it is used. */
void box_ain_hold(uint32_t ms);

/* ---- BORROW THE CONVERTER ----
 *
 * Stop the sampler, wait for it to actually be stopped, and hand the device to
 * the caller powered and idle. For a command that must drive the ADC itself --
 * `adccal`, which sweeps a DAC ramp into an analog input from the console
 * thread. Returns 0 on success, -EBUSY if the sampler did not stop in time
 * (which means the caller must NOT touch the device), or -ENODEV.
 *
 * WAITING IS THE POINT. Without it this is just a race with better manners: the
 * sampler can be parked inside k_timer_status_sync for a whole period, so
 * "asked it to stop" and "it has stopped" are up to 1/ain_rate apart.
 *
 * `ms` is how long the borrow is guaranteed for -- the same self-expiring
 * deadline box_ain_hold() uses, so a caller that dies mid-sweep loses the
 * converter for `ms` and no longer.
 *
 * box_ain_return() ends it early. It does NOT decide the resting state: it
 * clears the hold and lets the sampler reconcile, so the converter ends up
 * powered or suspended according to the live config rather than according to
 * whatever the borrower assumed. */
int  box_ain_borrow(uint32_t ms);
void box_ain_return(void);

/* 1 while the sampler is actually running. Distinct from `ain_en`, which is what
 * the operator ASKED for -- this is what the box is DOING, and publishing both
 * is what makes "analog is configured but nothing is arriving" a one-glance
 * diagnosis instead of an evening. */
int  box_ain_running(void);

/* How many times something has pushed analog aside with box_ain_hold(). Climbs
 * during an OTA and nowhere else; a box whose analog "went quiet" with this
 * frozen was not held, so look elsewhere. */
uint32_t box_ain_holds(void);

/* Service loop: pop one ready block. Returns 1 and fills `out`, or 0 if none.
 * Non-blocking. */
int box_ain_pop(ain_block_t *out);

/* sweeps  -- base scans actually taken
 * blocks  -- blocks handed to the service loop
 * dropped -- blocks lost because the queue was full (the loop is not draining)
 * late    -- ticks where the cadence had already expired again before we ran,
 *            i.e. sampling fell behind. NOT the same as dropped, and the more
 *            important of the two: dropped loses data the box knows about,
 *            late means samples were never taken.
 * throttled -- blocks refused by the per-group rate ceiling. Non-zero means a
 *            group is trying to publish faster than the uplink can afford,
 *            which in on-change mode is driven by INPUT NOISE rather than by
 *            config -- the failure that took box3 off the air on 2026-07-28. */
void box_ain_stats(uint32_t *sweeps, uint32_t *blocks,
		   uint32_t *dropped, uint32_t *late, uint32_t *throttled);
void box_ain_stats_reset(void);

/* +26 stall ledger: floor length of the last / worst sweep starvation gap */
void box_ain_late_gaps(uint32_t *last_us, uint32_t *max_us);

/* Sweeps the DRIVER refused (adc_read() < 0, -EAGAIN excluded), and the last
 * such errno. The polled loop SKIPS a failed sweep -- correct per sweep, and
 * before this counter it meant a converter refusing every conversion (wrong
 * oversample, wrong reference) was indistinguishable from a box with quiet
 * inputs: enabled, running, publishing nothing. `sweeps` climbing while
 * `sweep_err` climbs with it is the fingerprint to look for. */
void box_ain_sweep_errs(uint32_t *errs, int32_t *last_rc);

/* ---- v25 pacing: what is RUNNING, next to what was ASKED FOR ----
 *
 * Published as a pair (ain/dbg/pace, ain/dbg/pace_want) on purpose. Hardware
 * pacing can decline to start -- no ADC, no DMA channel, a channel set the
 * chain cannot hold -- and the sampler then falls back to the polled loop
 * rather than stopping. That is the right behaviour and the wrong thing to
 * leave invisible: a box whose timing quietly got worse looks exactly like a
 * box whose timing was always that bad. */
int box_ain_streaming(void);    /* 1 = hardware-paced right now */
int box_ain_pace_want(void);    /* 1 = the config asked for it  */

/* Geometry the stream planner settled on -- trigger rate, triggers averaged
 * per delivered sample, hardware AVGS exponent per trigger -- plus:
 *   clamped  1 = the requested oversample did NOT fit the sample period and
 *            was reduced. The operator is getting less averaging than asked.
 *   fails    stream starts refused (each one a fall back to polled)
 *   resyncs  FIFO phase slips that forced a restart
 * All zero on a polled box. */
void box_ain_stream_info(uint32_t *trig_hz, uint16_t *spacing, uint8_t *avgs_exp,
			 uint8_t *clamped, uint32_t *fails, uint32_t *resyncs);

#endif /* BOX_AIN_H */
