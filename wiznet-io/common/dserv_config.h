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

typedef struct {
    char     name[PICO_NAME_MAX];       /* device prefix; "" => "pico"        */
    uint8_t  pin_mode[PICO_NPINS];      /* 0 unset, 1 out, 2 in, 3 in_pullup  */
    uint32_t do_pulse_us[PICO_NPINS];
    uint8_t  net_ip[4];
    uint8_t  dserv_ip[4];
    uint16_t dserv_port;
    uint32_t applied_count;             /* persistent settings applied        */
    uint8_t  debounce_ms[PICO_NPINS];   /* per-input debounce window, 0 = off */
} pico_config_t;

typedef enum { GPIO_OP_NONE = 0, GPIO_OP_SET, GPIO_OP_PULSE } gpio_op_t;
typedef struct { gpio_op_t op; uint8_t pin; uint32_t value; } gpio_cmd_t;

typedef enum {
    CFG_NONE = 0,   /* not under this box's name          */
    CFG_NAME,
    CFG_PIN_MODE,
    CFG_DO_PULSE,
    CFG_DEBOUNCE,
    CFG_NET_IP,
    CFG_DSERV_IP,
    CFG_DSERV_PORT,
    CFG_GPIO,       /* a cmd/do output command parsed into *cmd */
    CFG_SAVE,
    CFG_REBOOT,
    CFG_FACTORY,
    CFG_UNKNOWN     /* under this box's name but unrecognized */
} cfg_result_t;

/* Build "<name>/state/<leaf>" for datapoints the box PUBLISHES to dserv. */
static inline void dserv_state_name(const pico_config_t *c, char *buf, int sz,
                                    const char *leaf);

static inline const char *dserv_cfg_name(const pico_config_t *c)
{ return c->name[0] ? c->name : "pico"; }

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
    if (strcmp(k, "name") == 0) {
        dserv_msg_copy_cstr(m, c->name, sizeof c->name); c->applied_count++; return CFG_NAME;
    }
    if (strcmp(k, "net/ip") == 0) {
        char ip[24]; dserv_msg_copy_cstr(m, ip, sizeof ip);
        if (dserv_cfg__parse_ip(ip, c->net_ip) == 0) { c->applied_count++; return CFG_NET_IP; }
        return CFG_UNKNOWN;
    }
    if (strcmp(k, "dserv/ip") == 0) {
        char ip[24]; dserv_msg_copy_cstr(m, ip, sizeof ip);
        if (dserv_cfg__parse_ip(ip, c->dserv_ip) == 0) { c->applied_count++; return CFG_DSERV_IP; }
        return CFG_UNKNOWN;
    }
    if (strcmp(k, "dserv/port") == 0) {
        c->dserv_port = (uint16_t) dserv_msg_as_long(m); c->applied_count++; return CFG_DSERV_PORT;
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
    if (strcmp(k, "save")    == 0) return CFG_SAVE;
    if (strcmp(k, "reboot")  == 0) return CFG_REBOOT;
    if (strcmp(k, "factory") == 0) return CFG_FACTORY;
    return CFG_UNKNOWN;
}

/* Route one datapoint. Config keys mutate *c; a gpio command fills *cmd (set
 * cmd->op == GPIO_OP_NONE first). Pass cmd=NULL if you only handle config. */
static inline cfg_result_t dserv_dispatch(pico_config_t *c, const dserv_msg_t *m,
                                          gpio_cmd_t *cmd)
{
    if (cmd) cmd->op = GPIO_OP_NONE;

    const char *nm = dserv_cfg_name(c);
    uint16_t nlen = (uint16_t) strlen(nm);
    if (m->namelen < (uint16_t)(nlen + 1)) return CFG_NONE;
    if (memcmp(m->name, nm, nlen) != 0 || m->name[nlen] != '/') return CFG_NONE;

    char sub[DSERV_MSG_MAX_PAYLOAD + 1];
    uint16_t sl = (uint16_t)(m->namelen - (nlen + 1));
    if (sl > DSERV_MSG_MAX_PAYLOAD) sl = DSERV_MSG_MAX_PAYLOAD;
    memcpy(sub, m->name + nlen + 1, sl); sub[sl] = '\0';

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
    case CFG_DSERV_IP:   return "dserv_ip";
    case CFG_DSERV_PORT: return "dserv_port";
    case CFG_GPIO:       return "cmd_do";
    case CFG_SAVE:       return "save";
    case CFG_REBOOT:     return "reboot";
    case CFG_FACTORY:    return "factory";
    default:             return "unknown";
    }
}

static inline void dserv_state_name(const pico_config_t *c, char *buf, int sz,
                                    const char *leaf)
{ snprintf(buf, sz, "%s/state/%s", dserv_cfg_name(c), leaf); }

#endif /* DSERV_CONFIG_H */
