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
#define PICO_PERSIST_VERSION  2   /* v2: added per-pin debounce_ms */
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

/* Validate + load a blob into cfg. Returns 0 on success, -1 if invalid. */
static inline int pico_persist_deserialize(const uint8_t *buf, uint32_t len,
                                           pico_config_t *cfg)
{
    uint32_t clen = (uint32_t) sizeof *cfg;
    if (len < 4 + 2 + 2 + clen + 4) return -1;

    uint32_t magic; memcpy(&magic, buf, 4);
    uint16_t ver;   memcpy(&ver, buf + 4, 2);
    uint16_t blen;  memcpy(&blen, buf + 6, 2);
    if (magic != PICO_PERSIST_MAGIC || ver != PICO_PERSIST_VERSION || blen != clen)
        return -1;

    uint32_t want; memcpy(&want, buf + 8 + clen, 4);
    if (pico_crc32(buf, 8 + clen) != want) return -1;

    memcpy(cfg, buf + 8, clen);
    return 0;
}

#endif /* PICO_PERSIST_H */
