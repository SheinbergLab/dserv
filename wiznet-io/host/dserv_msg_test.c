/*
 * dserv_msg_test.c -- verify dserv_msg.h builds frames that dserv's '>' handler
 * (src/Dataserver.cpp:2531) parses correctly. Parses with the SAME pointer walk
 * dserv uses, and checks fields round-trip.
 *
 *   cc -O2 -Wall -I../common -o dserv_msg_test dserv_msg_test.c && ./dserv_msg_test
 */
#include "dserv_msg.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;

/* Mirror of the dserv receiver's parse of the 127 bytes after '>'. */
static void parse_and_check(const uint8_t *frame, const char *exp_name,
                            uint64_t exp_ts, uint32_t exp_type,
                            const void *exp_data, uint32_t exp_len)
{
    if (frame[0] != DSERV_MSG_CHAR) { printf("  FAIL lead char\n"); fails++; return; }

    const uint8_t *bufptr = frame + 1;             /* dserv reads buffer[127] here */
    uint16_t varlen;   memcpy(&varlen, bufptr, 2);        bufptr += 2;
    char name[128];    memcpy(name, bufptr, varlen); name[varlen] = 0; bufptr += varlen;
    uint64_t ts;       memcpy(&ts, bufptr, 8);            bufptr += 8;
    uint32_t type;     memcpy(&type, bufptr, 4);          bufptr += 4;
    uint32_t len;      memcpy(&len, bufptr, 4);           bufptr += 4;
    const uint8_t *data = bufptr;

    int ok = 1;
    if (strcmp(name, exp_name) != 0)                 { ok = 0; printf("  name mismatch: '%s' != '%s'\n", name, exp_name); }
    if (ts != exp_ts)                                { ok = 0; printf("  ts mismatch: %llu != %llu\n", (unsigned long long)ts, (unsigned long long)exp_ts); }
    if (type != exp_type)                            { ok = 0; printf("  type mismatch: %u != %u\n", type, exp_type); }
    if (len != exp_len)                              { ok = 0; printf("  len mismatch: %u != %u\n", len, exp_len); }
    if (len == exp_len && memcmp(data, exp_data, len)) { ok = 0; printf("  data bytes mismatch\n"); }

    printf("  %-22s varlen=%2u ts=%llu type=%u len=%u -> %s\n",
           exp_name, varlen, (unsigned long long)ts, type, len, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

int main(void)
{
    uint8_t f[DSERV_MSG_LEN];
    int r;

    printf("dserv_msg.h round-trip (parse == Dataserver.cpp '>' handler)\n");

    r = dserv_msg_int(f, "pico/watchdog", 0, 42);
    printf("build ret=%d (want %d)\n", r, DSERV_MSG_LEN);
    { int32_t v = 42; parse_and_check(f, "pico/watchdog", 0, DSERV_INT, &v, 4); }

    r = dserv_msg_int64(f, "pico/uptime_us", 123456789ULL, 9876543210LL);
    { int64_t v = 9876543210LL; parse_and_check(f, "pico/uptime_us", 123456789ULL, DSERV_INT64, &v, 8); }

    r = dserv_msg_float(f, "pico/temp_c", 0, 36.6f);
    { float v = 36.6f; parse_and_check(f, "pico/temp_c", 0, DSERV_FLOAT, &v, 4); }

    r = dserv_msg_double(f, "pico/volts", 0, 3.301);
    { double v = 3.301; parse_and_check(f, "pico/volts", 0, DSERV_DOUBLE, &v, 8); }

    r = dserv_msg_string(f, "pico/status", 0, "alive");
    parse_and_check(f, "pico/status", 0, DSERV_STRING, "alive", 6 /* incl NUL */);

    /* boundary: varname + data must be <= 109 */
    char big[110]; memset(big, 'x', sizeof big);
    r = dserv_msg_bytes(f, "n", 0, big, 109 - 1 /* +1 for "n" */);
    printf("boundary fit ret=%d (want %d)\n", r, DSERV_MSG_LEN);
    if (r != DSERV_MSG_LEN) fails++;
    r = dserv_msg_bytes(f, "n", 0, big, 110);       /* one too big */
    printf("boundary overflow ret=%d (want -1)\n", r);
    if (r != -1) fails++;

    printf(fails ? "\nFAILED (%d)\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
