/*
 * dserv_config.h -- hardware-independent dispatch of the box's datapoints.
 *
 * Every datapoint the box cares about lives under a CONFIGURABLE device name
 * (default "pico"), so multiple boxes coexist on one dserv (e.g. "io1", "rig2").
 * The name is persisted config; set it over the USB CLI (`name io1`) or via the
 * datapoint <name>/config/name. dserv relays <name>/config/(keys) and <name>/gpio/(keys)
 * to the box (%reg ... 1 + matches).
 *
 *   <name>/config/name            (str)  rename the device
 *   <name>/config/pin/<n>/mode    (int)  0=unset 1=out 2=in 3=in_pullup
 *   <name>/config/pin/<n>/pulse_us(int)  default DO pulse width for pin n
 *   <name>/config/net/ip          (str)  "a.b.c.d" (applied on save/boot)
 *   <name>/config/dserv/ip        (str)  "a.b.c.d"
 *   <name>/config/dserv/port      (int)
 *   <name>/config/save            (any)  persist to flash
 *   <name>/gpio/<n>               (int)  drive pin level 0/1        [transient]
 *   <name>/gpio/<n>/pulse_us      (int)  box-timed high pulse (us)  [transient]
 *
 * config/(keys) are persistent settings; gpio/(keys) are transient commands (never saved).
 * config_apply mutates the struct (portable); a gpio command is returned as a
 * gpio_cmd_t for the platform layer (real GPIO on device, print in sim).
 *
 * Pure C, zero-alloc, no hardware. Shared by firmware and simulator.
 */
#ifndef DSERV_CONFIG_H
#define DSERV_CONFIG_H

#include "dserv_msg.h"
#include <stdio.h>       /* sscanf */

#define PICO_NPINS     30
#define PICO_NAME_MAX  16

/* Namespace class: every box publishes/subscribes under <BOX_CLASS>/<name>/... so
 * all external I/O boxes group under one parent (extio/) in the datapoint tree,
 * clearly separated from local ess/gpio/mtouch/etc. One-line change to rename. */
#define BOX_CLASS "extio"

typedef struct {
    char     name[PICO_NAME_MAX];       /* device name (within BOX_CLASS/); "" => "pico" */
    uint8_t  pin_mode[PICO_NPINS];      /* 0 unset, 1 out, 2 in, 3 in_pullup  */
    uint32_t do_pulse_us[PICO_NPINS];
    uint8_t  net_ip[4];
    uint8_t  dserv_ip[4];
    uint16_t dserv_port;
    uint32_t applied_count;             /* persistent settings applied        */
    uint8_t  debounce_ms[PICO_NPINS];   /* per-input debounce window, 0 = off */
    uint8_t  net_mode;                  /* 0=DHCP (default), 1=static; NET_MODE_* */
    uint8_t  obs_pin;                   /* GPIO driven to the live ess/in_obs copy (valid iff obs_en) */
    uint8_t  obs_en;                    /* 1 = obs-mirror on; 0 = off (default). Separate flag so any
                                         * GPIO incl GP0 is usable and factory-zero == off. */
    uint32_t di_active_low;             /* bitmask: bit n set => publish state/di/n inverted (pressed=1,
                                         * matching the local gpio ACTIVE_LOW convention) */
    char     wifi_ssid[32];             /* pico2w WiFi creds set at RUNTIME (USB CLI / datapoint);
                                         * empty => fall back to compile-time WIFI_SSID/PASSWORD */
    char     wifi_pass[64];
    uint8_t  wifi_pm;                   /* pico2w: 1 = WiFi power-save (battery), 0 = off (low latency) */
    uint8_t  ain_rate;                  /* ADS1115 data rate: 0=default(128SPS), 1..8=8/16/32/64/128/250/475/860 */
    uint8_t  ain_gain;                  /* ADS1115 PGA/FSR:   0=default(4.096V), 1..6=6.144/4.096/2.048/1.024/0.512/0.256 */
    uint8_t  ain_en;                    /* 1 = activate ADS1115 analog-in (ADC must be wired); 0 = off (default),
                                         * leaves the I2C pins free for GPIO. Applied at boot (save+reboot). */
    uint8_t  transport_mode;            /* DEPRECATED/unused: transport is now the GP28 boot strap, not config.
                                         * Kept only to preserve the append-only pico_persist v11 layout. */
} pico_config_t;

/* pico_config_t.net_mode. Zeroed default (factory/blank config) => DHCP, so a
 * fresh box works out of the box on a router; a static net/ip is honored only in
 * NET_MODE_STATIC. DHCP falls back to the static/default IP if no lease. */
enum { NET_MODE_DHCP = 0, NET_MODE_STATIC = 1 };

typedef enum { GPIO_OP_NONE = 0, GPIO_OP_SET, GPIO_OP_PULSE,
               GPIO_OP_SCHED_PULSE,   /* fire a pulse at beginobs + value(us)  */
               GPIO_OP_SCHED_TIMER    /* post state/timer/<pin> at beginobs + value(us) */
} gpio_op_t;
typedef struct { gpio_op_t op; uint8_t pin; uint32_t value; } gpio_cmd_t;

typedef enum {
    CFG_NONE = 0,   /* not under this box's name          */
    CFG_NAME,
    CFG_PIN_MODE,
    CFG_DO_PULSE,
    CFG_DEBOUNCE,
    CFG_NET_IP,
    CFG_NET_MODE,
    CFG_OBS_PIN,
    CFG_ACTIVE_LOW,
    CFG_WIFI_SSID,
    CFG_WIFI_PASS,
    CFG_WIFI_PM,
    CFG_AIN_RATE,
    CFG_AIN_GAIN,
    CFG_AIN_EN,
    CFG_DSERV_IP,
    CFG_DSERV_PORT,
    CFG_GPIO,       /* a cmd/do output command parsed into *cmd */
    CFG_SAVE,
    CFG_REBOOT,
    CFG_FACTORY,
    CFG_BOOTSEL,    /* cmd/bootsel -> reboot into USB BOOTSEL for reflashing */
    CFG_UNKNOWN     /* under this box's name but unrecognized */
} cfg_result_t;

/* Build "<BOX_CLASS>/<name>/state/<leaf>" for datapoints the box PUBLISHES. */
static inline void dserv_state_name(const pico_config_t *c, char *buf, int sz,
                                    const char *leaf);

static inline const char *dserv_cfg_name(const pico_config_t *c)
{ return c->name[0] ? c->name : "pico"; }

/* The box's datapoint prefix "<BOX_CLASS>/<name>" (e.g. extio/office). Returns len. */
static inline int dserv_cfg_prefix(const pico_config_t *c, char *buf, int sz)
{ return snprintf(buf, sz, "%s/%s", BOX_CLASS, dserv_cfg_name(c)); }

/* per-pin active-low (invert published DI level). */
static inline int  di_active_low(const pico_config_t *c, int pin)
{ return (pin >= 0 && pin < PICO_NPINS) ? (int) ((c->di_active_low >> pin) & 1u) : 0; }
static inline void di_active_low_set(pico_config_t *c, int pin, int on)
{ if (pin < 0 || pin >= PICO_NPINS) return;
  if (on) c->di_active_low |= (1u << pin); else c->di_active_low &= ~(1u << pin); }

/* pin mode word -> value; -1 if not a recognized word (caller falls back to int) */
static inline int dserv_mode_val(const char *w)
{
    if (!strcmp(w, "out"))       return 1;
    if (!strcmp(w, "in"))        return 2;
    if (!strcmp(w, "in_pullup")) return 3;
    if (!strcmp(w, "off") || !strcmp(w, "unset")) return 0;
    return -1;
}
static inline const char *dserv_mode_str(uint8_t m)
{ return m == 1 ? "out" : m == 2 ? "in" : m == 3 ? "in_pullup" : "off"; }

static inline const char *dserv_netmode_str(uint8_t mode)
{ return mode == NET_MODE_STATIC ? "static" : "dhcp"; }

/* Transport backends for the DUAL build. Which one boots is decided by the GP28
 * hardware strap at boot (see BOX_MODE_STRAP_PIN in pico_gpio.h), not by config:
 * open/high = USB, tied to GND = Ethernet. dserv_xport_str is used only to log the
 * strap's decision. No fragile boot-time cable auto-detect. */
enum { XPORT_USB = 0, XPORT_ETH = 1, XPORT_SWITCH = 2 };
static inline const char *dserv_xport_str(uint8_t m)
{ return m == XPORT_ETH ? "eth" : m == XPORT_SWITCH ? "switch" : "usb"; }

/* obs-mirror accessors: enable is a flag distinct from the pin, so GP0 is a
 * valid mirror pin and the zeroed factory default is "off". */
static inline int  obs_mirror_enabled(const pico_config_t *c) { return c->obs_en != 0; }
static inline int  obs_mirror_pin(const pico_config_t *c)     { return (int) c->obs_pin; }
static inline void obs_mirror_set(pico_config_t *c, int gpio) { c->obs_pin = (uint8_t) gpio; c->obs_en = 1; }
static inline void obs_mirror_off(pico_config_t *c)          { c->obs_en = 0; }

/* The device name is a datapoint-prefix: it must be non-empty, printable, and
 * carry no '/' (or a stray control byte from line-editing) -- a bad name breaks
 * the config/cmd/state namespace match silently. Hyphens are fine. */
static inline int dserv_name_valid(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) if ((unsigned char) *s < 0x20 || (unsigned char) *s > 0x7e || *s == '/') return 0;
    return 1;
}

static inline int dserv_cfg__parse_ip(const char *s, uint8_t out[4])
{
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -1;
    if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
    out[0]=(uint8_t)a; out[1]=(uint8_t)b; out[2]=(uint8_t)c; out[3]=(uint8_t)d;
    return 0;
}

static inline cfg_result_t dserv_cfg__config(pico_config_t *c, const char *k,
                                             const dserv_msg_t *m)
{
    int n, pos = -1;
    if (sscanf(k, "pin/%d/mode%n", &n, &pos) == 1 && pos > 0 &&
        k[pos] == '\0' && n >= 0 && n < PICO_NPINS) {
        char w[16]; dserv_msg_copy_cstr(m, w, sizeof w);   /* accept word or int */
        int mv = dserv_mode_val(w);
        if (mv < 0) mv = (int) dserv_msg_as_long(m);
        c->pin_mode[n] = (uint8_t) mv; c->applied_count++; return CFG_PIN_MODE;
    }
    pos = -1;
    if (sscanf(k, "pin/%d/pulse_us%n", &n, &pos) == 1 && pos > 0 &&
        k[pos] == '\0' && n >= 0 && n < PICO_NPINS) {
        c->do_pulse_us[n] = (uint32_t) dserv_msg_as_long(m); c->applied_count++; return CFG_DO_PULSE;
    }
    pos = -1;
    if (sscanf(k, "pin/%d/debounce_ms%n", &n, &pos) == 1 && pos > 0 &&
        k[pos] == '\0' && n >= 0 && n < PICO_NPINS) {
        long v = dserv_msg_as_long(m); if (v < 0) v = 0; if (v > 255) v = 255;
        c->debounce_ms[n] = (uint8_t) v; c->applied_count++; return CFG_DEBOUNCE;
    }
    pos = -1;
    if (sscanf(k, "pin/%d/active_low%n", &n, &pos) == 1 && pos > 0 &&
        k[pos] == '\0' && n >= 0 && n < PICO_NPINS) {
        di_active_low_set(c, n, dserv_msg_as_long(m) ? 1 : 0); c->applied_count++; return CFG_ACTIVE_LOW;
    }
    if (strcmp(k, "name") == 0) {
        char w[PICO_NAME_MAX]; dserv_msg_copy_cstr(m, w, sizeof w);
        if (!dserv_name_valid(w)) return CFG_UNKNOWN;
        strncpy(c->name, w, sizeof c->name - 1); c->name[sizeof c->name - 1] = '\0';
        c->applied_count++; return CFG_NAME;
    }
    if (strcmp(k, "net/ip") == 0) {
        char ip[24]; dserv_msg_copy_cstr(m, ip, sizeof ip);
        if (dserv_cfg__parse_ip(ip, c->net_ip) == 0) { c->applied_count++; return CFG_NET_IP; }
        return CFG_UNKNOWN;
    }
    if (strcmp(k, "net/mode") == 0) {
        char w[12]; dserv_msg_copy_cstr(m, w, sizeof w);
        if      (!strcmp(w, "dhcp"))   c->net_mode = NET_MODE_DHCP;
        else if (!strcmp(w, "static")) c->net_mode = NET_MODE_STATIC;
        else c->net_mode = dserv_msg_as_long(m) ? NET_MODE_STATIC : NET_MODE_DHCP;
        c->applied_count++; return CFG_NET_MODE;
    }
    if (strcmp(k, "dserv/ip") == 0) {
        char ip[24]; dserv_msg_copy_cstr(m, ip, sizeof ip);
        if (dserv_cfg__parse_ip(ip, c->dserv_ip) == 0) { c->applied_count++; return CFG_DSERV_IP; }
        return CFG_UNKNOWN;
    }
    if (strcmp(k, "dserv/port") == 0) {
        c->dserv_port = (uint16_t) dserv_msg_as_long(m); c->applied_count++; return CFG_DSERV_PORT;
    }
    if (strcmp(k, "wifi/ssid") == 0) {
        dserv_msg_copy_cstr(m, c->wifi_ssid, sizeof c->wifi_ssid); c->applied_count++; return CFG_WIFI_SSID;
    }
    if (strcmp(k, "wifi/pass") == 0) {
        dserv_msg_copy_cstr(m, c->wifi_pass, sizeof c->wifi_pass); c->applied_count++; return CFG_WIFI_PASS;
    }
    if (strcmp(k, "wifi/pm") == 0) {
        c->wifi_pm = dserv_msg_as_long(m) ? 1 : 0; c->applied_count++; return CFG_WIFI_PM;
    }
    if (strcmp(k, "ain/rate") == 0) {
        long v = dserv_msg_as_long(m); if (v < 0 || v > 8) return CFG_UNKNOWN;
        c->ain_rate = (uint8_t) v; c->applied_count++; return CFG_AIN_RATE;
    }
    if (strcmp(k, "ain/gain") == 0) {
        long v = dserv_msg_as_long(m); if (v < 0 || v > 6) return CFG_UNKNOWN;
        c->ain_gain = (uint8_t) v; c->applied_count++; return CFG_AIN_GAIN;
    }
    if (strcmp(k, "ain/enable") == 0) {
        c->ain_en = dserv_msg_as_long(m) ? 1 : 0; c->applied_count++; return CFG_AIN_EN;
    }
    if (strcmp(k, "obs/pin") == 0) {
        char w[8]; dserv_msg_copy_cstr(m, w, sizeof w);
        if (m->type == DSERV_STRING && !strcmp(w, "off")) obs_mirror_off(c);
        else {
            long v = dserv_msg_as_long(m);
            if (v >= 0 && v < PICO_NPINS) obs_mirror_set(c, (int) v);  /* 0..29, GP0 ok */
            else obs_mirror_off(c);                                    /* out-of-range = off */
        }
        c->applied_count++; return CFG_OBS_PIN;
    }
    return CFG_UNKNOWN;
}

/* cmd/ actions: do/<n> level, do/<n>/pulse_us pulse, save|reboot|factory */
static inline cfg_result_t dserv_cfg__cmd(const char *k, const dserv_msg_t *m,
                                          gpio_cmd_t *cmd)
{
    int n, pos = -1;
    if (sscanf(k, "do/%d%n", &n, &pos) == 1 && pos > 0 && k[pos] == '\0' &&
        n >= 0 && n < PICO_NPINS) {
        cmd->op = GPIO_OP_SET; cmd->pin = (uint8_t) n;
        cmd->value = dserv_msg_as_long(m) ? 1 : 0; return CFG_GPIO;
    }
    pos = -1;
    if (sscanf(k, "do/%d/pulse_us%n", &n, &pos) == 1 && pos > 0 && k[pos] == '\0' &&
        n >= 0 && n < PICO_NPINS) {
        cmd->op = GPIO_OP_PULSE; cmd->pin = (uint8_t) n;
        cmd->value = (uint32_t) dserv_msg_as_long(m); return CFG_GPIO;
    }
    pos = -1;   /* do/<n>/at <us> : schedule a pulse at beginobs + <us> */
    if (sscanf(k, "do/%d/at%n", &n, &pos) == 1 && pos > 0 && k[pos] == '\0' &&
        n >= 0 && n < PICO_NPINS) {
        cmd->op = GPIO_OP_SCHED_PULSE; cmd->pin = (uint8_t) n;
        cmd->value = (uint32_t) dserv_msg_as_long(m); return CFG_GPIO;
    }
    pos = -1;   /* timer/<n>/at <us> : post state/timer/<n> at beginobs + <us> */
    if (sscanf(k, "timer/%d/at%n", &n, &pos) == 1 && pos > 0 && k[pos] == '\0' &&
        n >= 0 && n < 64) {
        cmd->op = GPIO_OP_SCHED_TIMER; cmd->pin = (uint8_t) n;
        cmd->value = (uint32_t) dserv_msg_as_long(m); return CFG_GPIO;
    }
    if (strcmp(k, "save")    == 0) return CFG_SAVE;
    if (strcmp(k, "reboot")  == 0) return CFG_REBOOT;
    if (strcmp(k, "factory") == 0) return CFG_FACTORY;
    if (strcmp(k, "bootsel") == 0) return CFG_BOOTSEL;
    return CFG_UNKNOWN;
}

/* Route one datapoint. Config keys mutate *c; a gpio command fills *cmd (set
 * cmd->op == GPIO_OP_NONE first). Pass cmd=NULL if you only handle config. */
static inline cfg_result_t dserv_dispatch(pico_config_t *c, const dserv_msg_t *m,
                                          gpio_cmd_t *cmd)
{
    if (cmd) cmd->op = GPIO_OP_NONE;

    char pfx[DSERV_MSG_MAX_PAYLOAD + 1];
    int plen = dserv_cfg_prefix(c, pfx, sizeof pfx);   /* "extio/<name>" */
    if (plen <= 0 || m->namelen < (uint16_t)(plen + 1)) return CFG_NONE;
    if (memcmp(m->name, pfx, (size_t) plen) != 0 || m->name[plen] != '/') return CFG_NONE;

    char sub[DSERV_MSG_MAX_PAYLOAD + 1];
    uint16_t sl = (uint16_t)(m->namelen - (plen + 1));
    if (sl > DSERV_MSG_MAX_PAYLOAD) sl = DSERV_MSG_MAX_PAYLOAD;
    memcpy(sub, m->name + plen + 1, sl); sub[sl] = '\0';

    if (strncmp(sub, "config/", 7) == 0) return dserv_cfg__config(c, sub + 7, m);
    if (strncmp(sub, "cmd/", 4) == 0 && cmd) return dserv_cfg__cmd(sub + 4, m, cmd);
    return CFG_UNKNOWN;
}

/* Config-only convenience (ignores gpio commands). */
static inline cfg_result_t dserv_config_apply(pico_config_t *c, const dserv_msg_t *m)
{ gpio_cmd_t d; return dserv_dispatch(c, m, &d); }

static inline const char *dserv_cfg_result_str(cfg_result_t r)
{
    switch (r) {
    case CFG_NONE:       return "none";
    case CFG_NAME:       return "name";
    case CFG_PIN_MODE:   return "pin_mode";
    case CFG_DO_PULSE:   return "do_pulse";
    case CFG_DEBOUNCE:   return "debounce";
    case CFG_NET_IP:     return "net_ip";
    case CFG_NET_MODE:   return "net_mode";
    case CFG_OBS_PIN:    return "obs_pin";
    case CFG_ACTIVE_LOW: return "active_low";
    case CFG_WIFI_SSID:  return "wifi_ssid";
    case CFG_WIFI_PASS:  return "wifi_pass";
    case CFG_WIFI_PM:    return "wifi_pm";
    case CFG_AIN_RATE:   return "ain_rate";
    case CFG_AIN_GAIN:   return "ain_gain";
    case CFG_AIN_EN:     return "ain_en";
    case CFG_DSERV_IP:   return "dserv_ip";
    case CFG_DSERV_PORT: return "dserv_port";
    case CFG_GPIO:       return "cmd_do";
    case CFG_SAVE:       return "save";
    case CFG_REBOOT:     return "reboot";
    case CFG_FACTORY:    return "factory";
    case CFG_BOOTSEL:    return "bootsel";
    default:             return "unknown";
    }
}

static inline void dserv_state_name(const pico_config_t *c, char *buf, int sz,
                                    const char *leaf)
{ snprintf(buf, sz, "%s/%s/state/%s", BOX_CLASS, dserv_cfg_name(c), leaf); }

#endif /* DSERV_CONFIG_H */
