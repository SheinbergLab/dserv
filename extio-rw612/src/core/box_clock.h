/*
 * box_clock.h -- box->dserv clock alignment (offset + learned rate).
 *
 * The box runs on its own free-running time_us_64() clock (us since boot). To
 * make the events it publishes land in dserv's clock -- interleaved with local
 * evt_put/GPIO events as if the box were a local device -- we align to dserv
 * time at each observation-period sync edge (begin=1 AND end=0, two anchors
 * per obs): dserv's timestamp of the toggle rides in the datapoint frame and
 * pairs with a box-side time.
 *
 * OFFSET: snapped at every anchor, trusted or not:
 *
 *     offset_us = dserv_us - box_us      (box time -> dserv time is + offset)
 *
 * RATE: the crystals differ by tens of ppm (rig-measured: +43ppm box-fast,
 * stable to ~2ppm), so between anchors a pure offset drifts ~43us per second
 * -- with hardware-edge anchors (~10us class, see the TTL sync input) that
 * between-anchor drift is the DOMINANT stamping error on multi-second trials.
 * We learn the relative rate from consecutive TRUSTED anchors only -- the
 * caller marks an anchor trusted when its box time is an IRQ-latched TTL edge
 * (jitter ~us); frame-arrival (sw) anchors still snap the offset but carry
 * 100s-of-us transport jitter, which over a few seconds would swamp the
 * signal, so they never teach the rate. The slope sample
 *
 *     sample_ppb = (d_dserv - d_box) * 1e9 / d_box
 *
 * is folded in with an EMA (alpha 1/8: measured 2ppm pair noise -> <1ppm
 * steady state, still tracks thermal wander over minutes) and clamped to
 * +/-500 ppm -- a host clock step (chrony) makes one wild sample, which the
 * clamp discards while the offset snap absorbs the step itself. Trusted-pair
 * endpoints may span intervening sw anchors: slope only needs the endpoints.
 *
 *     stamp(t) = t + offset + (t - anchor_box) * rate / 1e9
 *
 * Boxes with no TTL wired never present a trusted anchor: rate stays 0 and
 * behavior is exactly the historical offset-only discipline.
 *
 * Before the first sync, stamp() returns 0 so dserv arrival-stamps the event
 * (today's behavior) -- events still publish, just not yet aligned.
 *
 * Dependency-free (stdint only); host-testable (host/clock_test.c). Caller
 * owns the struct, same as dserv_framer_t.
 */
#ifndef BOX_CLOCK_H
#define BOX_CLOCK_H

#include <stdint.h>

#define BOX_CLOCK_RATE_MAX_PPB   500000     /* +/-500 ppm sanity clamp        */
#define BOX_CLOCK_PAIR_MIN_US    500000     /* pairs closer than 0.5s: skip   */
#define BOX_CLOCK_PAIR_MAX_US    600000000  /* pairs further than 600s: stale */

typedef struct {
    int64_t  offset_us;       /* dserv - box at the last anchor              */
    uint64_t anchor_box_us;   /* box time of that anchor (rate ref point)    */
    int32_t  rate_ppb;        /* EMA'd relative rate, parts per billion      */
    uint64_t trust_dserv_us;  /* previous TRUSTED anchor (slope endpoint)    */
    uint64_t trust_box_us;
    uint8_t  synced;
    uint8_t  have_trust;
    uint8_t  rate_valid;      /* at least one accepted slope sample          */
} box_clock_t;

static inline void box_clock_reset(box_clock_t *c)
{
    c->offset_us = 0; c->anchor_box_us = 0; c->rate_ppb = 0;
    c->trust_dserv_us = c->trust_box_us = 0;
    c->synced = 0; c->have_trust = 0; c->rate_valid = 0;
}

/* Re-align from one sync edge. trusted = box_us is a hardware edge latch
 * (teaches the rate); 0 = frame-arrival time (offset snap only). */
static inline void box_clock_sync(box_clock_t *c, uint64_t dserv_us,
                                  uint64_t box_us, int trusted)
{
    c->offset_us = (int64_t) dserv_us - (int64_t) box_us;
    c->anchor_box_us = box_us;
    c->synced = 1;

    if (!trusted) return;
    if (c->have_trust && box_us > c->trust_box_us) {
        int64_t d_box   = (int64_t)(box_us - c->trust_box_us);
        int64_t d_dserv = (int64_t)(dserv_us - c->trust_dserv_us);
        if (d_box >= BOX_CLOCK_PAIR_MIN_US && d_box <= BOX_CLOCK_PAIR_MAX_US) {
            int64_t sample = ((d_dserv - d_box) * 1000000000LL) / d_box;
            if (sample >= -BOX_CLOCK_RATE_MAX_PPB && sample <= BOX_CLOCK_RATE_MAX_PPB) {
                if (!c->rate_valid) { c->rate_ppb = (int32_t) sample; c->rate_valid = 1; }
                else c->rate_ppb += (int32_t)((sample - c->rate_ppb) / 8);
            }
        }
    }
    c->trust_dserv_us = dserv_us;
    c->trust_box_us   = box_us;
    c->have_trust     = 1;
}

/* Map a box time_us_64() into dserv time. Returns 0 (=> dserv stamps arrival)
 * until the first sync edge has landed. Rate-corrects relative to the last
 * anchor; events from BEFORE the anchor (negative dt) extrapolate backwards,
 * which is equally valid. */
static inline uint64_t box_clock_stamp(const box_clock_t *c, uint64_t box_us)
{
    if (!c->synced) return 0;
    int64_t dt   = (int64_t) box_us - (int64_t) c->anchor_box_us;
    int64_t corr = (dt * (int64_t) c->rate_ppb) / 1000000000LL;
    return (uint64_t)((int64_t) box_us + c->offset_us + corr);
}

#endif /* BOX_CLOCK_H */
