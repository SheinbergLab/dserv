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
 * NOT EVERY BOX CLOCK IS A CRYSTAL: the FRDM-MCXN947's kernel timer (the OS
 * timer on CLK_1M) runs from the chip's internal RC oscillator, measured
 * 2300 ppm slow against PTP on a rig (2026-11-17) -- 2.3 ms per second of
 * trial, 60 ms by the end of a 26 s one. See "Far samples" below.
 * We learn the relative rate from consecutive TRUSTED anchors only -- the
 * caller marks an anchor trusted when its box time is an IRQ-latched TTL edge
 * (jitter ~us); frame-arrival (sw) anchors still snap the offset but carry
 * 100s-of-us transport jitter, which over a few seconds would swamp the
 * signal, so they never teach the rate. The slope sample
 *
 *     sample_ppb = (d_dserv - d_box) * 1e9 / d_box
 *
 * is folded in with an EMA (alpha 1/8: measured 2ppm pair noise -> <1ppm
 * steady state, still tracks thermal wander over minutes). Trusted-pair
 * endpoints may span intervening sw anchors: slope only needs the endpoints.
 *
 * FAR SAMPLES. A host clock step (chrony) makes one wild sample, which must
 * not move the rate while the offset snap absorbs the step itself. This used
 * to be a flat +/-500 ppm clamp, which also threw away every sample from a
 * box whose clock really is further off than that: the rate stayed 0, nothing
 * said so, and the stamps drifted for the length of each trial. So:
 *
 *   - a sample within NEAR (+/-500 ppm) of the current estimate (0 before
 *     the first) is folded in, as before;
 *   - a sample further off is held as a candidate and counted in
 *     rate_rejects. If the NEXT sample agrees with it (within AGREE,
 *     200 ppm), the pair is the box's real rate and becomes the estimate.
 *     A step is one sample, never two alike, so it is still discarded;
 *   - beyond MAX (5%) is never a clock rate: dropped.
 *
 * rate_last_ppb is the last raw sample, taken or not, so the host can see
 * what the estimator is being told.
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

#define BOX_CLOCK_RATE_NEAR_PPB  500000     /* +/-500 ppm of the estimate: fold in */
#define BOX_CLOCK_RATE_AGREE_PPB 200000     /* two far samples this close: believe */
#define BOX_CLOCK_RATE_MAX_PPB   50000000   /* +/-5%: beyond this, not a clock rate */
#define BOX_CLOCK_PAIR_MIN_US    500000     /* pairs closer than 0.5s: skip   */
#define BOX_CLOCK_PAIR_MAX_US    600000000  /* pairs further than 600s: stale */
#define BOX_CLOCK_TICK_PAIR_US   20000000   /* sw tick pairs this far apart teach the rate */
#define BOX_CLOCK_TICK_SPIKE_US  2000       /* a tick this much later than expected: skip */
#define BOX_CLOCK_TICK_SPIKES    3          /* ... unless this many in a row: re-anchor  */

/* Compiler fence only. Single-core preemption is the concurrency model on
 * every target this runs on (one writer thread, readers on other threads or
 * the same one), so ordering against the SEQ field is all the seqlock below
 * needs -- no cache-coherency barriers, no kernel dependency. GCC/Clang
 * builds are the fleet; a port to another compiler must supply this. */
#define BOX_CLOCK_FENCE() __asm__ __volatile__("" ::: "memory")

typedef struct {
    int64_t  offset_us;       /* dserv - box at the last anchor              */
    uint64_t anchor_box_us;   /* box time of that anchor (rate ref point)    */
    int32_t  rate_ppb;        /* EMA'd relative rate, parts per billion      */
    uint64_t trust_dserv_us;  /* previous TRUSTED anchor (slope endpoint)    */
    uint64_t trust_box_us;
    uint8_t  synced;
    uint8_t  have_trust;
    uint8_t  rate_valid;      /* at least one accepted slope sample          */
    uint8_t  have_pend;       /* a far sample is waiting for a second        */
    int32_t  pend_ppb;        /* that sample                                 */
    int32_t  rate_last_ppb;   /* the last raw slope sample, taken or not     */
    uint32_t rate_rejects;    /* slope samples not folded in (far or absurd) */
    /* sw ticks (box_clock_tick): the open end of the next slope pair, and
     * the filter's bookkeeping */
    uint64_t tick_base_dserv_us;
    uint64_t tick_base_box_us;
    uint8_t  have_tick_base;
    uint8_t  tick_spikes;     /* consecutive ticks skipped as late           */
    uint32_t ticks;           /* ticks taken                                 */
    uint32_t tick_skips;      /* ticks skipped as latency spikes             */
    int32_t  tick_resid_us;   /* the last tick's residual (see below)        */
    /* Seqlock, added with the +29 RX fast path: anchors are written on the
     * eth reader thread while event stamping keeps happening on the service
     * loop, and a preemption mid-64-bit-read must not hand a stamp half of
     * one epoch and half of another. Even = stable; odd = writer inside.
     * ONE writer at a time is still the contract (per box the anchoring
     * context is unique); the lock only protects readers. */
    volatile uint32_t seq;
} box_clock_t;

static inline void box_clock_reset(box_clock_t *c)
{
    c->seq++; BOX_CLOCK_FENCE();
    c->offset_us = 0; c->anchor_box_us = 0; c->rate_ppb = 0;
    c->trust_dserv_us = c->trust_box_us = 0;
    c->synced = 0; c->have_trust = 0; c->rate_valid = 0;
    c->have_pend = 0; c->pend_ppb = 0; c->rate_last_ppb = 0; c->rate_rejects = 0;
    c->tick_base_dserv_us = c->tick_base_box_us = 0; c->have_tick_base = 0;
    c->tick_spikes = 0; c->ticks = 0; c->tick_skips = 0; c->tick_resid_us = 0;
    BOX_CLOCK_FENCE(); c->seq++;
}

/* One slope sample (ppb) from a trusted pair: see "Far samples" above. The
 * caller holds the seqlock. */
static inline void box_clock_rate_sample(box_clock_t *c, int64_t sample)
{
    if (sample < -BOX_CLOCK_RATE_MAX_PPB || sample > BOX_CLOCK_RATE_MAX_PPB) {
        c->rate_last_ppb = sample < 0 ? -BOX_CLOCK_RATE_MAX_PPB : BOX_CLOCK_RATE_MAX_PPB;
        c->rate_rejects++;
        c->have_pend = 0;
        return;
    }
    c->rate_last_ppb = (int32_t) sample;

    int64_t d = sample - (c->rate_valid ? c->rate_ppb : 0);
    if (d >= -BOX_CLOCK_RATE_NEAR_PPB && d <= BOX_CLOCK_RATE_NEAR_PPB) {
        if (!c->rate_valid) { c->rate_ppb = (int32_t) sample; c->rate_valid = 1; }
        else c->rate_ppb += (int32_t)(d / 8);
        c->have_pend = 0;
        return;
    }
    /* far from the estimate: a step, or a rate we have not believed yet */
    if (c->have_pend) {
        int64_t dp = sample - c->pend_ppb;
        if (dp >= -BOX_CLOCK_RATE_AGREE_PPB && dp <= BOX_CLOCK_RATE_AGREE_PPB) {
            c->rate_ppb   = (int32_t)((sample + c->pend_ppb) / 2);
            c->rate_valid = 1;
            c->have_pend  = 0;
            return;
        }
    }
    c->pend_ppb  = (int32_t) sample;
    c->have_pend = 1;
    c->rate_rejects++;
}

/* Re-align from one sync edge. trusted = box_us is a hardware edge latch
 * (teaches the rate); 0 = frame-arrival time (offset snap only). */
static inline void box_clock_sync(box_clock_t *c, uint64_t dserv_us,
                                  uint64_t box_us, int trusted)
{
    c->seq++; BOX_CLOCK_FENCE();
    c->offset_us = (int64_t) dserv_us - (int64_t) box_us;
    c->anchor_box_us = box_us;
    c->synced = 1;

    if (trusted) {
        if (c->have_trust && box_us > c->trust_box_us) {
            int64_t d_box   = (int64_t)(box_us - c->trust_box_us);
            int64_t d_dserv = (int64_t)(dserv_us - c->trust_dserv_us);
            if (d_box >= BOX_CLOCK_PAIR_MIN_US && d_box <= BOX_CLOCK_PAIR_MAX_US) {
                int64_t sample = ((d_dserv - d_box) * 1000000000LL) / d_box;
                box_clock_rate_sample(c, sample);
            }
        }
        c->trust_dserv_us = dserv_us;
        c->trust_box_us   = box_us;
        c->have_trust     = 1;
    }
    BOX_CLOCK_FENCE(); c->seq++;
}

/* ---- sw ticks: a USB box between (and during) obs ----------------------
 *
 * A box with no PTP and no TTL is anchored only by frame arrival, and the obs
 * edges were the only such frames: between sessions nothing re-anchored it,
 * and sw anchors never taught the rate. On an MCXN947 over USB (RC timer,
 * ~1100 ppm off) that left the photo test's edges 600 ms from the patch
 * changes after ten idle minutes (2026-11-25). The host now sends a tick
 * (extio/clock, ~1 Hz, stamped just before the write) and the box folds each
 * one in here. Frame arrival is still the box time, so each carries the
 * transport's latency, but latency is never negative: the EARLIEST-arriving
 * frames are the truest. So:
 *
 *   residual r = dserv_us - stamp(box_us)   (what the mapping says now)
 *
 *   - no rate yet, or r > 0 (this frame beat the estimate): re-anchor here;
 *   - -SPIKE < r <= 0: later than the estimate -- latency, or a drift the
 *     rate misses -- move a quarter of the way;
 *   - r <= -SPIKE: a latency spike (a busy host), skipped; SPIKES of them in
 *     a row is the clock, not the host, and re-anchors.
 *
 * RATE. Over a pair TICK_PAIR apart, a few hundred us of jitter is a few ppm
 * -- the short-pair noise that keeps obs-edge sw anchors from teaching the
 * rate is gone. Each pair is a slope sample through the same estimator as the
 * trusted pairs (far samples need a second that agrees). Only when there is
 * no trusted anchor: a TTL-anchored rate is better and keeps the job.
 * Endpoints are ticks that were not spikes. */
static inline void box_clock_tick(box_clock_t *c, uint64_t dserv_us, uint64_t box_us)
{
    c->seq++; BOX_CLOCK_FENCE();
    int reanchor = 1;
    int64_t r = 0;

    if (c->synced) {
        int64_t dt   = (int64_t) box_us - (int64_t) c->anchor_box_us;
        int64_t pred = (int64_t) box_us + c->offset_us + (dt * (int64_t) c->rate_ppb) / 1000000000LL;
        r = (int64_t) dserv_us - pred;
        c->tick_resid_us = (int32_t)(r > 2000000000LL ? 2000000000LL :
                                     r < -2000000000LL ? -2000000000LL : r);
        if (c->rate_valid && r <= 0) {
            if (r <= -BOX_CLOCK_TICK_SPIKE_US && ++c->tick_spikes < BOX_CLOCK_TICK_SPIKES) {
                c->tick_skips++;
                BOX_CLOCK_FENCE(); c->seq++;
                return;
            }
            if (r > -BOX_CLOCK_TICK_SPIKE_US) {
                /* re-anchor at this tick on a quarter of the residual */
                c->offset_us = pred + r / 4 - (int64_t) box_us;
                c->anchor_box_us = box_us;
                reanchor = 0;
            }
        }
    }
    c->tick_spikes = 0;
    if (reanchor) {
        c->offset_us = (int64_t) dserv_us - (int64_t) box_us;
        c->anchor_box_us = box_us;
    }
    c->synced = 1;
    c->ticks++;

    if (!c->have_trust) {
        if (c->have_tick_base && box_us > c->tick_base_box_us) {
            int64_t d_box = (int64_t)(box_us - c->tick_base_box_us);
            if (d_box > BOX_CLOCK_PAIR_MAX_US) {
                c->have_tick_base = 0;               /* stale: start over */
            } else if (d_box >= BOX_CLOCK_TICK_PAIR_US) {
                int64_t d_dserv = (int64_t)(dserv_us - c->tick_base_dserv_us);
                box_clock_rate_sample(c, ((d_dserv - d_box) * 1000000000LL) / d_box);
                c->have_tick_base = 0;               /* this tick opens the next */
            }
        }
        if (!c->have_tick_base) {
            c->tick_base_dserv_us = dserv_us;
            c->tick_base_box_us   = box_us;
            c->have_tick_base     = 1;
        }
    }
    BOX_CLOCK_FENCE(); c->seq++;
}

/* Coherent snapshot of the mapping terms. Spins only while a writer is
 * mid-update, which is a handful of stores. */
static inline void box_clock_snap(const box_clock_t *c, int64_t *off,
                                  uint64_t *anchor, int32_t *rate, uint8_t *synced)
{
    uint32_t s0, s1;
    do {
        s0 = c->seq; BOX_CLOCK_FENCE();
        *off = c->offset_us; *anchor = c->anchor_box_us;
        *rate = c->rate_ppb; *synced = c->synced;
        BOX_CLOCK_FENCE(); s1 = c->seq;
    } while (s0 != s1 || (s0 & 1u));
}

/* Map a box time_us_64() into dserv time. Returns 0 (=> dserv stamps arrival)
 * until the first sync edge has landed. Rate-corrects relative to the last
 * anchor; events from BEFORE the anchor (negative dt) extrapolate backwards,
 * which is equally valid. */
static inline uint64_t box_clock_stamp(const box_clock_t *c, uint64_t box_us)
{
    int64_t off; uint64_t anchor; int32_t rate; uint8_t synced;
    box_clock_snap(c, &off, &anchor, &rate, &synced);
    if (!synced) return 0;
    int64_t dt   = (int64_t) box_us - (int64_t) anchor;
    int64_t corr = (dt * (int64_t) rate) / 1000000000LL;
    return (uint64_t)((int64_t) box_us + off + corr);
}

/* The inverse: a dserv-time instant onto the box clock (at_abs targets, the
 * PTP-held obs epoch). Two fixed-point iterations converge to well under a
 * microsecond at a fraction of a percent of rate. Lived in main.c as dserv_to_box_us;
 * moved here so the snapshot guard covers every reader of the mapping. */
static inline uint64_t box_clock_unstamp(const box_clock_t *c, uint64_t dserv_us)
{
    int64_t off; uint64_t anchor; int32_t rate; uint8_t synced;
    box_clock_snap(c, &off, &anchor, &rate, &synced);
    (void) synced;                 /* caller gates on synced separately */
    int64_t box = (int64_t) dserv_us - off;
    for (int i = 0; i < 2; i++) {
        int64_t dt   = box - (int64_t) anchor;
        int64_t corr = (dt * (int64_t) rate) / 1000000000LL;
        box = (int64_t) dserv_us - off - corr;
    }
    return (uint64_t) box;
}

/* ---- stamping from a reference taken NOW --------------------------------
 *
 * The mapping above extrapolates from the last anchor, so it is only as good
 * as the rate is steady, and on a box whose timer is an RC oscillator it is
 * not: the MCXN947's moved 500 ppm between an idle half hour and a session
 * (2026-11-17), which no rate held across a trial can follow.
 *
 * A box with a disciplined clock it can read at any moment (the 1588 clock)
 * does not need to extrapolate. Pair that clock with the box timer now
 * (ref_box, ref_dserv) and map an event relative to the pair: the rate then
 * only has to cover the time between the event and the read -- milliseconds
 * for an edge waiting in the FIFO -- where even an unlearned 2300 ppm is a
 * few microseconds. No shared state moves, so nothing steps mid-trial. */
static inline uint64_t box_clock_stamp_at(int32_t rate_ppb, uint64_t ref_box,
                                          uint64_t ref_dserv, uint64_t box_us)
{
    int64_t dt   = (int64_t) box_us - (int64_t) ref_box;
    int64_t corr = (dt * (int64_t) rate_ppb) / 1000000000LL;
    return (uint64_t)((int64_t) ref_dserv + dt + corr);
}

/* The inverse: a dserv instant onto the box timer, from the same pair. */
static inline uint64_t box_clock_unstamp_at(int32_t rate_ppb, uint64_t ref_box,
                                            uint64_t ref_dserv, uint64_t dserv_us)
{
    int64_t dd = (int64_t) dserv_us - (int64_t) ref_dserv;
    int64_t db = (dd * 1000000000LL) / (1000000000LL + (int64_t) rate_ppb);
    return (uint64_t)((int64_t) ref_box + db);
}

#endif /* BOX_CLOCK_H */
