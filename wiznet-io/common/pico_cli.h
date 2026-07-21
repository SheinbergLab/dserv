/*
 * pico_cli.h -- portable line-oriented CLI for the box, used over USB-CDC on the
 * device (bootstrap before the network is up + recovery) and over stdin in the
 * simulator. Pure C. One command per line; writes a human response into `out`
 * and returns an action for the caller to perform platform-specific storage.
 *
 * Commands:
 *   help
 *   show
 *   mode auto|usb|eth    (dual image: boot transport policy when the GP28 strap
 *                         is open; strap to GND still hard-forces Ethernet)
 *   net mode dhcp|static
 *   net ip A.B.C.D       (also sets mode=static)
 *   wifi ssid SSID       (pico2w; runtime creds, else compile-time fallback)
 *   wifi pass PASS
 *   dserv ip A.B.C.D
 *   dserv port N
 *   pin N mode out|in|in_pullup|off
 *   pin N pulse US
 *   desc TEXT...         (free-form box description; `desc off` clears)
 *   label N TEXT|off     (per-pin role label -> announced manifest)
 *   group G pins 2,3,4,5 (DI chord group; `group G off` clears)
 *   group G label NAME | group G settle MS | group G quiet 0|1
 *   save          -> CLI_SAVE   (caller persists to flash/file)
 *   factory       -> CLI_FACTORY(caller erases storage + resets cfg)
 *   reboot        -> CLI_REBOOT (caller resets the MCU)
 */
#ifndef PICO_CLI_H
#define PICO_CLI_H

#include "dserv_config.h"
#include <stdio.h>
#include <string.h>

/* CLI_GROUP = labels/groups/desc changed: caller refreshes the group runtime
 * and re-announces the manifest (no GPIO re-apply needed). */
typedef enum { CLI_OK, CLI_ERR, CLI_PIN, CLI_GROUP, CLI_AIN, CLI_GPIO, CLI_SAVE, CLI_FACTORY, CLI_REBOOT, CLI_BOOTSEL } cli_action_t;

/* mode word<->value shared with dserv_config.h: dserv_mode_val / dserv_mode_str */

static inline void pico_cli_show(const pico_config_t *c, char *out, int outsz)
{
    char obs[8], syn[8];
    if (obs_mirror_enabled(c))  snprintf(obs, sizeof obs, "%d", obs_mirror_pin(c));
    else                        snprintf(obs, sizeof obs, "off");
    if (sync_input_enabled(c))  snprintf(syn, sizeof syn, "%d", sync_input_pin(c));
    else                        snprintf(syn, sizeof syn, "off");
    int k = 0;
#ifdef BOX_NET_DUAL
    k += snprintf(out + k, outsz - k, "mode=%s ", dserv_xmode_str(c->transport_mode));
#endif
    k += snprintf(out + k, outsz - k,
        "name=%s net.mode=%s net.ip=%u.%u.%u.%u dserv=%u.%u.%u.%u:%u obs.pin=%s sync.pin=%s wifi.ssid=%s wifi.pass=%s wifi.pm=%u mcp.en=%u oled.en=%u ble.en=%u pipe.en=%u applied=%u\r\n",
        dserv_cfg_name(c), dserv_netmode_str(c->net_mode),
        c->net_ip[0], c->net_ip[1], c->net_ip[2], c->net_ip[3],
        c->dserv_ip[0], c->dserv_ip[1], c->dserv_ip[2], c->dserv_ip[3],
        dserv_cfg_port(c), obs, syn,   /* effective port (default when unset), not raw 0 */
        c->wifi_ssid[0] ? c->wifi_ssid : "(build)", c->wifi_pass[0] ? "set" : "(build)",
        c->wifi_pm, c->mcp_en, c->oled_en, c->ble_en, c->pipe_en, c->applied_count);
    if (c->desc[0] && k < outsz - 8)
        k += snprintf(out + k, outsz - k, "  desc=%s\r\n", c->desc);
    for (int i = 0; i < PICO_NPINS && k < outsz - 64; i++)
        if (c->pin_mode[i])
            k += snprintf(out + k, outsz - k, "  pin%d=%s pulse=%uus debounce=%ums%s%s%s\r\n",
                          i, dserv_mode_str(c->pin_mode[i]), c->do_pulse_us[i], c->debounce_ms[i],
                          di_active_low(c, i) ? " active_low" : "",
                          c->pin_label[i][0] ? " label=" : "", c->pin_label[i]);
    for (int g = 0; g < PICO_NGROUPS && k < outsz - 96; g++)
        if (c->group_pins[g]) {
            char gn[PICO_LABEL_MAX + 4], ps[96];
            dserv_group_name(c, g, gn, sizeof gn);
            dserv_pins_str(c->group_pins[g], ps, sizeof ps);
            k += snprintf(out + k, outsz - k, "  group%d=%s pins=%s settle=%ums%s\r\n",
                          g, gn, ps, c->group_settle_ms[g], c->group_quiet[g] ? " quiet" : "");
        }
}

/* Emit the CLI commands that reproduce this config (only the non-default settings), so
 * pasting the output into a fresh box's console clones this setup. Ends with `save`.
 * Uses printf (not `out`) so a big config isn't bounded by the response buffer. Comment
 * (#) lines are ignored by pico_cli_exec, so the whole capture pastes back cleanly. */
static inline void pico_cli_dump(const pico_config_t *c)
{
    printf("# extio box config dump -- paste into a new box's console to clone this setup\r\n");
    printf("# (uncomment the next line to wipe the target's existing config first)\r\n");
    printf("#factory\r\n");
    if (c->name[0])                       printf("name %s\r\n", c->name);
#ifdef BOX_NET_DUAL
    if (c->transport_mode)                printf("mode %s\r\n", dserv_xmode_str(c->transport_mode));
#endif
    if (c->net_mode == NET_MODE_STATIC) {
        printf("net mode static\r\n");
        printf("net ip %u.%u.%u.%u\r\n", c->net_ip[0], c->net_ip[1], c->net_ip[2], c->net_ip[3]);
    }
    if (c->dserv_ip[0] || c->dserv_ip[1] || c->dserv_ip[2] || c->dserv_ip[3])
        printf("dserv ip %u.%u.%u.%u\r\n", c->dserv_ip[0], c->dserv_ip[1], c->dserv_ip[2], c->dserv_ip[3]);
    if (c->dserv_port)                    printf("dserv port %u\r\n", c->dserv_port);
    for (int i = 0; i < PICO_NPINS; i++) {
        if (c->pin_mode[i])      printf("pin %d mode %s\r\n",     i, dserv_mode_str(c->pin_mode[i]));
        if (c->do_pulse_us[i])   printf("pin %d pulse %u\r\n",    i, (unsigned) c->do_pulse_us[i]);
        if (c->debounce_ms[i])   printf("pin %d debounce %u\r\n", i, c->debounce_ms[i]);
        if (di_active_low(c, i)) printf("pin %d active_low 1\r\n", i);
        if (c->pin_label[i][0])  printf("label %d %s\r\n",        i, c->pin_label[i]);
    }
    if (c->desc[0])                       printf("desc %s\r\n", c->desc);
    if (c->channel[0])                    printf("channel %s\r\n", c->channel);
    for (int g = 0; g < PICO_NGROUPS; g++)
        if (c->group_pins[g]) {
            char ps[96]; dserv_pins_str(c->group_pins[g], ps, sizeof ps);
            printf("group %d pins %s\r\n", g, ps);
            if (c->group_label[g][0])     printf("group %d label %s\r\n",  g, c->group_label[g]);
            if (c->group_settle_ms[g])    printf("group %d settle %u\r\n", g, c->group_settle_ms[g]);
            if (c->group_quiet[g])        printf("group %d quiet 1\r\n",   g);
        }
    if (obs_mirror_enabled(c))            printf("obs pin %d\r\n", obs_mirror_pin(c));
    if (sync_input_enabled(c))            printf("sync pin %d\r\n", sync_input_pin(c));
    if (c->wifi_ssid[0])                  printf("wifi ssid %s\r\n", c->wifi_ssid);
    if (c->wifi_pass[0])                  printf("# wifi pass <re-enter manually; not dumped>\r\n");
    if (c->wifi_pm)                       printf("wifi pm 1\r\n");
    if (c->mcp_en)                        printf("mcp enable 1\r\n");
    if (c->mcp_rate)                      printf("mcp rate %u\r\n", c->mcp_rate);
    for (int ag = 0; ag < PICO_NAGROUPS; ag++)
        if (c->ain_group_chans[ag]) {
            char cs[16]; dserv_pins_str(c->ain_group_chans[ag], cs, sizeof cs);
            printf("ain group %d channels %s\r\n", ag, cs);
            if (c->ain_group_label[ag][0]) printf("ain group %d label %s\r\n",    ag, c->ain_group_label[ag]);
            if (c->ain_group_mode[ag])     printf("ain group %d mode continuous\r\n", ag);
            if (c->ain_group_deadband[ag]) printf("ain group %d deadband %u\r\n", ag, c->ain_group_deadband[ag]);
            if (c->ain_group_decimate[ag]) printf("ain group %d decimate %u\r\n", ag, c->ain_group_decimate[ag]);
            if (c->ain_group_batch[ag])    printf("ain group %d batch %u\r\n",    ag, c->ain_group_batch[ag]);
            if (c->ain_group_flags[ag] & AIN_GROUP_FLAG_AVG) printf("ain group %d average 1\r\n", ag);
        }
    if (c->oled_en)                       printf("oled enable 1\r\n");
    if (c->ble_en)                        printf("ble enable 1\r\n");
    if (c->pipe_en)                       printf("ble pipe 1\r\n");
    if (c->ble_latency)                   printf("ble latency %u\r\n", c->ble_latency);
    printf("save\r\n");
    printf("# reboot   (uncomment / run to apply mode/net changes)\r\n");
}

#ifdef BOX_NET_DUAL
#define PICO_CLI_HELP_XTRA "mode auto|usb|eth | phylink [1|0] | "
#else
#define PICO_CLI_HELP_XTRA ""
#endif
#if defined(BOX_BLE)
#define PICO_CLI_HELP_BLE "ble | ble scan 1|0 | ble pipe 1|0 | ble latency <n> | ble pair <s> | ble forget | ble bonds | "   /* runtime radio cmds live in cmd_exec */
#elif defined(BOX_NET_BLE)
#define PICO_CLI_HELP_BLE "ble | "
#else
#define PICO_CLI_HELP_BLE ""
#endif

/* Execute one line. Returns an action; fills `out` with a response line.
 * `cmd` (may be NULL) receives a GPIO command for the `do` verbs (CLI_GPIO). */
static inline cli_action_t pico_cli_exec(pico_config_t *c, const char *line,
                                         char *out, int outsz, gpio_cmd_t *cmd)
{
    int n, v; char w[24];
    if (cmd) cmd->op = GPIO_OP_NONE;

    /* skip leading spaces; ignore blank lines and # comments (so a pasted `dump` -- which
     * includes header/# lines -- applies cleanly) */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '#') { out[0] = '\0'; return CLI_OK; }

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
    /* channel: the firmware update track the host tool compares against. Bare
     * `channel` queries; `channel dev`/`off` resets to the default (stored empty
     * -> reads as dev). Not part of the pin/group manifest, so CLI_OK -- it
     * re-announces as state/channel at the next (re)connect. */
    if (!strcmp(line, "channel")) {
        snprintf(out, outsz, "OK channel=%s\r\n", dserv_cfg_channel(c)); return CLI_OK;
    }
    if (sscanf(line, "channel %15s", w) == 1) {
        if (!strcmp(w, "off") || !strcmp(w, "dev")) c->channel[0] = '\0';
        else if (!dserv_name_valid(w)) { snprintf(out, outsz, "ERR channel: printable, no '/'\r\n"); return CLI_ERR; }
        else { strncpy(c->channel, w, sizeof c->channel - 1); c->channel[sizeof c->channel - 1] = '\0'; }
        c->applied_count++;
        snprintf(out, outsz, "OK channel=%s\r\n", dserv_cfg_channel(c)); return CLI_OK;
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
    if (sscanf(line, "label %d %15s", &n, w) == 2) {
        if (n < 0 || n >= PICO_NPINS) { snprintf(out, outsz, "ERR bad pin\r\n"); return CLI_ERR; }
        if (!strcmp(w, "off")) w[0] = '\0';
        if (!dserv_label_valid(w)) { snprintf(out, outsz, "ERR label: printable, no '/' or spaces\r\n"); return CLI_ERR; }
        snprintf(c->pin_label[n], PICO_LABEL_MAX, "%s", w);
        c->applied_count++;
        snprintf(out, outsz, "OK pin%d label=%s\r\n", n, c->pin_label[n][0] ? c->pin_label[n] : "(none)");
        return CLI_GROUP;
    }
    if (sscanf(line, "group %d pins %23s", &n, w) == 2) {
        uint32_t mask;
        if (n < 0 || n >= PICO_NGROUPS || dserv_parse_pins(w, &mask) < 0) {
            snprintf(out, outsz, "ERR group pins: 'group G pins 2,3,4,5' (G 0-%d, pins 0-%d)\r\n",
                     PICO_NGROUPS - 1, PICO_NPINS - 1);
            return CLI_ERR;
        }
        c->group_pins[n] = mask; c->applied_count++;
        snprintf(out, outsz, "OK group%d pins=%s\r\n", n, w); return CLI_GROUP;
    }
    if (sscanf(line, "group %d label %15s", &n, w) == 2) {
        if (n < 0 || n >= PICO_NGROUPS) { snprintf(out, outsz, "ERR bad group\r\n"); return CLI_ERR; }
        if (!strcmp(w, "off")) w[0] = '\0';
        if (!dserv_label_valid(w)) { snprintf(out, outsz, "ERR label: printable, no '/' or spaces\r\n"); return CLI_ERR; }
        snprintf(c->group_label[n], PICO_LABEL_MAX, "%s", w);
        c->applied_count++;
        snprintf(out, outsz, "OK group%d label=%s\r\n", n, c->group_label[n][0] ? c->group_label[n] : "(none)");
        return CLI_GROUP;
    }
    if (sscanf(line, "group %d settle %d", &n, &v) == 2) {
        if (n < 0 || n >= PICO_NGROUPS || v < 0 || v > 65535) {
            snprintf(out, outsz, "ERR group settle 0-65535 ms\r\n"); return CLI_ERR; }
        c->group_settle_ms[n] = (uint16_t) v; c->applied_count++;
        snprintf(out, outsz, "OK group%d settle=%dms\r\n", n, v); return CLI_GROUP;
    }
    if (sscanf(line, "group %d quiet %d", &n, &v) == 2) {
        if (n < 0 || n >= PICO_NGROUPS) { snprintf(out, outsz, "ERR bad group\r\n"); return CLI_ERR; }
        c->group_quiet[n] = v ? 1 : 0; c->applied_count++;
        snprintf(out, outsz, "OK group%d quiet=%d\r\n", n, c->group_quiet[n]); return CLI_GROUP;
    }
    if (sscanf(line, "group %d %23s", &n, w) == 2 && !strcmp(w, "off")) {
        if (n < 0 || n >= PICO_NGROUPS) { snprintf(out, outsz, "ERR bad group\r\n"); return CLI_ERR; }
        c->group_pins[n] = 0; c->applied_count++;
        snprintf(out, outsz, "OK group%d off\r\n", n); return CLI_GROUP;
    }
    /* ---- analog (MCP3204) groups: base scan rate + per-group channel-set policy ---- */
    if (sscanf(line, "mcp rate %d", &v) == 1) {
        if (v < 1 || v > 65535) { snprintf(out, outsz, "ERR mcp rate 1-65535 Hz\r\n"); return CLI_ERR; }
        c->mcp_rate = (uint16_t) v; c->applied_count++;
        snprintf(out, outsz, "OK mcp rate=%dHz (base scan; save+reboot)\r\n", v); return CLI_AIN;
    }
    if (sscanf(line, "ain group %d channels %23s", &n, w) == 2) {
        uint32_t mask;
        if (n < 0 || n >= PICO_NAGROUPS || dserv_parse_pins(w, &mask) < 0 || (mask & ~0x0Fu)) {
            snprintf(out, outsz, "ERR ain channels: 'ain group G channels 0,1' (G 0-%d, ch 0-3)\r\n",
                     PICO_NAGROUPS - 1);
            return CLI_ERR;
        }
        c->ain_group_chans[n] = (uint8_t) mask; c->applied_count++;
        snprintf(out, outsz, "OK ain group%d channels=%s\r\n", n, w); return CLI_AIN;
    }
    if (sscanf(line, "ain group %d label %15s", &n, w) == 2) {
        if (n < 0 || n >= PICO_NAGROUPS) { snprintf(out, outsz, "ERR bad group\r\n"); return CLI_ERR; }
        if (!strcmp(w, "off")) w[0] = '\0';
        if (!dserv_label_valid(w)) { snprintf(out, outsz, "ERR label: printable, no '/' or spaces\r\n"); return CLI_ERR; }
        snprintf(c->ain_group_label[n], PICO_LABEL_MAX, "%s", w); c->applied_count++;
        snprintf(out, outsz, "OK ain group%d label=%s\r\n", n,
                 c->ain_group_label[n][0] ? c->ain_group_label[n] : "(none)");
        return CLI_AIN;
    }
    if (sscanf(line, "ain group %d mode %15s", &n, w) == 2) {
        if (n < 0 || n >= PICO_NAGROUPS) { snprintf(out, outsz, "ERR bad group\r\n"); return CLI_ERR; }
        if (!strcmp(w, "continuous")) c->ain_group_mode[n] = 1;
        else if (!strcmp(w, "onchange")) c->ain_group_mode[n] = 0;
        else { snprintf(out, outsz, "ERR ain mode: onchange|continuous\r\n"); return CLI_ERR; }
        c->applied_count++; snprintf(out, outsz, "OK ain group%d mode=%s\r\n", n, w); return CLI_AIN;
    }
    if (sscanf(line, "ain group %d deadband %d", &n, &v) == 2) {
        if (n < 0 || n >= PICO_NAGROUPS || v < 0 || v > 4095) {
            snprintf(out, outsz, "ERR ain deadband 0-4095\r\n"); return CLI_ERR; }
        c->ain_group_deadband[n] = (uint16_t) v; c->applied_count++;
        snprintf(out, outsz, "OK ain group%d deadband=%d\r\n", n, v); return CLI_AIN;
    }
    if (sscanf(line, "ain group %d decimate %d", &n, &v) == 2) {
        if (n < 0 || n >= PICO_NAGROUPS || v < 1 || v > 255) {
            snprintf(out, outsz, "ERR ain decimate 1-255\r\n"); return CLI_ERR; }
        c->ain_group_decimate[n] = (uint8_t) v; c->applied_count++;
        snprintf(out, outsz, "OK ain group%d decimate=%d\r\n", n, v); return CLI_AIN;
    }
    if (sscanf(line, "ain group %d batch %d", &n, &v) == 2) {
        if (n < 0 || n >= PICO_NAGROUPS || v < 1 || v > 255) {
            snprintf(out, outsz, "ERR ain batch 1-255\r\n"); return CLI_ERR; }
        c->ain_group_batch[n] = (uint8_t) v; c->applied_count++;
        snprintf(out, outsz, "OK ain group%d batch=%d\r\n", n, v); return CLI_AIN;
    }
    if (sscanf(line, "ain group %d average %d", &n, &v) == 2) {
        if (n < 0 || n >= PICO_NAGROUPS) { snprintf(out, outsz, "ERR bad group\r\n"); return CLI_ERR; }
        if (v) c->ain_group_flags[n] |= AIN_GROUP_FLAG_AVG;
        else   c->ain_group_flags[n] &= (uint8_t) ~AIN_GROUP_FLAG_AVG;
        c->applied_count++; snprintf(out, outsz, "OK ain group%d average=%d\r\n", n, v ? 1 : 0); return CLI_AIN;
    }
    if (sscanf(line, "ain group %d %15s", &n, w) == 2 && !strcmp(w, "off")) {
        if (n < 0 || n >= PICO_NAGROUPS) { snprintf(out, outsz, "ERR bad group\r\n"); return CLI_ERR; }
        c->ain_group_chans[n] = 0; c->ain_group_label[n][0] = '\0'; c->applied_count++;
        snprintf(out, outsz, "OK ain group%d off\r\n", n); return CLI_AIN;
    }
    /* desc: value is the rest of the line verbatim, so spaces survive. */
    if (!strncmp(line, "desc ", 5)) {
        if (!strcmp(line + 5, "off")) c->desc[0] = '\0';
        else { strncpy(c->desc, line + 5, sizeof c->desc - 1); c->desc[sizeof c->desc - 1] = '\0'; }
        c->applied_count++;
        snprintf(out, outsz, "OK desc=%s\r\n", c->desc[0] ? c->desc : "(none)"); return CLI_GROUP;
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
    if (sscanf(line, "sync pin %d", &n) == 1) {   /* TTL obs-sync INPUT (hardware clock anchor) */
        if (n < 0 || n >= PICO_NPINS) { snprintf(out, outsz, "ERR sync pin 0-%d (or 'sync off')\r\n", PICO_NPINS - 1); return CLI_ERR; }
        sync_input_set(c, n); c->applied_count++;
        snprintf(out, outsz, "OK sync pin=%d (input; wire the rig host's obs TTL)\r\n", n); return CLI_PIN;
    }
    if (!strcmp(line, "sync off")) {
        sync_input_off(c); c->applied_count++;
        snprintf(out, outsz, "OK sync off\r\n"); return CLI_PIN;
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
    if (sscanf(line, "mcp enable %d", &v) == 1) { /* activate the MCP3204 SPI ADC (save+reboot to apply) */
        c->mcp_en = v ? 1 : 0; c->applied_count++;
        snprintf(out, outsz, "OK mcp enable=%d (claims SPI0 pins 2-5; save+reboot to apply)\r\n", c->mcp_en); return CLI_OK;
    }
    if (sscanf(line, "oled enable %d", &v) == 1) { /* SSD1306 SPI status display (see PINMAP.md) */
        c->oled_en = v ? 1 : 0; c->applied_count++;
        snprintf(out, outsz, "OK oled enable=%d (claims SPI0 pins; save+reboot to apply)\r\n", c->oled_en);
        return CLI_OK;
    }
    if (sscanf(line, "ble enable %d", &v) == 1) { /* BLE central (BOX_BLE radio builds; see BLE.md) */
        c->ble_en = v ? 1 : 0; c->applied_count++;
        snprintf(out, outsz, "OK ble enable=%d (live on radio builds; `save` to persist)\r\n", c->ble_en);
        return CLI_OK;
    }
    if (sscanf(line, "ble pipe %d", &v) == 1) {   /* receiver relay latch (v16); cmd_exec fires the
                                                   * live request first, then falls through here */
        c->pipe_en = v ? 1 : 0; c->applied_count++;
        snprintf(out, outsz, "OK ble pipe=%d (live on the receiver; `save` to auto-arm at boot)\r\n", c->pipe_en);
        return CLI_OK;
    }
    if (sscanf(line, "ble latency %d", &v) == 1) { /* idle peripheral-latency target (v17); read live by
                                                    * box_ble_latency_service. 0 = always-listen default */
        if (v < 0) v = 0;
        if (v > 30) v = 30;
        c->ble_latency = (uint8_t) v; c->applied_count++;
        snprintf(out, outsz, "OK ble latency=%d (0=always listen; raises when synced+idle; `save` to persist)\r\n",
                 c->ble_latency);
        return CLI_OK;
    }
    if (sscanf(line, "net mode %11s", w) == 1) {
        if      (!strcmp(w, "dhcp"))   c->net_mode = NET_MODE_DHCP;
        else if (!strcmp(w, "static")) c->net_mode = NET_MODE_STATIC;
        else { snprintf(out, outsz, "ERR net mode dhcp|static\r\n"); return CLI_ERR; }
        c->applied_count++; snprintf(out, outsz, "OK net mode=%s (save+reboot to apply)\r\n", dserv_netmode_str(c->net_mode)); return CLI_OK;
    }
#ifdef BOX_NET_DUAL
    /* Boot transport policy for the open-strap case (GND strap always forces eth).
     * Safe to persist again: with the core split, a bad choice can't kill the
     * console -- and auto never commits to a transport it can't bring up. */
    if (sscanf(line, "mode %7s", w) == 1) {
        int m = !strcmp(w, "auto") ? XMODE_AUTO :
                !strcmp(w, "eth")  ? XMODE_ETH  :
                !strcmp(w, "usb")  ? XMODE_USB  : -1;
        if (m < 0) { snprintf(out, outsz, "ERR mode auto|usb|eth\r\n"); return CLI_ERR; }
        c->transport_mode = (uint8_t) m; c->applied_count++;
        snprintf(out, outsz, "OK mode=%s (GND strap overrides; save+reboot to apply)\r\n",
                 dserv_xmode_str(c->transport_mode));
        return CLI_OK;
    }
#endif
    if (!strcmp(line, "show"))    { pico_cli_show(c, out, outsz); return CLI_OK; }
    if (!strcmp(line, "dump"))    { pico_cli_dump(c); out[0] = '\0'; return CLI_OK; }  /* config as replayable cmds */
    if (!strcmp(line, "save"))    { snprintf(out, outsz, "saving...\r\n"); return CLI_SAVE; }
    if (!strcmp(line, "factory")) { snprintf(out, outsz, "factory reset...\r\n"); return CLI_FACTORY; }
    if (!strcmp(line, "reboot"))  { snprintf(out, outsz, "rebooting...\r\n"); return CLI_REBOOT; }
    if (!strcmp(line, "bootsel")) { snprintf(out, outsz, "entering USB BOOTSEL (then: picotool load <uf2>)...\r\n"); return CLI_BOOTSEL; }
    if (!strcmp(line, "help")) {
        snprintf(out, outsz,
            "cmds: show | dump | name NAME | desc TEXT | channel NAME | " PICO_CLI_HELP_XTRA
            "net mode dhcp|static | net ip A.B.C.D |\r\n"
            "      wifi ssid SSID | wifi pass PASS | wifi pm 0|1 | dserv ip A.B.C.D | dserv port N |\r\n"
            "      pin N mode out|in|in_pullup|off | pin N pulse US | pin N debounce MS |\r\n"
            "      pin N active_low 0|1 | label N TEXT|off | obs pin N | obs off |\r\n"
            "      sync pin N | sync off |\r\n"
            "      group G pins 2,3,4,5 | group G label NAME | group G settle MS |\r\n"
            "      group G quiet 0|1 | group G off |\r\n"
            "      mcp enable 0|1 | mcp rate HZ | oled enable 0|1 |\r\n"
            "      ain group G channels 0,1 | ain group G label NAME | ain group G mode onchange|continuous |\r\n"
            "      ain group G deadband N | ain group G decimate N | ain group G batch N | ain group G average 0|1 | ain group G off |\r\n"
            "      ble enable 0|1 | " PICO_CLI_HELP_BLE "\r\n"
            "      do N 0|1 | do N pulse US | wdt 0|1|test | save | factory | reboot | bootsel\r\n");
        return CLI_OK;
    }
    snprintf(out, outsz, "ERR unknown (try 'help')\r\n");
    return CLI_ERR;
}

#endif /* PICO_CLI_H */
