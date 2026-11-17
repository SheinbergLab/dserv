/*
 * clock_test.c -- box_clock.h's rate estimator on the host.
 *
 * The crystal cases (tens of ppm, sw anchors, a host clock step) are what
 * wiznet-io/host/clock_test.c checks for pico_clock.h; they are repeated here
 * so a change to this header cannot lose them. The rest is what that test
 * never asked: a box clock that is NOT a crystal. The FRDM-MCXN947's kernel
 * timer runs from an RC oscillator, measured 2300 ppm slow against PTP on a
 * rig (2026-11-17). The old flat +/-500 ppm clamp dropped every sample from
 * it, the rate stayed 0, and each trial's stamps drifted 2.3 ms per second.
 *
 *   cc -O2 -Wall -I../src/core -o clock_test clock_test.c && ./clock_test
 */
#include "box_clock.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL %s\n", msg); fails++; } \
                           else       printf("  ok   %s\n", msg); } while (0)

static uint64_t T0 = 1700000000000000ull;   /* dserv epoch-us at box boot */
static uint64_t B0 = 5000000ull;            /* box us at that instant     */

/* box time for a dserv time, the box running `ppm` FAST (negative: slow) */
static uint64_t box_at(uint64_t dserv_us, int ppm)
{
    int64_t el = (int64_t)(dserv_us - T0);
    return (uint64_t)((int64_t) B0 + el + (el * ppm) / 1000000);
}

/* n trusted anchors `step_us` apart with +/-noise us of stamp noise */
static uint64_t run(box_clock_t *c, uint64_t d, int n, int step_us, int ppm, int noise)
{
    for (int i = 0; i < n; i++) {
        d += (uint64_t) step_us;
        int nz = noise ? rand() % (2 * noise + 1) - noise : 0;
        box_clock_sync(c, d + nz, box_at(d, ppm), 1);
    }
    return d;
}

static long long stamp_err(const box_clock_t *c, uint64_t d, int ppm)
{
    return (long long) box_clock_stamp(c, box_at(d, ppm)) - (long long) d;
}

int main(void)
{
    /* zeroed, as the firmware's static one is: box_clock_reset() bumps the
       seqlock counter by two, so garbage that starts odd would stay odd and
       every reader would spin */
    box_clock_t c = {0};
    uint64_t d;

    srand(3);

    printf("crystal, +43 ppm fast, TTL anchors every 2 s:\n");
    box_clock_reset(&c);
    CHECK(box_clock_stamp(&c, 123456) == 0, "stamp is 0 before first anchor");
    d = run(&c, T0, 40, 2000000, 43, 9);
    printf("       rate %d ppb (truth -43000), rejects %u\n", c.rate_ppb, c.rate_rejects);
    CHECK(c.rate_valid && abs(c.rate_ppb + 43000) < 3000, "rate within 3 ppm");
    CHECK(c.rate_rejects == 0, "nothing rejected");
    CHECK(llabs(stamp_err(&c, d + 8000000, 43)) < 40, "8 s past the anchor: under 40 us");

    printf("sw anchor, step, short pair:\n");
    int32_t before = c.rate_ppb;
    box_clock_sync(&c, d + 1000000 + 400, box_at(d + 1000000, 43), 0);
    CHECK(c.rate_ppb == before, "sw anchor leaves the rate alone");
    d += 2000000;
    box_clock_sync(&c, d, box_at(d, 43), 1);
    CHECK(abs(c.rate_ppb + 43000) < 3000, "trusted pair spans the sw anchor");
    before = c.rate_ppb;
    d += 2000000;
    box_clock_sync(&c, d + 50000, box_at(d, 43), 1);          /* +50 ms step */
    CHECK(c.rate_ppb == before && c.rate_rejects == 1, "one step: rejected, counted");
    printf("       last sample %d ppb\n", c.rate_last_ppb);
    /* the pair that ends the step is as wild the other way: not a second
       agreeing sample, so still no change */
    d += 2000000;
    box_clock_sync(&c, d, box_at(d, 43), 1);
    CHECK(c.rate_ppb == before && c.rate_rejects == 2, "the step's far side: rejected too");
    d = run(&c, d, 4, 2000000, 43, 0);
    CHECK(abs(c.rate_ppb + 43000) < 3000 && c.rate_rejects == 2, "and the rate is intact after");
    before = c.rate_ppb;
    box_clock_sync(&c, d + 100000, box_at(d + 100000, 43), 1);
    CHECK(c.rate_ppb == before, "sub-500 ms pair skipped");

    printf("RC oscillator, 2300 ppm slow, PTP anchors every 1 s:\n");
    box_clock_reset(&c);
    d = run(&c, T0, 1, 1000000, -2300, 7);
    CHECK(!c.rate_valid, "one anchor: no rate");
    d = run(&c, d, 1, 1000000, -2300, 7);
    CHECK(!c.rate_valid && c.rate_rejects == 1, "first far sample: held, not believed");
    d = run(&c, d, 1, 1000000, -2300, 7);
    printf("       rate %d ppb after 3 anchors\n", c.rate_ppb);
    CHECK(c.rate_valid && abs(c.rate_ppb - 2300000) < 30000, "second agreeing sample: believed");
    d = run(&c, d, 30, 1000000, -2300, 7);
    printf("       rate %d ppb (truth ~ +2305000), rejects %u\n", c.rate_ppb, c.rate_rejects);
    CHECK(abs(c.rate_ppb - 2305300) < 8000, "settles within 8 ppm");
    CHECK(c.rate_rejects == 1, "no further rejects");
    /* a 26 s trial with no anchor: was 60 ms early */
    long long e26 = stamp_err(&c, d + 26000000, -2300);
    printf("       26 s past the anchor: %lld us (uncorrected: -59800)\n", e26);
    CHECK(llabs(e26) < 250, "26 s trial: under 250 us");
    uint64_t back = box_clock_unstamp(&c, d + 26000000);
    CHECK(llabs((long long) back - (long long) box_at(d + 26000000, -2300)) < 250,
          "unstamp inverts it at this rate");

    printf("a step on that box, then a real change of rate:\n");
    before = c.rate_ppb;
    d += 1000000;
    box_clock_sync(&c, d + 30000, box_at(d, -2300), 1);       /* +30 ms step */
    d = run(&c, d, 1, 1000000, -2300, 0);
    CHECK(c.rate_ppb == before, "step and its far side: rate unchanged");
    d = run(&c, d, 3, 1000000, -2300, 7);
    CHECK(abs(c.rate_ppb - 2305300) < 8000, "still tracking after");
    /* the oscillator moves 900 ppm (a warm box, or another clock source):
       far from the estimate, but every sample agrees with the last */
    uint64_t t1 = d;
    for (int i = 0; i < 3; i++) {
        d += 1000000;
        int64_t el1 = (int64_t)(t1 - T0), el2 = (int64_t)(d - t1);
        uint64_t b = (uint64_t)((int64_t) B0 + el1 - (el1 * 2300) / 1000000
                                + el2 - (el2 * 3200) / 1000000);
        box_clock_sync(&c, d, b, 1);
    }
    printf("       rate %d ppb (truth ~ +3210000)\n", c.rate_ppb);
    CHECK(abs(c.rate_ppb - 3210000) < 30000, "a sustained new rate is adopted");

    printf("absurd, and untrusted:\n");
    box_clock_reset(&c);
    box_clock_sync(&c, T0 + 1000000, 1000000, 1);
    box_clock_sync(&c, T0 + 3000000, 2800000, 1);             /* 11% */
    box_clock_sync(&c, T0 + 5000000, 4600000, 1);
    CHECK(!c.rate_valid && c.rate_rejects == 2, "beyond 5%: never a rate, even twice");
    box_clock_reset(&c);
    box_clock_sync(&c, 1000000, 500000, 0);
    box_clock_sync(&c, 3000000, 2500086, 0);
    CHECK(c.rate_ppb == 0 && !c.rate_valid, "sw anchors never teach it");
    CHECK(box_clock_stamp(&c, 2600000) == 2600000 + 499914, "offset-only stamping");

    /* The oscillator does not hold its rate: 2300 ppm slow for the first
       half of a 26 s trial, 1800 for the rest (what psychobox did between a
       session and an idle half hour). The estimate is the stale 2300. An
       event is stamped 2 ms after it happened, from a reference pair read
       at that moment -- the 1588 clock, which is right by construction. */
    printf("stamping from a reference taken at the stamp:\n");
    {
        uint64_t t0 = T0 + 100000000, tb0 = B0 + 77;     /* trial start, both clocks */
        int32_t  est = 2305300;                          /* ppb, learned before */
        long long worst_at = 0, worst_anchor = 0;
        box_clock_t a = {0};
        box_clock_reset(&a);
        box_clock_sync(&a, t0, tb0, 1);
        a.rate_ppb = est; a.rate_valid = 1;              /* the anchor path's rate */
        for (int ms = 0; ms <= 26000; ms += 50) {
            /* true box time at dserv time t0+ms: slow by 2300, then 1800 ppm */
            int64_t el = (int64_t) ms * 1000;
            int64_t h  = el < 13000000 ? el : 13000000;
            int64_t bx = (h - h * 2300 / 1000000) + ((el - h) - (el - h) * 1800 / 1000000);
            uint64_t ev_d = t0 + (uint64_t) el, ev_b = tb0 + (uint64_t) bx;
            /* reference pair 2 ms later (box timer advances ~1996 us) */
            uint64_t ref_d = ev_d + 2000, ref_b = ev_b + 1996;
            long long e1 = (long long) box_clock_stamp_at(est, ref_b, ref_d, ev_b) - (long long) ev_d;
            long long e2 = (long long) box_clock_stamp(&a, ev_b) - (long long) ev_d;
            if (llabs(e1) > llabs(worst_at)) worst_at = e1;
            if (llabs(e2) > llabs(worst_anchor)) worst_anchor = e2;
        }
        printf("       worst error over the trial: fresh reference %lld us, last anchor %lld us\n",
               worst_at, worst_anchor);
        CHECK(llabs(worst_at) <= 3, "fresh reference: within 3 us all trial");
        CHECK(llabs(worst_anchor) > 5000, "last anchor: milliseconds out (the fault)");
        /* with no rate at all the fresh mapping is still only 2 ms * 2300 ppm */
        long long e0 = (long long) box_clock_stamp_at(0, tb0 + 1996, t0 + 2000, tb0) - (long long) t0;
        CHECK(llabs(e0) <= 6, "and with no learned rate: under 6 us");
        /* the inverse, for an at_abs target 80 ms ahead */
        uint64_t tgt = box_clock_unstamp_at(est, tb0, t0, t0 + 80000);
        long long eb = (long long) tgt - (long long)(tb0 + 80000 - 80000LL * 2300 / 1000000);
        CHECK(llabs(eb) <= 2, "unstamp_at: an 80 ms lead lands within 2 us");
        CHECK(box_clock_stamp_at(est, tb0, t0, box_clock_unstamp_at(est, tb0, t0, t0 + 5000000))
              - (t0 + 5000000) + 2 <= 4, "stamp_at inverts unstamp_at");
    }

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
