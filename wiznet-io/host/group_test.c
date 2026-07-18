/*
 * group_test.c -- exercise pico_group.h's chord-settle state machine on the
 * host: the corner roll (the reason groups exist), the sub-window tap rescue,
 * wiggle-while-held, settle_ms=0 passthrough, and the bit-order/csv contract.
 *   cc -O2 -Wall -I../common -o group_test group_test.c && ./group_test
 */
#include "dserv_config.h"
#include "pico_group.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL %s\n", msg); fails++; } \
                           else       printf("  ok   %s\n", msg); } while (0)

/* group 0 = a 4-switch hat on pins 4..7 -> bits: 4=up(1) 5=down(2) 6=left(4) 7=right(8) */
#define UP 0x1
#define DN 0x2
#define LF 0x4
#define RT 0x8

int main(void)
{
    pico_config_t cfg; memset(&cfg, 0, sizeof cfg);
    cfg.group_pins[0]      = (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7);
    cfg.group_settle_ms[0] = 25;

    group_rt_t rt; group_out_t out[2];
    group_reset(&rt, &cfg, 0, NULL);

    printf("bit order / csv contract:\n");
    CHECK(group_bit(&cfg, 0, 4) == 0 && group_bit(&cfg, 0, 7) == 3, "bit i = i-th lowest member");
    CHECK(group_bit(&cfg, 0, 9) == -1, "non-member pin -> -1");
    uint32_t mask; char ps[32];
    CHECK(dserv_parse_pins("4,5,6,7", &mask) == 4 && mask == cfg.group_pins[0], "parse csv");
    dserv_pins_str(mask, ps, sizeof ps);
    CHECK(strcmp(ps, "4,5,6,7") == 0, "format csv (ascending)");
    CHECK(dserv_parse_pins("off", &mask) == 0 && mask == 0, "'off' clears");
    CHECK(dserv_parse_pins("4,99", &mask) == -1, "reject out-of-range pin");

    printf("corner roll (UP @1ms, +RIGHT @13ms -> ONE event, stamped at 1ms):\n");
    CHECK(group_feed(&rt, &cfg, 0, 4, 1, 1000) == 1, "feed member consumed");
    CHECK(group_poll(&rt, &cfg, 0, 11000, out) == 0, "window still open at +10ms");
    group_feed(&rt, &cfg, 0, 7, 1, 13000);
    CHECK(group_poll(&rt, &cfg, 0, 20000, out) == 0, "second edge restarted window");
    int n = group_poll(&rt, &cfg, 0, 38001, out);
    CHECK(n == 1 && out[0].bits == (UP|RT) && out[0].t_us == 1000, "settled UP|RIGHT @ onset");

    printf("release (staggered) -> one 0 event stamped at first release:\n");
    group_feed(&rt, &cfg, 0, 4, 0, 100000);
    group_feed(&rt, &cfg, 0, 7, 0, 104000);
    n = group_poll(&rt, &cfg, 0, 129001, out);
    CHECK(n == 1 && out[0].bits == 0 && out[0].t_us == 100000, "settled 0 @ first release edge");

    printf("sub-window tap (DOWN 15ms) -> peak + release pair:\n");
    group_feed(&rt, &cfg, 0, 5, 1, 200000);
    group_feed(&rt, &cfg, 0, 5, 0, 215000);
    n = group_poll(&rt, &cfg, 0, 240001, out);
    CHECK(n == 2 && out[0].bits == DN && out[0].t_us == 200000, "tap: peak @ onset");
    CHECK(out[1].bits == 0 && out[1].t_us == 215000, "tap: return @ release edge");

    printf("wiggle while held (UP held, flick to UP|RIGHT and back):\n");
    group_feed(&rt, &cfg, 0, 4, 1, 300000);
    n = group_poll(&rt, &cfg, 0, 325001, out);
    CHECK(n == 1 && out[0].bits == UP, "holding UP");
    group_feed(&rt, &cfg, 0, 7, 1, 400000);
    group_feed(&rt, &cfg, 0, 7, 0, 405000);
    n = group_poll(&rt, &cfg, 0, 430001, out);
    CHECK(n == 2 && out[0].bits == (UP|RT) && out[0].t_us == 400000, "flick: peak @ onset");
    CHECK(out[1].bits == UP && out[1].t_us == 405000, "flick: back to held state");

    printf("roll-through to final (UP -> UP|RIGHT -> RIGHT inside one window):\n");
    group_feed(&rt, &cfg, 0, 4, 0, 500000);            /* release the held UP first */
    group_poll(&rt, &cfg, 0, 525001, out);
    group_feed(&rt, &cfg, 0, 4, 1, 600000);
    group_feed(&rt, &cfg, 0, 7, 1, 605000);
    group_feed(&rt, &cfg, 0, 4, 0, 610000);
    n = group_poll(&rt, &cfg, 0, 635001, out);
    CHECK(n == 1 && out[0].bits == RT && out[0].t_us == 600000, "final state wins, onset stamp");

    printf("settle_ms = 0 -> every settled transition, atomic per pass:\n");
    cfg.group_settle_ms[0] = 0;
    group_reset(&rt, &cfg, 0, NULL);
    group_feed(&rt, &cfg, 0, 6, 1, 700000);
    n = group_poll(&rt, &cfg, 0, 700000, out);
    CHECK(n == 1 && out[0].bits == LF && out[0].t_us == 700000, "immediate emit");
    group_feed(&rt, &cfg, 0, 6, 0, 701000);
    n = group_poll(&rt, &cfg, 0, 701000, out);
    CHECK(n == 1 && out[0].bits == 0, "immediate release emit");

    printf("reset from live levels + non-member feed:\n");
    uint8_t lv[PICO_NPINS] = {0}; lv[5] = 1;
    cfg.group_settle_ms[0] = 25;
    group_reset(&rt, &cfg, 0, lv);
    CHECK(rt.cur == DN, "reset derives bitmask from levels");
    CHECK(group_feed(&rt, &cfg, 0, 12, 1, 800000) == 0, "non-member ignored");
    CHECK(group_poll(&rt, &cfg, 0, 900000, out) == 0, "no episode from non-member");
    group_feed(&rt, &cfg, 0, 5, 1, 900000);            /* already-down re-seed: no change */
    CHECK(group_poll(&rt, &cfg, 0, 930000, out) == 0, "no-change feed opens no episode");

    printf(fails ? "\nFAILED (%d)\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
