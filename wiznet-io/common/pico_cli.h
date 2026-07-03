/*
 * pico_cli.h -- portable line-oriented CLI for the box, used over USB-CDC on the
 * device (bootstrap before the network is up + recovery) and over stdin in the
 * simulator. Pure C. One command per line; writes a human response into `out`
 * and returns an action for the caller to perform platform-specific storage.
 *
 * Commands:
 *   help
 *   show
 *   net ip A.B.C.D
 *   dserv ip A.B.C.D
 *   dserv port N
 *   pin N mode out|in|in_pullup|off
 *   pin N pulse US
 *   save          -> CLI_SAVE   (caller persists to flash/file)
 *   factory       -> CLI_FACTORY(caller erases storage + resets cfg)
 *   reboot        -> CLI_REBOOT (caller resets the MCU)
 */
#ifndef PICO_CLI_H
#define PICO_CLI_H

#include "dserv_config.h"
#include <stdio.h>
#include <string.h>

typedef enum { CLI_OK, CLI_ERR, CLI_PIN, CLI_GPIO, CLI_SAVE, CLI_FACTORY, CLI_REBOOT } cli_action_t;

/* mode word<->value shared with dserv_config.h: dserv_mode_val / dserv_mode_str */

static inline void pico_cli_show(const pico_config_t *c, char *out, int outsz)
{
    int k = snprintf(out, outsz,
        "name=%s net.ip=%u.%u.%u.%u dserv=%u.%u.%u.%u:%u applied=%u\r\n",
        dserv_cfg_name(c),
        c->net_ip[0], c->net_ip[1], c->net_ip[2], c->net_ip[3],
        c->dserv_ip[0], c->dserv_ip[1], c->dserv_ip[2], c->dserv_ip[3],
        c->dserv_port, c->applied_count);
    for (int i = 0; i < PICO_NPINS && k < outsz - 48; i++)
        if (c->pin_mode[i])
            k += snprintf(out + k, outsz - k, "  pin%d=%s pulse=%uus debounce=%ums\r\n",
                          i, dserv_mode_str(c->pin_mode[i]), c->do_pulse_us[i], c->debounce_ms[i]);
}

/* Execute one line. Returns an action; fills `out` with a response line.
 * `cmd` (may be NULL) receives a GPIO command for the `do` verbs (CLI_GPIO). */
static inline cli_action_t pico_cli_exec(pico_config_t *c, const char *line,
                                         char *out, int outsz, gpio_cmd_t *cmd)
{
    int n, v; char w[24];
    if (cmd) cmd->op = GPIO_OP_NONE;

    /* skip leading spaces; ignore blank lines */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') { out[0] = '\0'; return CLI_OK; }

    if (sscanf(line, "do %d pulse %d", &n, &v) == 2) {
        if (n < 0 || n >= PICO_NPINS || v < 0) { snprintf(out, outsz, "ERR bad do/pulse\r\n"); return CLI_ERR; }
        if (cmd) { cmd->op = GPIO_OP_PULSE; cmd->pin = (uint8_t) n; cmd->value = (uint32_t) v; }
        snprintf(out, outsz, "OK do%d pulse=%dus\r\n", n, v); return CLI_GPIO;
    }
    if (sscanf(line, "do %d %d", &n, &v) == 2) {
        if (n < 0 || n >= PICO_NPINS) { snprintf(out, outsz, "ERR bad do pin\r\n"); return CLI_ERR; }
        if (cmd) { cmd->op = GPIO_OP_SET; cmd->pin = (uint8_t) n; cmd->value = v ? 1 : 0; }
        snprintf(out, outsz, "OK do%d=%d\r\n", n, v ? 1 : 0); return CLI_GPIO;
    }

    if (sscanf(line, "name %15s", w) == 1) {
        strncpy(c->name, w, sizeof c->name - 1); c->name[sizeof c->name - 1] = '\0';
        c->applied_count++; snprintf(out, outsz, "OK name=%s\r\n", c->name); return CLI_OK;
    }
    if (sscanf(line, "pin %d mode %23s", &n, w) == 2) {
        int m = dserv_mode_val(w);
        if (n < 0 || n >= PICO_NPINS || m < 0) { snprintf(out, outsz, "ERR bad pin/mode\r\n"); return CLI_ERR; }
        c->pin_mode[n] = (uint8_t) m; c->applied_count++;
        snprintf(out, outsz, "OK pin%d=%s\r\n", n, dserv_mode_str((uint8_t)m)); return CLI_PIN;
    }
    if (sscanf(line, "pin %d pulse %d", &n, &v) == 2) {
        if (n < 0 || n >= PICO_NPINS || v < 0) { snprintf(out, outsz, "ERR bad pin/pulse\r\n"); return CLI_ERR; }
        c->do_pulse_us[n] = (uint32_t) v; c->applied_count++;
        snprintf(out, outsz, "OK pin%d pulse=%dus\r\n", n, v); return CLI_OK;
    }
    if (sscanf(line, "pin %d debounce %d", &n, &v) == 2) {
        if (n < 0 || n >= PICO_NPINS || v < 0 || v > 255) { snprintf(out, outsz, "ERR bad pin/debounce (0-255ms)\r\n"); return CLI_ERR; }
        c->debounce_ms[n] = (uint8_t) v; c->applied_count++;
        snprintf(out, outsz, "OK pin%d debounce=%dms\r\n", n, v); return CLI_OK;
    }
    if (sscanf(line, "dserv ip %23s", w) == 1) {
        if (dserv_cfg__parse_ip(w, c->dserv_ip)) { snprintf(out, outsz, "ERR bad ip\r\n"); return CLI_ERR; }
        c->applied_count++; snprintf(out, outsz, "OK dserv ip=%s\r\n", w); return CLI_OK;
    }
    if (sscanf(line, "dserv port %d", &v) == 1) {
        if (v < 1 || v > 65535) { snprintf(out, outsz, "ERR bad port\r\n"); return CLI_ERR; }
        c->dserv_port = (uint16_t) v; c->applied_count++;
        snprintf(out, outsz, "OK dserv port=%d\r\n", v); return CLI_OK;
    }
    if (sscanf(line, "net ip %23s", w) == 1) {
        if (dserv_cfg__parse_ip(w, c->net_ip)) { snprintf(out, outsz, "ERR bad ip\r\n"); return CLI_ERR; }
        c->applied_count++; snprintf(out, outsz, "OK net ip=%s (save+reboot to apply)\r\n", w); return CLI_OK;
    }
    if (!strcmp(line, "show"))    { pico_cli_show(c, out, outsz); return CLI_OK; }
    if (!strcmp(line, "save"))    { snprintf(out, outsz, "saving...\r\n"); return CLI_SAVE; }
    if (!strcmp(line, "factory")) { snprintf(out, outsz, "factory reset...\r\n"); return CLI_FACTORY; }
    if (!strcmp(line, "reboot"))  { snprintf(out, outsz, "rebooting...\r\n"); return CLI_REBOOT; }
    if (!strcmp(line, "help")) {
        snprintf(out, outsz,
            "cmds: show | name NAME | net ip A.B.C.D | dserv ip A.B.C.D | dserv port N |\r\n"
            "      pin N mode out|in|in_pullup|off | pin N pulse US | pin N debounce MS |\r\n"
            "      do N 0|1 | do N pulse US | save | factory | reboot\r\n");
        return CLI_OK;
    }
    snprintf(out, outsz, "ERR unknown (try 'help')\r\n");
    return CLI_ERR;
}

#endif /* PICO_CLI_H */
