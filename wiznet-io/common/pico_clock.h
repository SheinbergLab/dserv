/*
 * pico_clock.h -- box->dserv clock alignment (offset-only discipline).
 *
 * The box runs on its own free-running time_us_64() clock (us since boot). To
 * make the events it publishes land in dserv's clock -- interleaved with local
 * evt_put/GPIO events as if the box were a local device -- we align to dserv
 * time at each observation-period sync edge.
 *
 * At every ess/in_obs edge (begin=1 AND end=0, so two anchors per obs) dserv's
 * timestamp of the toggle rides in the datapoint frame. We pair it with the
 * box's local receipt time and SNAP the offset:
 *
 *     offset_us = dserv_us - box_us      (box time -> dserv time is + offset)
 *
 * Offset-only on purpose: obs periods are short (seconds), so pico-crystal drift
 * within one is sub-100us, and slope/rate estimation would only amplify the
 * network jitter in each anchor's receipt time. If tighter alignment is ever
 * needed, wire the physical obs line into a box DI pin (jitter-free anchors) and
 * a rate term becomes worth adding.
 *
 * Before the first sync, stamp() returns 0 so dserv arrival-stamps the event
 * (today's behavior) -- events still publish, just not yet aligned.
 *
 * Dependency-free (stdint only); host-testable. Caller owns the struct, same as
 * dserv_framer_t.
 */
#ifndef PICO_CLOCK_H
#define PICO_CLOCK_H

#include <stdint.h>

typedef struct { int64_t offset_us; uint8_t synced; } box_clock_t;

static inline void box_clock_reset(box_clock_t *c)
{ c->offset_us = 0; c->synced = 0; }

/* Re-align to dserv time from one sync edge. */
static inline void box_clock_sync(box_clock_t *c, uint64_t dserv_us, uint64_t box_us)
{ c->offset_us = (int64_t) dserv_us - (int64_t) box_us; c->synced = 1; }

/* Map a box time_us_64() into dserv time. Returns 0 (=> dserv stamps arrival)
 * until the first sync edge has landed. */
static inline uint64_t box_clock_stamp(const box_clock_t *c, uint64_t box_us)
{ return c->synced ? (uint64_t) ((int64_t) box_us + c->offset_us) : 0; }

#endif /* PICO_CLOCK_H */
