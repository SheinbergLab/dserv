/*
 * box_ota.h -- portable OTA receive logic: stream one firmware image into the
 * inactive slot, hashing every byte, WITHOUT ever holding the image in RAM.
 *
 * Ported from wiznet-io/pico/pico_ota.h (pico_* -> box_*), which is already
 * rig-proven on the RP2350 end to end. Kept as pure logic with no platform
 * dependency so it behaves identically on device and host:
 *
 *   - flash never touched directly -- only through the box_ota_flash_t hooks,
 *     wired on the RW612 to src/platform/box_ota_flash.c (slot1_partition).
 *     Either hook may be NULL for a sha-only dry run, which is how you separate
 *     "the bytes did not arrive intact" from "the flash write failed".
 *   - sha256 is the compact software implementation carried over from the
 *     RP2350's host/sim path. The RP2350 build could use hardware sha; the
 *     RW612 has no equivalent exposed here, and at ~640 kB per image the cost
 *     is a fraction of the ~10 s the flash writes take anyway (PORTING.md).
 *
 * DIFFERENCES FROM THE RP2350 ORIGINAL, both deliberate:
 *
 *   1. The erase granularity is a RUNTIME field, not a compile-time constant.
 *      The original hardcoded PICO_OTA_SECTOR = 4096; here it comes from
 *      flash_get_page_info_by_offs() via box_ota_flash_sector(), so the module
 *      cannot silently disagree with the device about where sector boundaries
 *      are -- which would erase the wrong span and corrupt already-written data.
 *   2. box_ota_begin() takes the sector size and validates it, rather than
 *      trusting the caller.
 *
 * The delivery front-end is separate on purpose: this same sink is fed by the
 * datapoint chunk path (cmd/ota/chunk) today and could be fed by a raw 'D'-frame
 * push or an Ethernet pull without changing a line here. See wiznet-io/OTA.md.
 */
#ifndef BOX_OTA_H
#define BOX_OTA_H

#include <stdint.h>
#include <string.h>

#define BOX_OTA_PAGE       256u    /* program granularity we buffer to */
#define BOX_OTA_SHA_BYTES  32

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
static inline void ota_sha_finish(ota_sha_t *c, uint8_t out[BOX_OTA_SHA_BYTES])
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

/* ---- flash write hooks (app-supplied) ----
 * Offsets are RELATIVE TO THE SLOT, matching box_ota_flash.h -- so a bug here
 * cannot reach slot0 or the running image.
 *   erase:   erase the sector containing `off`. 0 ok, <0 fail.
 *   program: write BOX_OTA_PAGE bytes from `page` at `off`. 0 ok, <0 fail.
 * Either may be NULL (sha-only dry run). */
typedef struct {
    int (*erase)(uint32_t off);
    int (*program)(uint32_t off, const uint8_t *page, uint32_t len);
} box_ota_flash_t;

typedef enum {
    BOX_OTA_IDLE = 0,
    BOX_OTA_STAGING,     /* streaming bytes into the inactive slot */
    BOX_OTA_VERIFY,      /* transfer done, comparing size + sha    */
    BOX_OTA_DONE_OK,     /* image received + verified in the slot  */
    BOX_OTA_DONE_FAIL,   /* aborted; see .err                      */
} box_ota_state_t;

/* fail codes (box_ota_t.err; 0 = none) */
enum {
    BOX_OTA_ERR_NONE     =  0,
    BOX_OTA_ERR_TOO_BIG  = -1,   /* image exceeds the slot            */
    BOX_OTA_ERR_FLASH    = -2,   /* an erase/program hook failed      */
    BOX_OTA_ERR_SIZE     = -3,   /* received != expected_size         */
    BOX_OTA_ERR_SHA      = -4,   /* sha256 mismatch                   */
    BOX_OTA_ERR_SHA_INIT = -5,   /* couldn't start the sha engine     */
    BOX_OTA_ERR_STATE    = -6,   /* sink/finish in the wrong state    */
    BOX_OTA_ERR_GEOM     = -7,   /* sector size not a multiple of page */
};

typedef struct {
    box_ota_state_t          state;
    const box_ota_flash_t   *flash;
    uint32_t   cap;                          /* slot size in bytes                 */
    uint32_t   sector;                       /* erase granularity (runtime)        */
    uint32_t   expected_size;
    uint8_t    expected_sha[BOX_OTA_SHA_BYTES];
    uint32_t   received;                     /* image bytes accepted so far        */
    uint32_t   write_off;                    /* running offset within the slot     */
    uint8_t    page[BOX_OTA_PAGE];
    uint16_t   page_len;                     /* bytes buffered in `page`           */
    int        err;
    ota_sha_t  sha;
    uint8_t    result_sha[BOX_OTA_SHA_BYTES];
} box_ota_t;

/* Program the buffered page at write_off, erasing the sector it opens.
 * `n` is the byte count to program (BOX_OTA_PAGE except for the padded tail). */
static inline int box_ota_flush_page(box_ota_t *o, uint32_t n)
{
    uint32_t off = o->write_off;

    if (o->sector && (off % o->sector) == 0) {          /* first page of a sector */
        if (o->flash && o->flash->erase && o->flash->erase(off) < 0) {
            o->err = BOX_OTA_ERR_FLASH; return -1;
        }
    }
    if (o->flash && o->flash->program && o->flash->program(off, o->page, n) < 0) {
        o->err = BOX_OTA_ERR_FLASH; return -1;
    }
    o->write_off += n;
    o->page_len   = 0;
    return 0;
}

/* Begin a transfer into a slot of `cap` bytes with `sector`-sized erase blocks.
 * Returns 0, or <0 with state DONE_FAIL. */
static inline int box_ota_begin(box_ota_t *o, const box_ota_flash_t *flash,
                                uint32_t cap, uint32_t sector,
                                const uint8_t expected_sha[BOX_OTA_SHA_BYTES],
                                uint32_t expected_size)
{
    memset(o, 0, sizeof *o);
    o->flash  = flash;
    o->cap    = cap;
    o->sector = sector;
    o->expected_size = expected_size;
    memcpy(o->expected_sha, expected_sha, BOX_OTA_SHA_BYTES);

    /* erase-as-you-go only lands on sector boundaries if a sector is a whole
     * number of pages; otherwise a page would straddle two sectors and the
     * second erase would wipe bytes already written. Refuse rather than corrupt. */
    if (sector == 0 || (sector % BOX_OTA_PAGE) != 0) {
        o->state = BOX_OTA_DONE_FAIL; o->err = BOX_OTA_ERR_GEOM; return -1;
    }
    if (expected_size == 0 || expected_size > cap) {
        o->state = BOX_OTA_DONE_FAIL; o->err = BOX_OTA_ERR_TOO_BIG; return -1;
    }
    if (ota_sha_start(&o->sha) != 0) {
        o->state = BOX_OTA_DONE_FAIL; o->err = BOX_OTA_ERR_SHA_INIT; return -1;
    }
    o->state = BOX_OTA_STAGING;
    return 0;
}

/* Feed the next contiguous span of image bytes. Accumulates whole pages,
 * erasing as it opens each sector, and hashes every byte.
 * 0 = keep going, <0 = abort. */
static inline int box_ota_sink(box_ota_t *o, const uint8_t *data, uint32_t len)
{
    if (o->state != BOX_OTA_STAGING) { o->err = BOX_OTA_ERR_STATE; return -1; }
    if (o->received + len > o->cap)  { o->err = BOX_OTA_ERR_TOO_BIG; return -1; }

    ota_sha_update(&o->sha, data, len);
    while (len) {
        uint32_t take = BOX_OTA_PAGE - o->page_len;

        if (take > len) take = len;
        memcpy(o->page + o->page_len, data, take);
        o->page_len += (uint16_t) take;
        data += take; len -= take; o->received += take;
        if (o->page_len == BOX_OTA_PAGE && box_ota_flush_page(o, BOX_OTA_PAGE) < 0) {
            return -1;
        }
    }
    return 0;
}

/* Close the transfer: flush the short tail page, finish the hash, check size
 * and sha. `xfer_ok` < 0 marks a transport-level failure so a truncated push
 * fails even if the partial image happens to look sane. */
static inline void box_ota_finish(box_ota_t *o, int xfer_ok)
{
    if (o->state != BOX_OTA_STAGING) return;

    if (o->page_len) {
        uint32_t n = o->page_len;

        memset(o->page + o->page_len, 0xFF, BOX_OTA_PAGE - o->page_len);
        if (box_ota_flush_page(o, n) < 0) {
            ota_sha_cleanup(&o->sha); o->state = BOX_OTA_DONE_FAIL; return;
        }
    }
    o->state = BOX_OTA_VERIFY;
    ota_sha_finish(&o->sha, o->result_sha);
    ota_sha_cleanup(&o->sha);

    if (o->err)                          { o->state = BOX_OTA_DONE_FAIL; return; }
    if (xfer_ok < 0)                     { o->err = BOX_OTA_ERR_SIZE; o->state = BOX_OTA_DONE_FAIL; return; }
    if (o->received != o->expected_size) { o->err = BOX_OTA_ERR_SIZE; o->state = BOX_OTA_DONE_FAIL; return; }
    if (memcmp(o->result_sha, o->expected_sha, BOX_OTA_SHA_BYTES) != 0) {
        o->err = BOX_OTA_ERR_SHA; o->state = BOX_OTA_DONE_FAIL; return;
    }
    o->state = BOX_OTA_DONE_OK;
}

static inline int box_ota_progress_pct(const box_ota_t *o)
{ return o->expected_size ? (int)((uint64_t) o->received * 100u / o->expected_size) : 0; }

static inline const char *box_ota_state_str(box_ota_state_t s)
{
    switch (s) {
        case BOX_OTA_IDLE:      return "idle";
        case BOX_OTA_STAGING:   return "staging";
        case BOX_OTA_VERIFY:    return "verify";
        case BOX_OTA_DONE_OK:   return "ok";
        case BOX_OTA_DONE_FAIL: return "fail";
        default:                return "?";
    }
}

static inline const char *box_ota_err_str(int err)
{
    switch (err) {
        case BOX_OTA_ERR_NONE:     return "none";
        case BOX_OTA_ERR_TOO_BIG:  return "too_big";
        case BOX_OTA_ERR_FLASH:    return "flash";
        case BOX_OTA_ERR_SIZE:     return "size_mismatch";
        case BOX_OTA_ERR_SHA:      return "sha_mismatch";
        case BOX_OTA_ERR_SHA_INIT: return "sha_init";
        case BOX_OTA_ERR_STATE:    return "bad_state";
        case BOX_OTA_ERR_GEOM:     return "bad_geometry";
        default:                   return "?";
    }
}

/* Parse 64 hex chars into 32 bytes (cmd/ota/begin carries the sha as hex). */
static inline int box_ota_parse_sha(const char *hex, uint8_t out[BOX_OTA_SHA_BYTES])
{
    for (int i = 0; i < BOX_OTA_SHA_BYTES; i++) {
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

#endif /* BOX_OTA_H */
