/*
 * meta_test.c -- exercise box_meta.h on the host: the host-owned key/value
 * area (meta/model, meta/cal/<sub>). Set, replace, delete, the caps the wire
 * imposes, a full area, a zero-defaulted area from an older blob, and that a
 * malformed line cannot hide the entries after it.
 *   cc -O2 -Wall -I../src/core -o meta_test meta_test.c && ./meta_test
 */
#include "box_meta.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL %s\n", msg); fails++; } \
                           else       printf("  ok   %s\n", msg); } while (0)

int main(void)
{
    char m[BOX_META_MAX], v[BOX_META_VAL_MAX], csv[BOX_META_MAX];

    memset(m, 0, sizeof m);                       /* an older blob: zero-defaulted */
    CHECK(!box_meta_get(m, sizeof m, "model", v, sizeof v), "empty area has no keys");
    box_meta_keys_csv(m, sizeof m, csv, sizeof csv);
    CHECK(csv[0] == '\0', "empty area -> empty key list");

    CHECK(box_meta_set(m, sizeof m, "model", "astick") == 0, "set model");
    CHECK(box_meta_set(m, sizeof m, "cal/slider", "v1 cx2048.1 cy2038.1 sx1 sy0 ix1 iy0 t1249") == 0,
          "set a calibration");
    CHECK(box_meta_get(m, sizeof m, "model", v, sizeof v) && !strcmp(v, "astick"), "get model");
    box_meta_keys_csv(m, sizeof m, csv, sizeof csv);
    CHECK(!strcmp(csv, "model,cal/slider"), "key list in order");

    CHECK(box_meta_set(m, sizeof m, "model", "astick2") == 0, "replace");
    CHECK(box_meta_get(m, sizeof m, "model", v, sizeof v) && !strcmp(v, "astick2"), "...new value");
    box_meta_keys_csv(m, sizeof m, csv, sizeof csv);
    CHECK(!strcmp(csv, "cal/slider,model"), "...one entry, moved to the end");

    CHECK(box_meta_set(m, sizeof m, "model", "") == 0, "delete");
    CHECK(!box_meta_get(m, sizeof m, "model", v, sizeof v), "...gone");
    CHECK(box_meta_set(m, sizeof m, "nothere", "") == 0, "deleting an absent key is ok");

    CHECK(box_meta_set(m, sizeof m, "a=b", "x") == -1, "'=' in a key refused");
    CHECK(box_meta_set(m, sizeof m, "/lead", "x") == -1, "leading '/' refused");
    CHECK(box_meta_set(m, sizeof m, "sixteen_chars_xx", "x") == -1, "16-char key refused");
    CHECK(box_meta_set(m, sizeof m, "fifteen_chars_x", "x") == 0, "15-char key ok");
    CHECK(box_meta_set(m, sizeof m, "k", "line\nbreak") == -1, "newline in a value refused");
    char v56[57], v57[58];
    memset(v56, 'x', 56); v56[56] = '\0';
    memset(v57, 'x', 57); v57[57] = '\0';
    CHECK(box_meta_set(m, sizeof m, "k", v56) == 0, "56-char value ok");
    CHECK(box_meta_set(m, sizeof m, "k", v57) == -1, "57-char value refused (wire cap)");
    CHECK(box_meta_set(m, sizeof m, "eq", "a=b c=d") == 0 &&
          box_meta_get(m, sizeof m, "eq", v, sizeof v) && !strcmp(v, "a=b c=d"),
          "'=' inside a VALUE survives");

    /* fill it */
    memset(m, 0, sizeof m);
    int n = 0, rc = 0;
    char key[16];
    while (n < 20) {
        snprintf(key, sizeof key, "k%02d", n);
        if ((rc = box_meta_set(m, sizeof m, key, v56)) != 0) break;
        n++;
    }
    CHECK(rc == -2, "a full area says so (-2)");
    CHECK(n == 4, "...after four 60-byte entries");
    CHECK(m[sizeof m - 1] == '\0', "...still NUL-terminated");
    CHECK(box_meta_get(m, sizeof m, "k03", v, sizeof v), "...and nothing already in it was lost");

    /* a malformed line in the middle does not end the walk */
    memset(m, 0, sizeof m);
    snprintf(m, sizeof m, "a=1\ngarbage\nb=2\n");
    CHECK(box_meta_get(m, sizeof m, "b", v, sizeof v) && !strcmp(v, "2"), "entry after a bad line found");
    CHECK(box_meta_set(m, sizeof m, "c", "3") == 0, "set over a bad line...");
    box_meta_keys_csv(m, sizeof m, csv, sizeof csv);
    CHECK(!strcmp(csv, "a,b,c"), "...drops the bad line, keeps the rest");

    /* an unterminated area (corrupt flash) is bounded, not overrun */
    memset(m, 'z', sizeof m);
    CHECK(!box_meta_get(m, sizeof m, "z", v, sizeof v), "unterminated area: no runaway read");
    CHECK(box_meta_set(m, sizeof m, "ok", "1") == 0 &&
          box_meta_get(m, sizeof m, "ok", v, sizeof v), "...and a set repairs it");

    printf(fails ? "META TEST FAIL (%d)\n" : "META TEST PASS\n", fails);
    return fails ? 1 : 0;
}
