/*
 * pico_ota.h -- Stage-0 OTA receiver: pull one firmware image over the dserv
 * link, stream it to a scratch flash region, and verify sha256 -- WITHOUT ever
 * buffering the ~150KB image in SRAM. This is DELIVERY PROOF only (OTA.md
 * "Stage 0"): receive -> scratch flash -> verify sha -> report. No partition
 * table, no A/B slots, no boot switch yet (that's Stage 1+).
 *
 * SPLIT OF RESPONSIBILITY (why this header owns only the pure logic):
 *   - The bytes arrive via box_net_get_binary(key, sink), which runs on the RT
 *     core (core 1: it owns the W6300 SPI). Its `sink` is called synchronously
 *     in the recv loop -- so pico_ota_sink() ALSO runs on core 1.
 *   - Flash erase/program CANNOT run on core 1 in these builds: they use
 *     copy_to_ram + PICO_FLASH_ASSUME_CORE1_SAFE, under which flash_safe_execute
 *     called from core 1 returns PICO_ERROR_NOT_PERMITTED (it assumes core 0 is
 *     the actor). That is exactly why `save` marshals to core 0 via g_save_q.
 *   So this module NEVER touches flash itself. It calls out through
 *   pico_ota_flash_t hooks that the app wires to a core-0 flash op (the
 *   save-queue pattern). On the host/sim the hooks point at a RAM buffer, so the
 *   whole sink+sha path unit-tests off-target.
 *
 * The app (wizchip_dserv_config.c) drives it: on `cmd/ota/begin <sha256 size>`
 * (gated to !in_obs), pico_ota_begin(); box_net_get_binary(key, pico_ota_sink,
 * &ota); pico_ota_finish(); then publishes state/ota/{state,progress,result}.
 *
 * Pure C, zero heap; the sink/sha/geometry logic has no hardware dependency
 * beyond the flash constants (device) or their fallbacks (host).
 */
#ifndef PICO_OTA_H
#define PICO_OTA_H

#include <stdint.h>
#include <string.h>

#ifdef PICO_ON_DEVICE
#include "pico_flash.h"   /* FLASH_STORE_OFFSET + hardware/flash.h geometry */
#define PICO_OTA_PAGE     FLASH_PAGE_SIZE      /* 256  -- program granularity */
#define PICO_OTA_SECTOR   FLASH_SECTOR_SIZE    /* 4096 -- erase granularity   */
/* Scratch = a sector-aligned span just BELOW the persist sector, big enough for
 * a ~150KB image with generous margin. Stays clear of FLASH_STORE_OFFSET (the
 * config blob) and, having no partition table yet, of the running image low in
 * flash. Stage 1 replaces this with the inactive A/B slot. */
#define PICO_OTA_SCRATCH_BYTES  (256u * 1024u)
#define PICO_OTA_SCRATCH_OFFSET (FLASH_STORE_OFFSET - PICO_OTA_SCRATCH_BYTES)
_Static_assert(PICO_OTA_SCRATCH_BYTES % PICO_OTA_SECTOR == 0,  "scratch not sector-aligned");
_Static_assert(PICO_OTA_SCRATCH_OFFSET % PICO_OTA_SECTOR == 0, "scratch base not sector-aligned");
#else
/* Host / sim: no real flash. Fixed geometry so the logic compiles + tests. */
#define PICO_OTA_PAGE     256u
#define PICO_OTA_SECTOR   4096u
#define PICO_OTA_SCRATCH_BYTES  (256u * 1024u)
#define PICO_OTA_SCRATCH_OFFSET 0u
#endif

#define PICO_OTA_SHA_BYTES 32

/* ---- sha256 backend: RP2350 hardware on device, a compact software fallback
 *      on the host so the module is self-contained + unit-testable. ---- */
#if defined(PICO_ON_DEVICE) && !defined(PICO_OTA_SOFT_SHA)
#include "pico/sha256.h"
typedef pico_sha256_state_t ota_sha_t;
static inline int  ota_sha_start(ota_sha_t *s)
{ return pico_sha256_try_start(s, SHA256_BIG_ENDIAN, true) == PICO_OK ? 0 : -1; }
static inline void ota_sha_update(ota_sha_t *s, const void *d, uint32_t n)
{ pico_sha256_update(s, (const uint8_t *) d, n); }
static inline void ota_sha_finish(ota_sha_t *s, uint8_t out[PICO_OTA_SHA_BYTES])
{ sha256_result_t r; pico_sha256_finish(s, &r); memcpy(out, r.bytes, PICO_OTA_SHA_BYTES); }
static inline void ota_sha_cleanup(ota_sha_t *s) { pico_sha256_cleanup(s); }
#else
typedef struct { uint32_t s[8]; uint64_t len; uint8_t buf[64]; uint32_t n; } ota_sha_t;
static const uint32_t ota_sha_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
static inline uint32_t ota_ror(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }
static inline void ota_sha_block(ota_sha_t *c, const uint8_t *p)
{
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t) p[i*4] << 24) | ((uint32_t) p[i*4+1] << 16) |
               ((uint32_t) p[i*4+2] << 8) | (uint32_t) p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ota_ror(w[i-15],7) ^ ota_ror(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ota_ror(w[i-2],17) ^ ota_ror(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=c->s[0]; b=c->s[1]; cc=c->s[2]; d=c->s[3]; e=c->s[4]; f=c->s[5]; g=c->s[6]; h=c->s[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ota_ror(e,6) ^ ota_ror(e,11) ^ ota_ror(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + ota_sha_k[i] + w[i];
        uint32_t S0 = ota_ror(a,2) ^ ota_ror(a,13) ^ ota_ror(a,22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->s[0]+=a; c->s[1]+=b; c->s[2]+=cc; c->s[3]+=d; c->s[4]+=e; c->s[5]+=f; c->s[6]+=g; c->s[7]+=h;
}
static inline int ota_sha_start(ota_sha_t *c)
{
    c->s[0]=0x6a09e667; c->s[1]=0xbb67ae85; c->s[2]=0x3c6ef372; c->s[3]=0xa54ff53a;
    c->s[4]=0x510e527f; c->s[5]=0x9b05688c; c->s[6]=0x1f83d9ab; c->s[7]=0x5be0cd19;
    c->len = 0; c->n = 0; return 0;
}
static inline void ota_sha_update(ota_sha_t *c, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *) data;
    c->len += len;
    while (len) {
        uint32_t k = 64 - c->n; if (k > len) k = len;
        memcpy(c->buf + c->n, p, k); c->n += k; p += k; len -= k;
        if (c->n == 64) { ota_sha_block(c, c->buf); c->n = 0; }
    }
}
static inline void ota_sha_finish(ota_sha_t *c, uint8_t out[PICO_OTA_SHA_BYTES])
{
    uint64_t bits = c->len * 8;               /* capture BEFORE padding grows len */
    uint8_t one = 0x80, zero = 0, L[8];
    ota_sha_update(c, &one, 1);
    while (c->n != 56) ota_sha_update(c, &zero, 1);
    for (int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - 8*i));
    ota_sha_update(c, L, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(c->s[i] >> 24); out[i*4+1] = (uint8_t)(c->s[i] >> 16);
        out[i*4+2] = (uint8_t)(c->s[i] >> 8);  out[i*4+3] = (uint8_t)(c->s[i]);
    }
}
static inline void ota_sha_cleanup(ota_sha_t *c) { (void) c; }
#endif /* sha backend */

/* ---- flash write hooks (app-supplied; run on core 0 in the real box) ----
 * Both take an ABSOLUTE flash offset (XIP-relative, as flash_range_* want).
 * erase:   erase the PICO_OTA_SECTOR-sized sector at `off`. 0 ok, <0 fail.
 * program: program one PICO_OTA_PAGE-sized page from `page` at `off`. 0 ok, <0.
 * Either may be NULL (sha-only dry run: verify the bytes crossed the link
 * without committing them to flash -- handy for first bring-up). */
typedef struct {
    int (*erase)(uint32_t off);
    int (*program)(uint32_t off, const uint8_t *page);
} pico_ota_flash_t;

typedef enum {
    PICO_OTA_IDLE = 0,
    PICO_OTA_STAGING,     /* streaming bytes to scratch                    */
    PICO_OTA_VERIFY,      /* transfer done, comparing sha/size             */
    PICO_OTA_DONE_OK,     /* image received + verified in scratch          */
    PICO_OTA_DONE_FAIL,   /* aborted; see .err                             */
} pico_ota_state_t;

/* fail codes (pico_ota_t.err; 0 = none) */
enum {
    PICO_OTA_ERR_NONE     =  0,
    PICO_OTA_ERR_TOO_BIG  = -1,   /* image exceeds PICO_OTA_SCRATCH_BYTES  */
    PICO_OTA_ERR_FLASH    = -2,   /* an erase/program hook failed          */
    PICO_OTA_ERR_SIZE     = -3,   /* received != expected_size             */
    PICO_OTA_ERR_SHA      = -4,   /* sha256 mismatch                       */
    PICO_OTA_ERR_SHA_INIT = -5,   /* couldn't start the sha engine         */
    PICO_OTA_ERR_STATE    = -6,   /* sink/finish called in the wrong state */
};

typedef struct {
    pico_ota_state_t         state;
    const pico_ota_flash_t  *flash;
    uint32_t   expected_size;
    uint8_t    expected_sha[PICO_OTA_SHA_BYTES];
    uint32_t   received;                     /* real image bytes streamed so far */
    uint32_t   write_off;                    /* running offset WITHIN scratch    */
    uint8_t    page[PICO_OTA_PAGE];
    uint16_t   page_len;                     /* bytes buffered in `page`         */
    int        err;
    ota_sha_t  sha;
    uint8_t    result_sha[PICO_OTA_SHA_BYTES];
} pico_ota_t;

/* Program the currently-buffered page to scratch; erase the sector first when
 * this page opens one. `page` must be full (caller pads the final short page). */
static inline int pico_ota_flush_page(pico_ota_t *o)
{
    uint32_t off = PICO_OTA_SCRATCH_OFFSET + o->write_off;
    if ((o->write_off % PICO_OTA_SECTOR) == 0)                 /* first page of a sector */
        if (o->flash && o->flash->erase && o->flash->erase(off) < 0) { o->err = PICO_OTA_ERR_FLASH; return -1; }
    if (o->flash && o->flash->program && o->flash->program(off, o->page) < 0) { o->err = PICO_OTA_ERR_FLASH; return -1; }
    o->write_off += PICO_OTA_PAGE;
    o->page_len = 0;
    return 0;
}

/* Begin a transfer. expected_sha/expected_size come from cmd/ota/begin.
 * Returns 0, or <0 (and state=DONE_FAIL) if the sha engine won't start. */
static inline int pico_ota_begin(pico_ota_t *o, const pico_ota_flash_t *flash,
                                 const uint8_t expected_sha[PICO_OTA_SHA_BYTES],
                                 uint32_t expected_size)
{
    memset(o, 0, sizeof *o);
    o->flash = flash;
    o->expected_size = expected_size;
    memcpy(o->expected_sha, expected_sha, PICO_OTA_SHA_BYTES);
    if (ota_sha_start(&o->sha) != 0) { o->state = PICO_OTA_DONE_FAIL; o->err = PICO_OTA_ERR_SHA_INIT; return -1; }
    o->state = PICO_OTA_STAGING;
    return 0;
}

/* box_net_bin_sink: fed successive spans of the image's raw value on core 1.
 * Accumulates into whole flash pages (erase-as-you-go), hashes every byte.
 * 0 = keep going, <0 = abort (get_binary tears the socket down). */
static inline int pico_ota_sink(void *ud, const uint8_t *data, uint32_t len)
{
    pico_ota_t *o = (pico_ota_t *) ud;
    if (o->state != PICO_OTA_STAGING) { o->err = PICO_OTA_ERR_STATE; return -1; }
    if (o->received + len > PICO_OTA_SCRATCH_BYTES) { o->err = PICO_OTA_ERR_TOO_BIG; return -1; }
    ota_sha_update(&o->sha, data, len);
    while (len) {
        uint32_t take = PICO_OTA_PAGE - o->page_len;
        if (take > len) take = len;
        memcpy(o->page + o->page_len, data, take);
        o->page_len += (uint16_t) take;
        data += take; len -= take; o->received += take;
        if (o->page_len == PICO_OTA_PAGE && pico_ota_flush_page(o) < 0) return -1;
    }
    return 0;
}

/* Close out the transfer: flush the final short page (0xFF-padded), finish the
 * hash, then check size + sha. Sets state to DONE_OK or DONE_FAIL. Idempotent-ish:
 * only acts from STAGING. `xfer_ok` is the box_net_get_binary return sign (>=0
 * datalen, <0 transport error) so a truncated pull fails even if the partial
 * happens to look sane. */
static inline void pico_ota_finish(pico_ota_t *o, int xfer_ok)
{
    if (o->state != PICO_OTA_STAGING) return;
    if (o->page_len) {                                        /* pad + write the tail page */
        memset(o->page + o->page_len, 0xFF, PICO_OTA_PAGE - o->page_len);
        if (pico_ota_flush_page(o) < 0) { ota_sha_cleanup(&o->sha); o->state = PICO_OTA_DONE_FAIL; return; }
    }
    o->state = PICO_OTA_VERIFY;
    ota_sha_finish(&o->sha, o->result_sha);
    ota_sha_cleanup(&o->sha);
    if (o->err)                                    { o->state = PICO_OTA_DONE_FAIL; return; }
    if (xfer_ok < 0)                               { o->err = PICO_OTA_ERR_SIZE; o->state = PICO_OTA_DONE_FAIL; return; }
    if (o->received != o->expected_size)           { o->err = PICO_OTA_ERR_SIZE; o->state = PICO_OTA_DONE_FAIL; return; }
    if (memcmp(o->result_sha, o->expected_sha, PICO_OTA_SHA_BYTES) != 0)
                                                   { o->err = PICO_OTA_ERR_SHA;  o->state = PICO_OTA_DONE_FAIL; return; }
    o->state = PICO_OTA_DONE_OK;
}

/* 0..100 for state/ota/progress (0 when the size is unknown). */
static inline int pico_ota_progress_pct(const pico_ota_t *o)
{ return o->expected_size ? (int)((uint64_t) o->received * 100u / o->expected_size) : 0; }

static inline const char *pico_ota_state_str(pico_ota_state_t s)
{
    switch (s) {
        case PICO_OTA_IDLE:      return "idle";
        case PICO_OTA_STAGING:   return "staging";
        case PICO_OTA_VERIFY:    return "verify";
        case PICO_OTA_DONE_OK:   return "ok";
        case PICO_OTA_DONE_FAIL: return "fail";
        default:                 return "?";
    }
}

static inline const char *pico_ota_err_str(int err)
{
    switch (err) {
        case PICO_OTA_ERR_NONE:     return "none";
        case PICO_OTA_ERR_TOO_BIG:  return "too_big";
        case PICO_OTA_ERR_FLASH:    return "flash";
        case PICO_OTA_ERR_SIZE:     return "size_mismatch";
        case PICO_OTA_ERR_SHA:      return "sha_mismatch";
        case PICO_OTA_ERR_SHA_INIT: return "sha_init";
        case PICO_OTA_ERR_STATE:    return "bad_state";
        default:                    return "?";
    }
}

/* Parse a 64-char lowercase/uppercase hex sha256 into 32 bytes. 0 ok, -1 on any
 * non-hex or short input (cmd/ota/begin carries the sha as hex text). */
static inline int pico_ota_parse_sha(const char *hex, uint8_t out[PICO_OTA_SHA_BYTES])
{
    for (int i = 0; i < PICO_OTA_SHA_BYTES; i++) {
        int hi = hex[i*2], lo = hex[i*2 + 1], v = 0;
        for (int k = 0; k < 2; k++) {
            int ch = k ? lo : hi, d;
            if      (ch >= '0' && ch <= '9') d = ch - '0';
            else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
            else return -1;
            v = (v << 4) | d;
        }
        out[i] = (uint8_t) v;
    }
    return 0;
}

#endif /* PICO_OTA_H */
