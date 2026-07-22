/*
 * pico_persist.h -- portable serialize/validate for pico_config_t.
 *
 * Pure C (stdint/string), no hardware. The STORAGE is platform-specific:
 *   - device: RP2350 flash (pico/pico_flash.h, flash_range_program)
 *   - sim:    a file (sim/pico_sim.c)
 * This module only turns a pico_config_t into a versioned, CRC-checked blob and
 * back, so the save/load logic is identical and testable on the Mac.
 *
 * Blob layout:  [magic u32][version u16][len u16][ pico_config_t bytes ][crc32 u32]
 * crc32 covers everything before the trailing crc.
 *
 * NB: the blob is only ever written and read on the SAME platform (device
 * flash, or sim file) -- we store the raw struct, so cross-platform blobs are
 * not portable (fine: a sim-saved file isn't loaded on the board). magic+
 * version+len+crc reject a stale/corrupt/foreign blob and fall back to defaults.
 */
#ifndef PICO_PERSIST_H
#define PICO_PERSIST_H

#include "dserv_config.h"
#include <stdint.h>
#include <string.h>

#define PICO_PERSIST_MAGIC    0x57494F31u   /* "WIO1" */
#define PICO_PERSIST_VERSION  21  /* v21: static net gateway + subnet mask (net_gw/net_sn).
                                   * v20: MCP3204 analog groups (mcp_rate + ain_group_*).
                                   * v19: channel (firmware update track, default dev).
                                   * v18: mcp_en (MCP3204 SPI analog-in).
                                   * v17: ble_latency (idle peripheral-latency target).
                                   * v16: pipe_en (receiver relay auto-arm). v15: ble_en (BLE central
                                   * on radio builds). v14: sync_pin/sync_en (TTL obs-sync input).
                                   * v13: desc + pin labels + DI chord groups.
                                   * v12: oled_en. v11: transport_mode (dual). v10: ain_en. Fields are
                                   * APPEND-ONLY: never reorder/resize existing ones, so an older
                                   * (shorter) blob loads forward-compatibly with new fields
                                   * defaulted (0 = off/none). */
#define PICO_PERSIST_BLOB_MAX  (12 + sizeof(pico_config_t))  /* hdr+struct+crc */

static inline uint32_t pico_crc32(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

/* Serialize cfg into buf. Returns blob length, or 0 if buf too small. */
static inline uint32_t pico_persist_serialize(const pico_config_t *cfg,
                                              uint8_t *buf, uint32_t bufsz)
{
    uint32_t clen = (uint32_t) sizeof *cfg;
    uint32_t total = 4 + 2 + 2 + clen + 4;
    if (bufsz < total) return 0;

    uint8_t *p = buf;
    uint32_t magic = PICO_PERSIST_MAGIC; memcpy(p, &magic, 4); p += 4;
    uint16_t ver = PICO_PERSIST_VERSION; memcpy(p, &ver, 2);   p += 2;
    uint16_t len = (uint16_t) clen;      memcpy(p, &len, 2);   p += 2;
    memcpy(p, cfg, clen);                                      p += clen;
    uint32_t crc = pico_crc32(buf, (uint32_t)(p - buf));       memcpy(p, &crc, 4);
    return total;
}

/* Validate + load a blob into cfg. Returns 0 on success, -1 if invalid.
 *
 * Forward-compatible: a blob written by an OLDER firmware (same magic, ver <=
 * ours, struct only ever grew by appends) loads fine -- we copy the bytes it has
 * and zero-default the fields it predates. So flashing a new firmware over an old
 * saved config keeps name/net/dserv/etc. instead of wiping to defaults. A blob
 * from a NEWER firmware (ver too high, or struct bigger than ours) is rejected. */
static inline int pico_persist_deserialize(const uint8_t *buf, uint32_t len,
                                           pico_config_t *cfg)
{
    uint32_t clen = (uint32_t) sizeof *cfg;
    if (len < 12) return -1;                             /* magic+ver+len+crc minimum */

    uint32_t magic; memcpy(&magic, buf, 4);
    uint16_t ver;   memcpy(&ver, buf + 4, 2);
    uint16_t blen;  memcpy(&blen, buf + 6, 2);
    if (magic != PICO_PERSIST_MAGIC) return -1;
    if (ver > PICO_PERSIST_VERSION) return -1;           /* newer format we can't read */
    if (blen > clen) return -1;                          /* struct only appends; bigger = incompatible */
    if (len < (uint32_t) 8 + blen + 4) return -1;        /* truncated */

    uint32_t want; memcpy(&want, buf + 8 + blen, 4);
    if (pico_crc32(buf, 8 + blen) != want) return -1;

    memcpy(cfg, buf + 8, blen);                          /* saved fields */
    if (blen < clen) memset((uint8_t *) cfg + blen, 0, clen - blen);  /* default the rest */
    return 0;
}

#endif /* PICO_PERSIST_H */
