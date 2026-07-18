/*
 * clock_test.c -- exercise pico_clock.h's offset+rate discipline on the host:
 * rate learning from trusted anchors with known synthetic drift, sw anchors
 * snapping offset without teaching rate, the host-clock-step clamp, and the
 * between-anchor stamping error that rate correction exists to remove.
 *   cc -O2 -Wall -I../common -o clock_test clock_test.c && ./clock_test
 */
#include "pico_clock.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL %s\n", msg); fails++; } \
                           else       printf("  ok   %s\n", msg); } while (0)

/* Ground truth: box crystal runs +43 ppm FAST relative to dserv.
 * dserv time t -> box reads box0 + (t - t0)*(1 + 43e-6). Integer math to
 * keep the model exact: box elapsed = dserv elapsed + dserv elapsed*43/1e6. */
#define PPM 43
static uint64_t T0 = 1700000000000000ull;   /* dserv epoch-us at box boot */
static uint64_t B0 = 5000000ull;            /* box us at that instant     */

static uint64_t box_of(uint64_t dserv_us)
{
    uint64_t el = dserv_us - T0;
    return B0 + el + (el * PPM) / 1000000ull;
}

int main(void)
{
    box_clock_t c;
    box_clock_reset(&c);

    printf("pre-sync:\n");
    CHECK(box_clock_stamp(&c, 123456) == 0, "stamp is 0 before first anchor");

    /* trusted anchors every 2s (the TTL cadence), tiny +/-9us stamp noise */
    printf("rate learning from trusted anchors:\n");
    srand(3);
    uint64_t d = T0;
    for (int i = 0; i < 40; i++) {
        d += 2000000;
        int noise = rand() % 19 - 9;
        box_clock_sync(&c, d + noise, box_of(d), 1);
    }
    CHECK(c.rate_valid, "rate learned");
    /* box runs fast -> dserv gains less than box -> rate is NEGATIVE ~ -43ppm
     * (sample = (d_dserv - d_box)/d_box; d_box > d_dserv) */
    printf("       learned rate = %d ppb (truth %d)\n", c.rate_ppb, -PPM * 1000);
    CHECK(abs(c.rate_ppb + PPM * 1000) < 3000, "rate within 3ppm of truth");

    /* the payoff: an event 8s after the last anchor. Uncorrected error would
     * be ~ 43ppm * 8s = 344us; corrected must be ~us-class (+ anchor noise). */
    printf("between-anchor stamping (8s past last anchor):\n");
    uint64_t ev_d = d + 8000000;
    int64_t err = (int64_t) box_clock_stamp(&c, box_of(ev_d)) - (int64_t) ev_d;
    printf("       corrected stamp error = %lld us\n", (long long) err);
    CHECK(err > -40 && err < 40, "corrected error < 40us (was ~344us offset-only)");

    /* events BEFORE the anchor extrapolate backwards equally well */
    uint64_t ev_b = d - 5000000;
    err = (int64_t) box_clock_stamp(&c, box_of(ev_b)) - (int64_t) ev_b;
    CHECK(err > -40 && err < 40, "pre-anchor (negative dt) also corrected");

    /* sw anchor: snaps offset (stamp exact AT it), leaves rate untouched */
    printf("sw anchors snap but don't teach:\n");
    int32_t rate_before = c.rate_ppb;
    uint64_t d_sw = d + 3000000;
    box_clock_sync(&c, d_sw + 400, box_of(d_sw), 0);      /* +400us transport */
    CHECK(c.rate_ppb == rate_before, "sw anchor left rate unchanged");
    err = (int64_t) box_clock_stamp(&c, box_of(d_sw)) - (int64_t)(d_sw + 400);
    CHECK(err == 0, "offset snapped to the sw anchor");

    /* next trusted pair SPANS the sw anchor and still measures cleanly */
    uint64_t d_t = d + 6000000;
    box_clock_sync(&c, d_t, box_of(d_t), 1);
    CHECK(abs(c.rate_ppb + PPM * 1000) < 3000, "trusted pair spans sw anchor");

    /* host clock step: one wild sample must be clamped away */
    printf("host-step clamp:\n");
    rate_before = c.rate_ppb;
    uint64_t d_j = d_t + 2000000;
    box_clock_sync(&c, d_j + 50000, box_of(d_j), 1);      /* +50ms step: 25000ppm */
    CHECK(c.rate_ppb == rate_before, "wild sample rejected by clamp");
    CHECK(c.offset_us == (int64_t)(d_j + 50000) - (int64_t) box_of(d_j),
          "offset still snapped through the step");
    box_clock_sync(&c, d_j + 2000000, box_of(d_j + 2000000), 1);
    CHECK(abs(c.rate_ppb + PPM * 1000) < 6000, "recovers after the step");

    /* too-close pair: ignored (interval floor) */
    rate_before = c.rate_ppb;
    uint64_t d_c = d_j + 2000000 + 100000;                /* 100ms later */
    box_clock_sync(&c, d_c, box_of(d_c), 1);
    CHECK(c.rate_ppb == rate_before, "sub-500ms pair skipped");

    /* untrusted-only box (no TTL): behaves exactly like the old offset-only */
    printf("no-TTL box unchanged:\n");
    box_clock_t u;
    box_clock_reset(&u);
    box_clock_sync(&u, 1000000, 500000, 0);
    box_clock_sync(&u, 3000000, 2500086, 0);
    CHECK(u.rate_ppb == 0 && !u.rate_valid, "rate never learned");
    CHECK(box_clock_stamp(&u, 2500086) == 3000000, "pure offset stamping");

    printf(fails ? "\nFAILED (%d)\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
