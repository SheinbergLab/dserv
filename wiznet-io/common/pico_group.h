/*
 * pico_group.h -- chord-settle state machine for DI pin groups.
 *
 * A group is a set of DI pins (pico_config_t.group_pins[g]) published as ONE
 * atomic bitmask datapoint instead of (or alongside) per-pin di/<n> events.
 * The problem it solves: a multi-switch device (a 4-switch joystick hat) never
 * closes two switches simultaneously -- a diagonal arrives as UP then, a few
 * ms later, UP+RIGHT. Per-pin events force the host to reassemble that burst;
 * this module settles it on-box and reports one event, stamped at the FIRST
 * edge, so downstream reaction times are measured at the true movement onset
 * while the reported state is the completed chord.
 *
 * Rules (per group, driven by already-DEBOUNCED di events -- pico_gpio.h's
 * per-pin debounce_ms runs first, so settle_ms only has to cover the
 * mechanical roll gap between switches, not contact bounce):
 *   - the first member edge that departs from the last PUBLISHED state opens
 *     an "episode": onset := that edge's timestamp
 *   - further member edges keep updating the pending bitmask; each restarts
 *     the settle window (group_settle_ms)
 *   - once the group has been quiet for settle_ms, the episode closes:
 *       pend != cur           -> emit {pend @ onset}          (the corner case:
 *                                UP@t0 + RIGHT@t0+12ms -> one UP|RIGHT @ t0)
 *       pend == cur, peak !=  -> emit {peak @ onset}, {cur @ last-edge}
 *                                (a sub-window tap: down-and-back must not be
 *                                swallowed -- it IS the response)
 *       otherwise             -> nothing (bounce that ended where it started)
 *   - settle_ms == 0          -> every settled DI transition emits on the next
 *                                poll (still an atomic snapshot per loop pass)
 *
 * Bit order contract: published bit i = i-th LOWEST member pin -- the same
 * order dserv_pins_str() announces in the manifest, so hosts decode from the
 * announced "2,3,4,5" string alone.
 *
 * Pure C, zero-alloc, no hardware; feed/poll take caller timestamps so the
 * whole machine unit-tests on the host (host/group_test.c).
 */
#ifndef PICO_GROUP_H
#define PICO_GROUP_H

#include "dserv_config.h"

typedef struct { uint8_t bits; uint64_t t_us; } group_out_t;

typedef struct {
    uint8_t  cur;          /* last emitted bitmask                          */
    uint8_t  pend;         /* live bitmask (logical levels, post-debounce)  */
    uint8_t  peak;         /* last non-equal-to-cur state seen this episode */
    uint8_t  open;         /* episode in progress                           */
    uint64_t onset_us;     /* first member edge since last emit -> stamp    */
    uint64_t last_chg_us;  /* settle-timer base (last member edge time)     */
} group_rt_t;

/* bit index of pin within group g (ascending member order), or -1. */
static inline int group_bit(const pico_config_t *c, int g, int pin)
{
    uint32_t m = c->group_pins[g];
    if (pin < 0 || pin >= PICO_NPINS || !((m >> pin) & 1u)) return -1;
    int b = 0;
    for (int i = 0; i < pin; i++) if ((m >> i) & 1u) b++;
    return b;
}

/* Re-derive a group's state from current logical pin levels (levels may be
 * NULL -> all released). Call at boot and after any group/pin-mode change. */
static inline void group_reset(group_rt_t *rt, const pico_config_t *c, int g,
                               const uint8_t *logical_levels)
{
    uint8_t bits = 0; int b = 0;
    for (int i = 0; i < PICO_NPINS; i++)
        if ((c->group_pins[g] >> i) & 1u) {
            if (logical_levels && logical_levels[i]) bits |= (uint8_t)(1u << b);
            b++;
        }
    rt->cur = rt->pend = rt->peak = bits;
    rt->open = 0; rt->onset_us = rt->last_chg_us = 0;
}

/* Feed one settled DI transition (LOGICAL level -- see di_logical). Returns 1
 * if the pin is a member of this group (drives the group_quiet suppression of
 * the per-pin publish), 0 otherwise. */
static inline int group_feed(group_rt_t *rt, const pico_config_t *c, int g,
                             int pin, int level, uint64_t t_us)
{
    int b = group_bit(c, g, pin);
    if (b < 0) return 0;
    uint8_t bits = rt->pend;
    if (level) bits |= (uint8_t)(1u << b); else bits &= (uint8_t) ~(1u << b);
    if (bits == rt->pend) return 1;              /* member, but no state change */
    rt->pend = bits;
    if (!rt->open) { rt->open = 1; rt->onset_us = t_us; rt->peak = rt->cur; }
    if (bits != rt->cur) rt->peak = bits;
    rt->last_chg_us = t_us;
    return 1;
}

/* Poll for settle-window expiry. Fills out[0..1]; returns the event count
 * (0..2). Call once per loop pass with the current box-clock time. */
static inline int group_poll(group_rt_t *rt, const pico_config_t *c, int g,
                             uint64_t now_us, group_out_t out[2])
{
    if (!rt->open) return 0;
    uint64_t win = (uint64_t) c->group_settle_ms[g] * 1000u;
    if (now_us - rt->last_chg_us < win) return 0;
    rt->open = 0;
    int n = 0;
    if (rt->pend != rt->cur) {                   /* settled on a new state */
        out[n].bits = rt->pend; out[n].t_us = rt->onset_us; n++;
        rt->cur = rt->pend;
    } else if (rt->peak != rt->cur) {            /* tap: went somewhere and back */
        out[n].bits = rt->peak; out[n].t_us = rt->onset_us;    n++;
        out[n].bits = rt->cur;  out[n].t_us = rt->last_chg_us; n++;
    }
    rt->peak = rt->cur;
    return n;
}

#endif /* PICO_GROUP_H */
