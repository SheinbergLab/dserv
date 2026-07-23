/*
 * box_ain_group.h -- sampling/packing state machine for MCP3204 ANALOG groups.
 *
 * The analog twin of box_group.h (DI chord groups). A group is a named set of
 * ADC channels (box_config_t.ain_group_chans[g]) published together under one
 * policy, so a host reshapes the samples from the group's announced channel list
 * alone -- the same "decode from the announced string" contract the DI groups
 * use. What DIFFERS from DI: DI groups are edge-driven (a settle machine); ADC
 * is clocked, so the settle machine is replaced by (a) deadband-onset detection
 * in on-change mode and (b) decimate+batch packing in continuous mode.
 *
 * The box scans the full channel set once per base tick (mcp_rate); each group
 * is fed that scan and decides what to emit:
 *   on-change  -> a count=1 block whenever a member moves > deadband (joystick).
 *                 Stamped at that sample (the movement is the event).
 *   continuous -> take one sample every `decimate` base scans (drop, or boxcar
 *                 MEAN of the window if AIN_GROUP_FLAG_AVG), accumulate `batch`
 *                 takes, then emit ONE self-describing block. interval_us =
 *                 decimate * base_period; t0 (block) = the first take's scan time.
 *
 * Wire block (DSERV_BYTE payload; built on core 1 from ain_block_t) -- 12-byte
 * header so it stands alone, then scan-major int16 samples in ASCENDING channel
 * order (the column contract):
 *   off size field
 *   0   1    ver (0x01)
 *   1   1    mask        channels present, bit c = ch c   (identity)
 *   2   1    nchan       popcount(mask)
 *   3   1    count       scans in this block (1 for on-change / per-scan)
 *   4   4    interval_us spacing between scans (0 if count==1)
 *   8   2    flags       bit0 = averaged; rest reserved
 *   10  2    reserved    0
 *   12  ...  int16[count*nchan]   scan-major, ascending channel
 * The frame timestamp carries t0. Sample k of column c is at t0 + k*interval_us.
 *
 * Pure C, zero-alloc, no hardware; feed takes caller timestamps so the whole
 * machine unit-tests on the host (host/ain_group_test.c).
 */
#ifndef BOX_AIN_GROUP_H
#define BOX_AIN_GROUP_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "dserv_config.h"

#define AIN_MAX_CH     4    /* MCP3204: 4 single-ended channels */
#define AIN_BLOCK_MAX  24   /* max int16 samples per block: 12B header + 48B + a
                             * long "extio/<name>/ain/<label>" varname must fit
                             * the 128B frame (varlen+datalen <= 109). */
#define AIN_BLOCK_VER  0x01

/* One packed block handed core0 -> core1 (g_ain_q). core1 builds the DSERV_BYTE
 * frame (name from dserv_ain_group_leaf(gidx)) and sends it. */
typedef struct {
    uint64_t t0_us;                 /* sample instant of scan 0 (on-change: the event sample) */
    uint8_t  gidx;                  /* source group index -> leaf/name on core 1 */
    uint8_t  mask;                  /* channel mask (ascending = column order) */
    uint8_t  nchan;                 /* popcount(mask) */
    uint8_t  count;                 /* scans in this block */
    uint32_t interval_us;           /* spacing between scans (0 if count==1) */
    uint16_t flags;                 /* bit0 = averaged */
    int16_t  v[AIN_BLOCK_MAX];      /* count*nchan samples, scan-major ascending channel */
} ain_block_t;

/* Per-group runtime (one per BOX_NAGROUPS slot; lives on core 0). */
typedef struct {
    uint8_t  dec_count;             /* base scans accumulated toward the next take */
    int32_t  acc[AIN_MAX_CH];       /* boxcar sums for the current decimate window (avg) */
    int16_t  ring[AIN_BLOCK_MAX];   /* pending block samples (scan-major) */
    uint8_t  ring_scans;            /* takes currently in ring */
    uint64_t block_t0_us;           /* t0 of the pending block (first take's scan time) */
    int16_t  last_pub[AIN_MAX_CH];  /* on-change: last published per-column values */
    uint8_t  have_last;             /* on-change: last_pub valid */
} ain_group_rt_t;

static inline void ain_group_reset(ain_group_rt_t *rt) { memset(rt, 0, sizeof *rt); }

/* Extract a group's channels (ascending) from a full scan into a compact column
 * array; returns nchan. */
static inline int ain_extract(uint8_t mask, const int16_t scan[AIN_MAX_CH], int16_t out[AIN_MAX_CH])
{
    int n = 0;
    for (int ch = 0; ch < AIN_MAX_CH; ch++)
        if ((mask >> ch) & 1u) out[n++] = scan[ch];
    return n;
}

/* effective batch for group g, clamped so count*nchan fits AIN_BLOCK_MAX. */
static inline int ain_group_batch_eff(const box_config_t *c, int g, int nchan)
{
    int batch = c->ain_group_batch[g] ? c->ain_group_batch[g] : 1;
    if (nchan > 0 && batch * nchan > AIN_BLOCK_MAX) batch = AIN_BLOCK_MAX / nchan;
    return batch < 1 ? 1 : batch;
}

/* Feed one full base scan (scan[AIN_MAX_CH] sampled at t_us; base_period_us =
 * 1e6 / base_rate) to analog group g. Returns 1 and fills *out when a block is
 * ready to publish, else 0. Inactive groups (mask 0) return 0. */
static inline int ain_group_feed(ain_group_rt_t *rt, const box_config_t *c, int g,
                                 const int16_t scan[AIN_MAX_CH], uint64_t t_us,
                                 uint32_t base_period_us, ain_block_t *out)
{
    uint8_t mask = c->ain_group_chans[g];
    if (!mask) return 0;
    int16_t col[AIN_MAX_CH];
    int nchan = ain_extract(mask, scan, col);
    if (nchan <= 0) return 0;

    if (!c->ain_group_mode[g]) {                     /* ---- on-change (deadband) ---- */
        int moved = !rt->have_last;
        for (int i = 0; i < nchan && !moved; i++)
            if (abs((int) col[i] - rt->last_pub[i]) > (int) c->ain_group_deadband[g]) moved = 1;
        if (!moved) return 0;
        for (int i = 0; i < nchan; i++) rt->last_pub[i] = col[i];
        rt->have_last = 1;
        out->t0_us = t_us; out->gidx = (uint8_t) g; out->mask = mask;
        out->nchan = (uint8_t) nchan; out->count = 1; out->interval_us = 0; out->flags = 0;
        for (int i = 0; i < nchan; i++) out->v[i] = col[i];
        return 1;
    }

    /* ---- continuous: decimate (drop or boxcar avg) into the batch ring ---- */
    int dec = c->ain_group_decimate[g] ? c->ain_group_decimate[g] : 1;
    int avg = (c->ain_group_flags[g] & AIN_GROUP_FLAG_AVG) != 0;
    if (rt->dec_count == 0 && rt->ring_scans == 0) rt->block_t0_us = t_us;  /* first take's t0 */
    if (avg) for (int i = 0; i < nchan; i++) rt->acc[i] += col[i];
    if (++rt->dec_count < dec) return 0;             /* decimate window not full */

    int base = rt->ring_scans * nchan;               /* append one take */
    for (int i = 0; i < nchan; i++)
        rt->ring[base + i] = avg ? (int16_t)(rt->acc[i] / dec) : col[i];
    for (int i = 0; i < nchan; i++) rt->acc[i] = 0;
    rt->dec_count = 0;
    rt->ring_scans++;

    int batch = ain_group_batch_eff(c, g, nchan);
    if (rt->ring_scans < batch) return 0;            /* block not full yet */

    out->t0_us = rt->block_t0_us; out->gidx = (uint8_t) g; out->mask = mask;
    out->nchan = (uint8_t) nchan; out->count = rt->ring_scans;
    out->interval_us = (uint32_t) dec * base_period_us;
    out->flags = avg ? AIN_GROUP_FLAG_AVG : 0;
    for (int i = 0; i < rt->ring_scans * nchan; i++) out->v[i] = rt->ring[i];
    rt->ring_scans = 0;
    return 1;
}

/* Serialize an ain_block_t into the 12-byte-header DSERV_BYTE payload; returns
 * the payload length. buf must hold 12 + count*nchan*2 bytes. */
static inline int ain_block_payload(const ain_block_t *b, uint8_t *buf)
{
    buf[0] = AIN_BLOCK_VER; buf[1] = b->mask; buf[2] = b->nchan; buf[3] = b->count;
    memcpy(buf + 4, &b->interval_us, 4);
    memcpy(buf + 8, &b->flags, 2);
    buf[10] = 0; buf[11] = 0;
    int nbytes = (int) b->count * b->nchan * 2;
    memcpy(buf + 12, b->v, (size_t) nbytes);
    return 12 + nbytes;
}

#endif /* BOX_AIN_GROUP_H */
