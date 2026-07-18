/*
 * dserv_msg.h -- portable builder for dserv's 128-byte binary datapoint message.
 *
 * Dependency-free (stdint + string only). Works on any little-endian target:
 * RP2350 / Pico, Teensy, x86_64, Apple silicon. dserv reads the fields with raw
 * casts (src/Dataserver.cpp '>' handler) and packs them the same way
 * (src/Datapoint.c dpoint_to_binary), so NO byte-swapping is done here.
 *
 * This is the same wire format the Teensy and eye-tracker use to pump datapoints
 * into dserv over a TCP connection. Build a frame, then write all 128 bytes to a
 * TCP socket connected to dserv's client port.
 *
 * Frame (fixed DSERV_MSG_LEN = 128 bytes; always send the whole thing, the
 * receiver always reads 127 bytes after the '>'):
 *
 *   off  size  field
 *   0    1     '>'                 DSERV_MSG_CHAR
 *   1    2     uint16 varlen       length of name (no NUL)
 *   3    v     name[varlen]
 *   ..   8     uint64 timestamp    0 => dserv stamps now() on arrival
 *   ..   4     uint32 datatype     ds_datatype_t (see below)
 *   ..   4     uint32 datalen      number of data bytes
 *   ..   d     data[datalen]
 *   ..   -     zero padding to byte 127
 *
 * Constraint: varlen + datalen <= DSERV_MSG_MAX_PAYLOAD (109).
 * Builders return DSERV_MSG_LEN (128) on success, or -1 if it won't fit.
 */
#ifndef DSERV_MSG_H
#define DSERV_MSG_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>      /* strtol/strtod for string-typed values */

#define DSERV_MSG_CHAR  '>'
/* Second start-marker: an OTA data frame carries raw firmware bytes (USB chunk
 * push), not a datapoint. Same fixed 128-byte framing, so the framer + resync are
 * unchanged -- it just accepts this as a frame start too, and the frame handler
 * dispatches on byte 0. See wiznet-io/OTA.md "USB OTA". */
#define DSERV_OTA_CHAR  'D'
#define DSERV_MSG_LEN   128
/* 128 - 1('>') - 2(varlen) - 8(ts) - 4(type) - 4(len) */
#define DSERV_MSG_MAX_PAYLOAD (DSERV_MSG_LEN - 19)   /* = 109, for varname+data */

/* Subset of ds_datatype_t (src/Datapoint.h). Define DSERV_MSG_NO_DATATYPES
 * before including this in a translation unit that already pulls in dserv's
 * Datapoint.h, to avoid clashing with its enum. */
#ifndef DSERV_MSG_NO_DATATYPES
enum {
    DSERV_BYTE   = 0,
    DSERV_STRING = 1,
    DSERV_FLOAT  = 2,
    DSERV_DOUBLE = 3,
    DSERV_SHORT  = 4,
    DSERV_INT    = 5,
    DSERV_EVT    = 9,
    DSERV_INT64  = 16
};
#endif

/* Core builder. `frame` must be at least DSERV_MSG_LEN bytes. */
static inline int dserv_msg_build(uint8_t *frame,
                                  const char *name,
                                  uint64_t timestamp,
                                  uint32_t datatype,
                                  const void *data,
                                  uint32_t datalen)
{
    uint16_t varlen = (uint16_t) strlen(name);
    if ((uint32_t) varlen + datalen > DSERV_MSG_MAX_PAYLOAD)
        return -1;

    memset(frame, 0, DSERV_MSG_LEN);
    uint8_t *p = frame;
    *p++ = DSERV_MSG_CHAR;
    memcpy(p, &varlen, sizeof varlen);        p += sizeof varlen;
    memcpy(p, name, varlen);                  p += varlen;
    memcpy(p, &timestamp, sizeof timestamp);  p += sizeof timestamp;
    memcpy(p, &datatype, sizeof datatype);    p += sizeof datatype;
    memcpy(p, &datalen, sizeof datalen);      p += sizeof datalen;
    if (datalen) memcpy(p, data, datalen);

    return DSERV_MSG_LEN;   /* always transmit the full fixed-length frame */
}

/* Typed convenience wrappers. timestamp 0 => dserv uses arrival time. */
static inline int dserv_msg_int(uint8_t *f, const char *n, uint64_t ts, int32_t v)
{ return dserv_msg_build(f, n, ts, DSERV_INT, &v, sizeof v); }

static inline int dserv_msg_int64(uint8_t *f, const char *n, uint64_t ts, int64_t v)
{ return dserv_msg_build(f, n, ts, DSERV_INT64, &v, sizeof v); }

static inline int dserv_msg_short(uint8_t *f, const char *n, uint64_t ts, int16_t v)
{ return dserv_msg_build(f, n, ts, DSERV_SHORT, &v, sizeof v); }

static inline int dserv_msg_float(uint8_t *f, const char *n, uint64_t ts, float v)
{ return dserv_msg_build(f, n, ts, DSERV_FLOAT, &v, sizeof v); }

static inline int dserv_msg_double(uint8_t *f, const char *n, uint64_t ts, double v)
{ return dserv_msg_build(f, n, ts, DSERV_DOUBLE, &v, sizeof v); }

/* String datapoint: datalen = the string LENGTH, no trailing NUL. Matches
 * dserv's own string convention (Datapoint.c allocates datalen+1 and appends
 * its own terminator), so the value carries no spurious NUL byte. Sending the
 * NUL (the old strlen+1) put it INTO the data -> string consumers that don't
 * expect it broke: JS Number("14\0")==NaN mangled pins/in, Tcl `switch` on a
 * label "up\0" never matched "up" (ess joystick map). C consumers still get a
 * terminated string because dserv re-appends the NUL on receive. */
static inline int dserv_msg_string(uint8_t *f, const char *n, uint64_t ts,
                                   const char *s)
{ return dserv_msg_build(f, n, ts, DSERV_STRING, s, (uint32_t) strlen(s)); }

static inline int dserv_msg_bytes(uint8_t *f, const char *n, uint64_t ts,
                                  const void *data, uint32_t len)
{ return dserv_msg_build(f, n, ts, DSERV_BYTE, data, len); }

/* Overwrite the timestamp of an already-built frame IN PLACE. The ts field sits
 * right after the variable-length name (offset 3 = 1('>')+2(varlen), then the
 * name), so we read varlen to find it. Used by the BLE receiver to rewrite a
 * relayed handheld frame's raw source time into dserv time at the radio
 * boundary (echo-sync, BLE.md "Time") -- the frame is forwarded whole, never
 * rebuilt, so this is the one seam that touches it. No-op on a malformed frame. */
static inline void dserv_msg_set_timestamp(uint8_t *frame, uint64_t ts)
{
    if (frame[0] != DSERV_MSG_CHAR) return;
    uint16_t varlen; memcpy(&varlen, frame + 1, sizeof varlen);
    if ((uint32_t) varlen > DSERV_MSG_MAX_PAYLOAD) return;
    memcpy(frame + 3 + varlen, &ts, sizeof ts);
}

/* ======================================================================== *
 *  RX / decode side  (zero-alloc; fields point into the caller's frame)
 * ======================================================================== */

typedef struct {
    const char    *name;      /* points into the frame; NOT NUL-terminated */
    uint16_t       namelen;
    uint64_t       timestamp;
    uint32_t       type;      /* ds_datatype_t */
    uint32_t       datalen;
    const uint8_t *data;      /* points into the frame                     */
} dserv_msg_t;

/* Parse one 128-byte frame. Returns 0 on success, -1 if malformed. */
static inline int dserv_msg_parse(const uint8_t *frame, dserv_msg_t *m)
{
    if (frame[0] != DSERV_MSG_CHAR) return -1;
    const uint8_t *p = frame + 1;
    uint16_t varlen; memcpy(&varlen, p, sizeof varlen); p += sizeof varlen;
    if ((uint32_t) varlen > DSERV_MSG_MAX_PAYLOAD) return -1;
    m->name = (const char *) p; m->namelen = varlen; p += varlen;
    memcpy(&m->timestamp, p, sizeof m->timestamp); p += sizeof m->timestamp;
    memcpy(&m->type, p, sizeof m->type);           p += sizeof m->type;
    memcpy(&m->datalen, p, sizeof m->datalen);     p += sizeof m->datalen;
    if ((uint32_t) varlen + m->datalen > DSERV_MSG_MAX_PAYLOAD) return -1;
    m->data = p;
    return 0;
}

static inline int dserv_msg_name_eq(const dserv_msg_t *m, const char *s)
{ size_t n = strlen(s); return n == m->namelen && memcmp(m->name, s, n) == 0; }

static inline int dserv_msg_name_prefix(const dserv_msg_t *m, const char *pfx)
{ size_t n = strlen(pfx); return m->namelen >= n && memcmp(m->name, pfx, n) == 0; }

/* Typed value accessors (alignment-safe copies). */
static inline int32_t dserv_msg_as_int(const dserv_msg_t *m)
{ int32_t v = 0; memcpy(&v, m->data, m->datalen < sizeof v ? m->datalen : sizeof v); return v; }
static inline int64_t dserv_msg_as_int64(const dserv_msg_t *m)
{ int64_t v = 0; memcpy(&v, m->data, m->datalen < sizeof v ? m->datalen : sizeof v); return v; }
static inline float dserv_msg_as_float(const dserv_msg_t *m)
{ float v = 0; memcpy(&v, m->data, m->datalen < sizeof v ? m->datalen : sizeof v); return v; }
static inline double dserv_msg_as_double(const dserv_msg_t *m)
{ double v = 0; memcpy(&v, m->data, m->datalen < sizeof v ? m->datalen : sizeof v); return v; }
/* Copy the value out as a bounded, NUL-terminated C string. Safe even though
 * dserv does NOT zero-pad frames and string data carries no trailing NUL --
 * we copy exactly datalen bytes and terminate. Use this, not a raw pointer. */
static inline void dserv_msg_copy_cstr(const dserv_msg_t *m, char *buf, uint32_t bufsz)
{
    if (bufsz == 0) return;
    uint32_t n = m->datalen < bufsz - 1 ? m->datalen : bufsz - 1;
    memcpy(buf, m->data, n);
    buf[n] = '\0';
}

/* Value as a long, accepting EITHER a numeric datatype (INT/SHORT/BYTE/INT64/
 * FLOAT/DOUBLE) OR a STRING -- because dserv sends bare `dservSet name val`
 * values as STRING, while a typed C/Python setter may send a real int. */
static inline long dserv_msg_as_long(const dserv_msg_t *m)
{
    if (m->type == DSERV_STRING) {
        char b[32]; dserv_msg_copy_cstr(m, b, sizeof b);
        return strtol(b, NULL, 0);
    }
    switch (m->type) {
    case DSERV_FLOAT:  return (long) dserv_msg_as_float(m);
    case DSERV_DOUBLE: return (long) dserv_msg_as_double(m);
    case DSERV_INT64:  return (long) dserv_msg_as_int64(m);
    default:           return (long) dserv_msg_as_int(m);  /* BYTE/SHORT/INT */
    }
}

/* ---- stream framer: TCP is a byte stream; reassemble fixed 128B frames ---- *
 * Zero-alloc. Frames on the leading '>' and emits every complete 128 bytes.
 * Resyncs automatically (skips bytes until a '>' when between frames).       */

typedef struct { uint8_t buf[DSERV_MSG_LEN]; uint16_t have; } dserv_framer_t;
typedef void (*dserv_frame_cb)(const uint8_t *frame, void *ud);

static inline void dserv_framer_reset(dserv_framer_t *f) { f->have = 0; }

static inline void dserv_framer_feed(dserv_framer_t *f, const uint8_t *in,
                                     uint32_t n, dserv_frame_cb cb, void *ud)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = in[i];
        if (f->have == 0 && b != DSERV_MSG_CHAR && b != DSERV_OTA_CHAR) continue;   /* resync ('>' datapoint or 'D' OTA) */
        f->buf[f->have++] = b;
        if (f->have == DSERV_MSG_LEN) { cb(f->buf, ud); f->have = 0; }
    }
}

#endif /* DSERV_MSG_H */
