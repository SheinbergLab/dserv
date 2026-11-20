/*
 * box_meta.h -- a small HOST-OWNED key/value area in the box's persisted config.
 *
 * The box stores these and announces them; it never interprets them. That is
 * the whole design, and it keeps the rule the rest of the config lives by: the
 * box knows pins, groups and labels -- never device types. What a key MEANS is
 * the host's business. Two uses motivated it (2026-11-20):
 *
 *   meta/model      which MODEL this unit was built as (extio_models on the
 *                   host: "astick"), so a box says what it is instead of the
 *                   host inferring it from its pin map
 *   meta/cal/<sub>  a CALIBRATION measured on this physical unit (a stick's
 *                   centre, orientation, throw), so it travels with the
 *                   hardware instead of living in one rig's database under a
 *                   name that the next stick will also answer to
 *
 * Storage: packed "key=value\n" lines in one NUL-terminated char array. Sized
 * by the WIRE, not by flash: a config write and a state announce each ride a
 * fixed 128-byte frame with 109 bytes for name + value. The longest name is
 * "extio/<15-char box>/config/meta/<15-char key>" = 49, which leaves 60; the
 * cap keeps a few bytes of margin. A longer value could be STORED but never
 * set or announced, so it is refused instead.
 *
 *   key    1..15 chars of [A-Za-z0-9_./-], not starting or ending with '/'
 *   value  0..56 printable ASCII chars; "" deletes the key
 *
 * Pure C, zero-alloc, no hardware -- shared by firmware, simulator and tests.
 */
#ifndef BOX_META_H
#define BOX_META_H

#include <string.h>
#include <stdio.h>

#define BOX_META_MAX      256   /* packed entries incl. the terminating NUL */
#define BOX_META_KEY_MAX  16    /* incl. NUL: 15 chars */
#define BOX_META_VAL_MAX  57    /* incl. NUL: 56 chars -- see the frame math above */

static inline int box_meta__len(const char *s, int cap)
{
    int n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

static inline int box_meta_key_valid(const char *k)
{
    int n = box_meta__len(k, BOX_META_KEY_MAX);
    if (n == 0 || n >= BOX_META_KEY_MAX) return 0;
    for (int i = 0; i < n; i++) {
        char ch = k[i];
        int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                 (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' ||
                 ch == '/' || ch == '-';
        if (!ok) return 0;
    }
    if (k[0] == '/' || k[n - 1] == '/') return 0;
    return 1;
}

static inline int box_meta_val_valid(const char *v)
{
    int n = box_meta__len(v, BOX_META_VAL_MAX);
    if (n >= BOX_META_VAL_MAX) return 0;
    for (int i = 0; i < n; i++)
        if (v[i] < 0x20 || v[i] > 0x7e) return 0;
    return 1;
}

/* Walk the entries. *pos starts at 0. Returns 1 with key/val filled, 0 at the
 * end. A malformed line (no '=', or too long for the caller's buffers) is
 * skipped rather than ending the walk, so one bad entry cannot hide the rest. */
static inline int box_meta_next(const char *meta, int metasz, int *pos,
                                char *key, int ksz, char *val, int vsz)
{
    while (*pos < metasz && meta[*pos]) {
        const char *s = meta + *pos;
        int len = 0, nl = 0;
        while (*pos + len < metasz && s[len] && s[len] != '\n') len++;
        if (*pos + len < metasz && s[len] == '\n') nl = 1;
        *pos += len + nl;

        int kl = 0;
        while (kl < len && s[kl] != '=') kl++;
        if (kl == len || kl == 0) continue;               /* no '=' / empty key */
        int vl = len - kl - 1;
        if (kl >= ksz || vl >= vsz) continue;
        memcpy(key, s, (size_t) kl);      key[kl] = '\0';
        memcpy(val, s + kl + 1, (size_t) vl); val[vl] = '\0';
        return 1;
    }
    return 0;
}

/* 1 and the value in out if key is present, else 0 (out = ""). */
static inline int box_meta_get(const char *meta, int metasz, const char *key,
                               char *out, int outsz)
{
    char k[BOX_META_KEY_MAX], v[BOX_META_VAL_MAX];
    int pos = 0;
    if (outsz > 0) out[0] = '\0';
    while (box_meta_next(meta, metasz, &pos, k, sizeof k, v, sizeof v))
        if (!strcmp(k, key)) { snprintf(out, (size_t) outsz, "%s", v); return 1; }
    return 0;
}

/* Set key to val; val "" deletes it. Returns 0 ok, -1 invalid key/value, -2
 * no room. Rewrites the whole area (it is 256 bytes), so a changed key moves
 * to the end and the area is always well-formed and NUL-padded afterwards --
 * including when it started as a zero-defaulted field from an older blob. */
static inline int box_meta_set(char *meta, int metasz, const char *key,
                               const char *val)
{
    if (metasz > BOX_META_MAX || metasz < 2) return -1;
    if (!box_meta_key_valid(key) || !box_meta_val_valid(val)) return -1;

    char tmp[BOX_META_MAX];
    char k[BOX_META_KEY_MAX], v[BOX_META_VAL_MAX];
    int tn = 0, pos = 0;

    meta[metasz - 1] = '\0';
    while (box_meta_next(meta, metasz, &pos, k, sizeof k, v, sizeof v)) {
        if (!strcmp(k, key)) continue;
        int need = (int) strlen(k) + 1 + (int) strlen(v) + 1;
        if (tn + need >= metasz) return -2;
        tn += snprintf(tmp + tn, sizeof tmp - (size_t) tn, "%s=%s\n", k, v);
    }
    if (val[0]) {
        int need = (int) strlen(key) + 1 + (int) strlen(val) + 1;
        if (tn + need >= metasz) return -2;
        tn += snprintf(tmp + tn, sizeof tmp - (size_t) tn, "%s=%s\n", key, val);
    }
    memset(meta, 0, (size_t) metasz);
    memcpy(meta, tmp, (size_t) tn);
    return 0;
}

/* Comma-joined key list (for state/meta/keys). Returns its length. */
static inline int box_meta_keys_csv(const char *meta, int metasz, char *out, int outsz)
{
    char k[BOX_META_KEY_MAX], v[BOX_META_VAL_MAX];
    int pos = 0, n = 0;
    if (outsz > 0) out[0] = '\0';
    while (box_meta_next(meta, metasz, &pos, k, sizeof k, v, sizeof v) && n < outsz - 1)
        n += snprintf(out + n, (size_t) (outsz - n), "%s%s", n ? "," : "", k);
    return n;
}

#endif /* BOX_META_H */
