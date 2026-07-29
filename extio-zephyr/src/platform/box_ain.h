/*
 * box_ain.h -- the analog sampling thread.
 *
 * Sits between box_adc.h (one sweep) and box_ain_group.h (the packing state
 * machine), and owns the one thing neither of them can: a clock. Scans the union
 * of every active group's channels at the box-wide base rate (config ain_rate),
 * feeds each group, and queues whatever blocks come out.
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
 * errno. Safe to call when ain_en is clear: the thread starts and idles. */
int box_ain_init(const box_config_t *cfg);

/* Re-read rate/channels/policy after a config change, and reset every group's
 * packing state. Call after `mcp`/`ain` CLI edits or a persist load. Cheap; the
 * sampler picks it up on its next tick rather than being restarted. */
void box_ain_apply(void);

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

#endif /* BOX_AIN_H */
