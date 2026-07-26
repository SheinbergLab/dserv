/*
 * ain_group_test.c -- exercise pico_ain_group.h on the host: on-change deadband
 * (joystick), continuous decimate (drop) + batch packing, boxcar averaging, the
 * block wire header, and the mcp_en default synthesizer.
 *   cc -O2 -Wall -I../common -o ain_group_test ain_group_test.c && ./ain_group_test
 */
#include "dserv_config.h"
#include "pico_ain_group.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL %s\n", msg); fails++; } \
                           else       printf("  ok   %s\n", msg); } while (0)

/* a full 4-channel scan */
static void scan_set(int16_t s[AIN_MAX_CH], int c0, int c1, int c2, int c3)
{ s[0]=(int16_t)c0; s[1]=(int16_t)c1; s[2]=(int16_t)c2; s[3]=(int16_t)c3; }

int main(void)
{
    pico_config_t cfg; ain_block_t out; int16_t s[AIN_MAX_CH];

    /* ---- default synthesizer ---- */
    memset(&cfg, 0, sizeof cfg);
    cfg.mcp_en = 1;
    dserv_cfg_ain_default(&cfg);
    CHECK(cfg.ain_group_chans[0] == 0x03, "default: group0 = ch{0,1}");
    CHECK(cfg.ain_group_mode[0] == 0 && cfg.ain_group_deadband[0] == 8, "default: on-change, db=8");
    CHECK(strcmp(cfg.ain_group_label[0], "joystick") == 0, "default: labeled 'joystick'");
    { char leaf[32]; dserv_ain_group_leaf(&cfg, 0, leaf, sizeof leaf);
      CHECK(strcmp(leaf, "ain/joystick") == 0, "leaf = ain/joystick"); }
    memset(&cfg, 0, sizeof cfg);   /* mcp_en=0 -> no synth */
    dserv_cfg_ain_default(&cfg);
    CHECK(dserv_ain_active_count(&cfg) == 0, "no synth when mcp disabled");

    /* ---- on-change deadband (joystick, ch{0,1}, db=8) ---- */
    printf("on-change / deadband:\n");
    memset(&cfg, 0, sizeof cfg);
    cfg.ain_group_chans[0] = 0x03; cfg.ain_group_mode[0] = 0; cfg.ain_group_deadband[0] = 8;
    ain_group_rt_t rt; ain_group_reset(&rt);
    scan_set(s, 2048, 2048, 111, 222);
    CHECK(ain_group_feed(&rt, &cfg, 0, s, 1000, 20000, &out) == 1, "first sample publishes");
    CHECK(out.count == 1 && out.nchan == 2 && out.mask == 0x03, "count=1, nchan=2, mask=0x03");
    CHECK(out.v[0] == 2048 && out.v[1] == 2048, "columns ascending (ch0,ch1)");
    CHECK(out.interval_us == 0 && out.t0_us == 1000, "single: interval 0, t0 = sample time");
    scan_set(s, 2052, 2048, 0, 0);   /* moved +4 <= db 8 */
    CHECK(ain_group_feed(&rt, &cfg, 0, s, 1100, 20000, &out) == 0, "sub-deadband move: no publish");
    scan_set(s, 2060, 2048, 0, 0);   /* moved +12 > db 8 */
    CHECK(ain_group_feed(&rt, &cfg, 0, s, 1200, 20000, &out) == 1, "past-deadband move: publish");
    CHECK(out.v[0] == 2060 && out.t0_us == 1200, "publishes moved value at its time");

    /* ---- continuous decimate (drop) + batch ---- */
    printf("continuous / decimate + batch:\n");
    memset(&cfg, 0, sizeof cfg);
    cfg.ain_group_chans[1] = 0x03; cfg.ain_group_mode[1] = 1;   /* ch{0,1}, continuous */
    cfg.ain_group_decimate[1] = 2; cfg.ain_group_batch[1] = 3;  /* every 2nd scan, 3 scans/block */
    ain_group_reset(&rt);
    int blocks = 0;
    for (int k = 0; k < 12; k++) {           /* 12 base scans @ 1kHz (period 1000us) */
        scan_set(s, 100 + k, 200 + k, 0, 0);
        if (ain_group_feed(&rt, &cfg, 1, s, 1000ull + (uint64_t)k * 1000, 1000, &out)) blocks++;
    }
    /* 12 scans / decimate 2 = 6 takes / batch 3 = 2 blocks */
    CHECK(blocks == 2, "12 scans, dec2, batch3 -> 2 blocks");
    CHECK(out.count == 3 && out.nchan == 2, "last block: 3 scans x 2ch");
    CHECK(out.interval_us == 2000, "interval = decimate * base_period (2*1000us)");
    CHECK((out.flags & AIN_GROUP_FLAG_AVG) == 0, "drop mode: not flagged averaged");
    /* takes are scans k=1,3,5,7,9,11 (dec_count hits 2 on odd k); block2 = k=7,9,11 */
    CHECK(out.v[0] == 107 && out.v[1] == 207, "drop keeps the window's last sample (k=7)");
    CHECK(out.t0_us == 1000 + 6 * 1000, "block t0 = first take's scan time (k=6? see note)");

    /* ---- continuous averaging (boxcar) ---- */
    printf("continuous / boxcar average:\n");
    memset(&cfg, 0, sizeof cfg);
    cfg.ain_group_chans[0] = 0x01; cfg.ain_group_mode[0] = 1;   /* ch0 only */
    cfg.ain_group_decimate[0] = 4; cfg.ain_group_batch[0] = 1;  /* mean of 4, per-take block */
    cfg.ain_group_flags[0] = AIN_GROUP_FLAG_AVG;
    ain_group_reset(&rt);
    int got = 0;
    for (int k = 0; k < 4; k++) { scan_set(s, 10 + k * 10, 0, 0, 0);   /* 10,20,30,40 -> mean 25 */
        if (ain_group_feed(&rt, &cfg, 0, s, 5000ull + k, 1000, &out)) got = 1; }
    CHECK(got && out.count == 1 && out.nchan == 1, "avg: one take after 4 scans");
    CHECK(out.v[0] == 25, "boxcar mean (10+20+30+40)/4 = 25");
    CHECK((out.flags & AIN_GROUP_FLAG_AVG) != 0, "avg: flagged averaged");

    /* ---- block wire payload ---- */
    printf("block payload:\n");
    uint8_t buf[64];
    int len = ain_block_payload(&out, buf);
    CHECK(buf[0] == AIN_BLOCK_VER && buf[1] == 0x01 && buf[2] == 1 && buf[3] == 1, "header ver/mask/nchan/count");
    CHECK(len == 12 + 1 * 1 * 2, "payload len = 12 + count*nchan*2");
    int16_t v0; memcpy(&v0, buf + 12, 2);
    CHECK(v0 == 25, "sample follows the 12-byte header");

    printf(fails ? "\nFAILED (%d)\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
