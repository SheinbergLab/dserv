/*
 * persist_cli_test.c -- verify pico_persist.h (serialize/validate/crc) and
 * pico_cli.h (command parsing) on the host.
 *   cc -O2 -Wall -I../common -o persist_cli_test persist_cli_test.c && ./persist_cli_test
 */
#include "pico_persist.h"
#include "pico_cli.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>     /* offsetof: hand-built v12 blob */

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

    /* ---- v13: desc / labels / chord groups ---- */
    act = pico_cli_exec(&c, "desc training rig box", out, sizeof out, &cli_cmd);
    CHECK(act == CLI_GROUP && strcmp(c.desc, "training rig box") == 0, "desc (spaces survive)");

    act = pico_cli_exec(&c, "label 5 up", out, sizeof out, &cli_cmd);
    CHECK(act == CLI_GROUP && strcmp(c.pin_label[5], "up") == 0, "pin label");
    CHECK(pico_cli_exec(&c, "label 5 has space", out, sizeof out, &cli_cmd) == CLI_GROUP
          && strcmp(c.pin_label[5], "has") == 0, "label takes one token");
    act = pico_cli_exec(&c, "label 5 off", out, sizeof out, &cli_cmd);
    CHECK(act == CLI_GROUP && c.pin_label[5][0] == '\0', "label off clears");
    CHECK(pico_cli_exec(&c, "label 99 up", out, sizeof out, &cli_cmd) == CLI_ERR, "reject bad label pin");

    act = pico_cli_exec(&c, "group 0 pins 4,5,6,7", out, sizeof out, &cli_cmd);
    CHECK(act == CLI_GROUP && c.group_pins[0] == 0xF0u, "group pins csv -> mask");
    act = pico_cli_exec(&c, "group 0 label joystick", out, sizeof out, &cli_cmd);
    CHECK(act == CLI_GROUP && strcmp(c.group_label[0], "joystick") == 0, "group label");
    act = pico_cli_exec(&c, "group 0 settle 25", out, sizeof out, &cli_cmd);
    CHECK(act == CLI_GROUP && c.group_settle_ms[0] == 25, "group settle");
    act = pico_cli_exec(&c, "group 0 quiet 1", out, sizeof out, &cli_cmd);
    CHECK(act == CLI_GROUP && c.group_quiet[0] == 1, "group quiet");
    CHECK(pico_cli_exec(&c, "group 0 pins 4,99", out, sizeof out, &cli_cmd) == CLI_ERR,
          "reject bad group pin");
    CHECK(pico_cli_exec(&c, "group 9 pins 4,5", out, sizeof out, &cli_cmd) == CLI_ERR,
          "reject bad group index");
    act = pico_cli_exec(&c, "group 1 pins 2,3", out, sizeof out, &cli_cmd);
    act = pico_cli_exec(&c, "group 1 off", out, sizeof out, &cli_cmd);
    CHECK(act == CLI_GROUP && c.group_pins[1] == 0, "group off clears");

    /* persistence survives a CLI-configured struct */
    n = pico_persist_serialize(&c, blob, sizeof blob);
    pico_config_t d; memset(&d, 0, sizeof d);
    CHECK(pico_persist_deserialize(blob, n, &d) == 0 && d.dserv_port == 4620
          && strcmp(d.name, "io1") == 0, "cli config (incl name) persists");
    CHECK(strcmp(d.desc, "training rig box") == 0 && d.group_pins[0] == 0xF0u
          && strcmp(d.group_label[0], "joystick") == 0 && d.group_settle_ms[0] == 25,
          "v13 fields persist");

    /* ---- forward-compat: a v12 (pre-label) blob loads with v13 fields zeroed ---- */
    uint32_t old_len = (uint32_t) offsetof(pico_config_t, desc);
    uint8_t v12[PICO_PERSIST_BLOB_MAX]; uint8_t *p = v12;
    uint32_t magic = PICO_PERSIST_MAGIC; memcpy(p, &magic, 4); p += 4;
    uint16_t ver = 12;                   memcpy(p, &ver, 2);   p += 2;
    uint16_t blen = (uint16_t) old_len;  memcpy(p, &blen, 2);  p += 2;
    memcpy(p, &c, old_len);                                    p += old_len;
    uint32_t crc = pico_crc32(v12, 8 + old_len);               memcpy(p, &crc, 4);
    pico_config_t e; memset(&e, 0xAA, sizeof e);
    CHECK(pico_persist_deserialize(v12, 12 + old_len, &e) == 0, "v12 blob accepted");
    CHECK(strcmp(e.name, "io1") == 0 && e.dserv_port == 4620, "v12 fields preserved");
    CHECK(e.desc[0] == 0 && e.pin_label[5][0] == 0 && e.group_pins[0] == 0
          && e.group_settle_ms[0] == 0, "v13 fields default to none");

    printf(fails ? "\nFAILED (%d)\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
