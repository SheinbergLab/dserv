/*
 * pico_cli.h -- portable line-oriented CLI for the box, used over USB-CDC on the
 * device (bootstrap before the network is up + recovery) and over stdin in the
 * simulator. Pure C. One command per line; writes a human response into `out`
 * and returns an action for the caller to perform platform-specific storage.
 *
 * Commands:
 *   help
 *   show
 *   net mode dhcp|static
 *   net ip A.B.C.D       (also sets mode=static)
 *   wifi ssid SSID       (pico2w; runtime creds, else compile-time fallback)
 *   wifi pass PASS
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
    char obs[8];
    if (obs_mirror_enabled(c)) snprintf(obs, sizeof obs, "%d", obs_mirror_pin(c));
    else                       snprintf(obs, sizeof obs, "off");
    int k = snprintf(out, outsz,
        "name=%s net.mode=%s transport=%s net.ip=%u.%u.%u.%u dserv=%u.%u.%u.%u:%u obs.pin=%s wifi.ssid=%s wifi.pass=%s wifi.pm=%u ain.en=%u ain.rate=%u ain.gain=%u applied=%u\r\n",
        dserv_cfg_name(c), dserv_netmode_str(c->net_mode), dserv_xport_str(c->transport_mode),
        c->net_ip[0], c->net_ip[1], c->net_ip[2], c->net_ip[3],
        c->dserv_ip[0], c->dserv_ip[1], c->dserv_ip[2], c->dserv_ip[3],
        c->dserv_port, obs,
        c->wifi_ssid[0] ? c->wifi_ssid : "(build)", c->wifi_pass[0] ? "set" : "(build)",
        c->wifi_pm, c->ain_en, c->ain_rate, c->ain_gain, c->applied_count);
    for (int i = 0; i < PICO_NPINS && k < outsz - 64; i++)
        if (c->pin_mode[i])
            k += snprintf(out + k, outsz - k, "  pin%d=%s pulse=%uus debounce=%ums%s\r\n",
                          i, dserv_mode_str(c->pin_mode[i]), c->do_pulse_us[i], c->debounce_ms[i],
                          di_active_low(c, i) ? " active_low" : "");
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
        if (!dserv_name_valid(w)) { snprintf(out, outsz, "ERR name: printable, no '/'\r\n"); return CLI_ERR; }
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
    if (sscanf(line, "pin %d active_low %d", &n, &v) == 2) {
        if (n < 0 || n >= PICO_NPINS) { snprintf(out, outsz, "ERR bad pin\r\n"); return CLI_ERR; }
        di_active_low_set(c, n, v ? 1 : 0); c->applied_count++;
        snprintf(out, outsz, "OK pin%d active_low=%d\r\n", n, v ? 1 : 0); return CLI_OK;
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
        c->net_mode = NET_MODE_STATIC;      /* setting a static IP implies static mode */
        c->applied_count++; snprintf(out, outsz, "OK net ip=%s mode=static (save+reboot to apply)\r\n", w); return CLI_OK;
    }
    if (sscanf(line, "obs pin %d", &n) == 1) {
        if (n < 0 || n >= PICO_NPINS) { snprintf(out, outsz, "ERR obs pin 0-%d (or 'obs off')\r\n", PICO_NPINS - 1); return CLI_ERR; }
        obs_mirror_set(c, n); c->applied_count++;
        snprintf(out, outsz, "OK obs pin=%d\r\n", n); return CLI_PIN;
    }
    if (!strcmp(line, "obs off")) {
        obs_mirror_off(c); c->applied_count++;
        snprintf(out, outsz, "OK obs off\r\n"); return CLI_PIN;
    }
    /* wifi creds: value is the rest of the line verbatim (one space separator),
     * so SSIDs/passwords with spaces or special chars survive intact. */
    if (!strncmp(line, "wifi ssid ", 10)) {
        strncpy(c->wifi_ssid, line + 10, sizeof c->wifi_ssid - 1); c->wifi_ssid[sizeof c->wifi_ssid - 1] = '\0';
        c->applied_count++; snprintf(out, outsz, "OK wifi ssid=%s (save+reboot to apply)\r\n", c->wifi_ssid); return CLI_OK;
    }
    if (!strncmp(line, "wifi pass ", 10)) {
        strncpy(c->wifi_pass, line + 10, sizeof c->wifi_pass - 1); c->wifi_pass[sizeof c->wifi_pass - 1] = '\0';
        c->applied_count++; snprintf(out, outsz, "OK wifi pass set (save+reboot to apply)\r\n"); return CLI_OK;
    }
    if (sscanf(line, "wifi pm %d", &v) == 1) {
        c->wifi_pm = v ? 1 : 0; c->applied_count++;
        snprintf(out, outsz, "OK wifi pm=%d (%s; save+reboot to apply)\r\n",
                 c->wifi_pm, c->wifi_pm ? "power-save/battery" : "off/low-latency"); return CLI_OK;
    }
    if (sscanf(line, "ain rate %d", &v) == 1) {   /* ADS1115: 0=default(128SPS), 1..8=8..860 */
        if (v < 0 || v > 8) { snprintf(out, outsz, "ERR ain rate 0-8 (0=128SPS)\r\n"); return CLI_ERR; }
        c->ain_rate = (uint8_t) v; c->applied_count++;
        snprintf(out, outsz, "OK ain rate=%d\r\n", v); return CLI_OK;   /* live on next sample */
    }
    if (sscanf(line, "ain gain %d", &v) == 1) {   /* 0=default(4.096V), 1..6=6.144..0.256 FSR */
        if (v < 0 || v > 6) { snprintf(out, outsz, "ERR ain gain 0-6 (0=4.096V)\r\n"); return CLI_ERR; }
        c->ain_gain = (uint8_t) v; c->applied_count++;
        snprintf(out, outsz, "OK ain gain=%d\r\n", v); return CLI_OK;
    }
    if (sscanf(line, "ain enable %d", &v) == 1) { /* activate the ADS1115 (save+reboot to apply) */
        c->ain_en = v ? 1 : 0; c->applied_count++;
        snprintf(out, outsz, "OK ain enable=%d (save+reboot to apply)\r\n", c->ain_en); return CLI_OK;
    }
    if (sscanf(line, "net mode %11s", w) == 1) {
        if      (!strcmp(w, "dhcp"))   c->net_mode = NET_MODE_DHCP;
        else if (!strcmp(w, "static")) c->net_mode = NET_MODE_STATIC;
        else { snprintf(out, outsz, "ERR net mode dhcp|static\r\n"); return CLI_ERR; }
        c->applied_count++; snprintf(out, outsz, "OK net mode=%s (save+reboot to apply)\r\n", dserv_netmode_str(c->net_mode)); return CLI_OK;
    }
    if (sscanf(line, "mode %11s", w) == 1) {      /* dual build: transport override (save+reboot) */
        int m = dserv_xport_val(w);
        if (m < 0) { snprintf(out, outsz, "ERR mode usb|eth|switch\r\n"); return CLI_ERR; }
        c->transport_mode = (uint8_t) m; c->applied_count++;
        snprintf(out, outsz, "OK transport mode=%s (save+reboot to apply)\r\n", dserv_xport_str((uint8_t)m)); return CLI_OK;
    }
    if (!strcmp(line, "show"))    { pico_cli_show(c, out, outsz); return CLI_OK; }
    if (!strcmp(line, "save"))    { snprintf(out, outsz, "saving...\r\n"); return CLI_SAVE; }
    if (!strcmp(line, "factory")) { snprintf(out, outsz, "factory reset...\r\n"); return CLI_FACTORY; }
    if (!strcmp(line, "reboot"))  { snprintf(out, outsz, "rebooting...\r\n"); return CLI_REBOOT; }
    if (!strcmp(line, "help")) {
        snprintf(out, outsz,
            "cmds: show | name NAME | mode usb|eth|switch | net mode dhcp|static | net ip A.B.C.D |\r\n"
            "      wifi ssid SSID | wifi pass PASS | wifi pm 0|1 | dserv ip A.B.C.D | dserv port N |\r\n"
            "      pin N mode out|in|in_pullup|off | pin N pulse US | pin N debounce MS |\r\n"
            "      pin N active_low 0|1 | obs pin N | obs off |\r\n"
            "      ain enable 0|1 | ain rate 0-8 | ain gain 0-6 |\r\n"
            "      do N 0|1 | do N pulse US | save | factory | reboot\r\n");
        return CLI_OK;
    }
    snprintf(out, outsz, "ERR unknown (try 'help')\r\n");
    return CLI_ERR;
}

#endif /* PICO_CLI_H */
