/*
 * persist_cli_test.c -- verify pico_persist.h (serialize/validate/crc) and
 * pico_cli.h (command parsing) on the host.
 *   cc -O2 -Wall -I../common -o persist_cli_test persist_cli_test.c && ./persist_cli_test
 */
#include "pico_persist.h"
#include "pico_cli.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL %s\n", msg); fails++; } \
                           else       printf("  ok   %s\n", msg); } while (0)

int main(void)
{
    /* ---- persistence round-trip ---- */
    pico_config_t a; memset(&a, 0, sizeof a);
    a.pin_mode[5] = 1; a.do_pulse_us[5] = 500; a.pin_mode[6] = 3;
    a.dserv_ip[0]=192; a.dserv_ip[1]=168; a.dserv_ip[2]=11; a.dserv_ip[3]=1;
    a.dserv_port = 4620; a.net_ip[3] = 42;

    uint8_t blob[PICO_PERSIST_BLOB_MAX];
    uint32_t n = pico_persist_serialize(&a, blob, sizeof blob);
    printf("persist: serialized %u bytes\n", n);
    CHECK(n > 0, "serialize");

    pico_config_t b; memset(&b, 0xAA, sizeof b);
    CHECK(pico_persist_deserialize(blob, n, &b) == 0, "deserialize valid");
    CHECK(memcmp(&a, &b, sizeof a) == 0, "round-trip identical");

    uint8_t bad[PICO_PERSIST_BLOB_MAX]; memcpy(bad, blob, n);
    bad[10] ^= 0xFF;                                  /* flip a payload byte */
    CHECK(pico_persist_deserialize(bad, n, &b) == -1, "crc rejects corruption");
    bad[0] ^= 0xFF;                                   /* also break magic    */
    memcpy(bad, blob, n); bad[0] ^= 0xFF;
    CHECK(pico_persist_deserialize(bad, n, &b) == -1, "magic rejects foreign blob");

    /* ---- CLI ---- */
    printf("cli:\n");
    pico_config_t c; memset(&c, 0, sizeof c);
    char out[256]; gpio_cmd_t cli_cmd;
    cli_action_t act;

    act = pico_cli_exec(&c, "name io1", out, sizeof out, &cli_cmd);
    printf("    > name io1          : %s", out);
    CHECK(act == CLI_OK && strcmp(c.name, "io1") == 0, "set device name");

    act = pico_cli_exec(&c, "pin 5 mode out", out, sizeof out, &cli_cmd);
    printf("    > pin 5 mode out    : %s", out);
    CHECK(act == CLI_PIN && c.pin_mode[5] == 1, "pin mode out (-> CLI_PIN)");

    act = pico_cli_exec(&c, "pin 5 pulse 500", out, sizeof out, &cli_cmd);
    CHECK(act == CLI_OK && c.do_pulse_us[5] == 500, "pin pulse");

    act = pico_cli_exec(&c, "dserv ip 10.0.0.5", out, sizeof out, &cli_cmd);
    printf("    > dserv ip 10.0.0.5 : %s", out);
    CHECK(act == CLI_OK && c.dserv_ip[0] == 10 && c.dserv_ip[3] == 5, "dserv ip");

    act = pico_cli_exec(&c, "dserv port 4620", out, sizeof out, &cli_cmd);
    CHECK(act == CLI_OK && c.dserv_port == 4620, "dserv port");

    act = pico_cli_exec(&c, "net ip 192.168.1.50", out, sizeof out, &cli_cmd);
    CHECK(act == CLI_OK && c.net_ip[0] == 192 && c.net_ip[3] == 50, "net ip");

    act = pico_cli_exec(&c, "pin 99 mode out", out, sizeof out, &cli_cmd);
    CHECK(act == CLI_ERR, "reject out-of-range pin");

    act = pico_cli_exec(&c, "dserv ip 999.1.1.1", out, sizeof out, &cli_cmd);
    CHECK(act == CLI_ERR, "reject bad ip");

    CHECK(pico_cli_exec(&c, "save", out, sizeof out, &cli_cmd) == CLI_SAVE, "save action");
    CHECK(pico_cli_exec(&c, "factory", out, sizeof out, &cli_cmd) == CLI_FACTORY, "factory action");
    CHECK(pico_cli_exec(&c, "bogus", out, sizeof out, &cli_cmd) == CLI_ERR, "unknown -> err");

    /* persistence survives a CLI-configured struct */
    n = pico_persist_serialize(&c, blob, sizeof blob);
    pico_config_t d; memset(&d, 0, sizeof d);
    CHECK(pico_persist_deserialize(blob, n, &d) == 0 && d.dserv_port == 4620
          && strcmp(d.name, "io1") == 0, "cli config (incl name) persists");

    printf(fails ? "\nFAILED (%d)\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
