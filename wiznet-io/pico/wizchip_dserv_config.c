/*
 * wizchip_dserv_config.c -- the "box" firmware. Transport-agnostic (box_net.h):
 * builds for wired W6300 (default) or Pico 2 W WiFi (-DBOX_NET_LWIP).
 *
 * Namespace: all boxes group under extio/<name>/... (BOX_CLASS="extio" + device name):
 *   extio/<name>/config/(keys)  host->box persistent settings (pushed by dserv; saved to flash)
 *   extio/<name>/cmd/(keys)     host->box transient actions   (pushed by dserv)
 *                       cmd/do/<n>=0|1, cmd/do/<n>/pulse_us, cmd/save|reboot|factory
 *   extio/<name>/state/(keys)   box->dserv published status   (box connects to dserv as client)
 *                       state/di/<n> (edge-timestamped, active_low-aware), state/do/<n> readback,
 *                       state/watchdog, state/uptime_us, state/link
 *                       state/sync/(dserv_us|box_us|offset_us)  clock-align audit trail
 *
 * Clock alignment: the box also subscribes to ess/in_obs. Each obs begin/end
 * edge carries dserv's timestamp; the box snaps a box->dserv offset from it
 * (pico_clock.h) and stamps the DI/DO events it publishes in dserv time, so they
 * interleave with local events as if the box were a local device. An optional
 * obs-mirror output (config/obs/pin, or CLI `obs pin N`) is driven to the live
 * ess/in_obs value -- an LED to eyeball obs tracking, or a scope tap to measure
 * the sync latency against the main system's physical obs line.
 *
 * dserv relay setup:  %reg <box_ip> <CFG_PORT> 1 ; %match <name>/config keys ; %match <name>/cmd keys ; %match ess/in_obs
 * dserv target for state:  set <name>/config/dserv/ip + dserv/port, then save.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#include "dserv_config.h"
#include "pico_persist.h"
#include "pico_cli.h"
#include "pico_flash.h"
#include "pico_gpio.h"
#include "pico_clock.h"
#include "box_net.h"

#define CFG_PORT    5010
#define RXBUF_SIZE  1024
#define HEARTBEAT_MS 1000
#define SYNC_DP     "ess/in_obs"   /* obs begin/end edge -> box->dserv clock anchor */

static uint8_t g_rxbuf[RXBUF_SIZE];
static pico_config_t  g_cfg;
static dserv_framer_t g_framer;
static box_clock_t    g_clock;      /* box->dserv time offset, snapped at obs edges */
static int32_t g_wdt;

/* ---- publish helpers (box -> dserv, best-effort) ---- */
static void publish_di(const di_event_t *e)
{
    char leaf[24], nm[64]; uint8_t f[DSERV_MSG_LEN];
    snprintf(leaf, sizeof leaf, "di/%u", e->pin);
    dserv_state_name(&g_cfg, nm, sizeof nm, leaf);
    uint8_t lvl = di_active_low(&g_cfg, e->pin) ? !e->level : e->level;   /* publish logical level */
    /* box edge time mapped into dserv time (0 => dserv arrival-stamps pre-sync) */
    dserv_msg_int(f, nm, box_clock_stamp(&g_clock, e->t_us), lvl);
    box_net_client_send(f, DSERV_MSG_LEN);
    printf("di: pin%u=%u @%lluus\n", e->pin, lvl, (unsigned long long) e->t_us);
}

/* ts = dserv-time of the actuation (box_clock_stamp of the pin-write instant). */
static void publish_do(uint8_t pin, uint8_t level, uint64_t ts)
{
    char leaf[24], nm[64]; uint8_t f[DSERV_MSG_LEN];
    snprintf(leaf, sizeof leaf, "do/%u", pin);
    dserv_state_name(&g_cfg, nm, sizeof nm, leaf);
    dserv_msg_int(f, nm, ts, level);
    box_net_client_send(f, DSERV_MSG_LEN);
}

/* Clock-align audit trail: the raw {dserv_us, box_us} anchor and the resulting
 * offset, one datapoint each, all stamped at the obs edge's dserv time. */
static void publish_sync(uint64_t dserv_us, uint64_t box_us, int64_t offset_us)
{
    char nm[64]; uint8_t f[DSERV_MSG_LEN];
    dserv_state_name(&g_cfg, nm, sizeof nm, "sync/dserv_us");
    dserv_msg_int64(f, nm, dserv_us, (int64_t) dserv_us);  box_net_client_send(f, DSERV_MSG_LEN);
    dserv_state_name(&g_cfg, nm, sizeof nm, "sync/box_us");
    dserv_msg_int64(f, nm, dserv_us, (int64_t) box_us);    box_net_client_send(f, DSERV_MSG_LEN);
    dserv_state_name(&g_cfg, nm, sizeof nm, "sync/offset_us");
    dserv_msg_int64(f, nm, dserv_us, offset_us);           box_net_client_send(f, DSERV_MSG_LEN);
}

static void publish_heartbeat(void)
{
    char nm[64]; uint8_t f[DSERV_MSG_LEN];
    dserv_state_name(&g_cfg, nm, sizeof nm, "watchdog");
    dserv_msg_int(f, nm, 0, g_wdt++);                    box_net_client_send(f, DSERV_MSG_LEN);
    dserv_state_name(&g_cfg, nm, sizeof nm, "uptime_us");
    dserv_msg_int64(f, nm, 0, (int64_t) time_us_64());   box_net_client_send(f, DSERV_MSG_LEN);
    dserv_state_name(&g_cfg, nm, sizeof nm, "link");
    dserv_msg_int(f, nm, 0, 1);                          box_net_client_send(f, DSERV_MSG_LEN);
}

/* ---- self-registration: tell dserv to push us config+cmd (box-initiated) ----
 * Sent on every (re)connect, so the box is autonomous: reset it, or restart
 * dserv, and it re-registers itself from its saved flash config. */
static void self_register(void)
{
    uint8_t bip[4]; box_net_local_ip(bip);
    uint16_t dport = g_cfg.dserv_port ? g_cfg.dserv_port : 4620;
    char pfx[64]; dserv_cfg_prefix(&g_cfg, pfx, sizeof pfx);   /* extio/<name> */
    char s[112];
    snprintf(s, sizeof s, "%%reg %u.%u.%u.%u %u 1\n", bip[0], bip[1], bip[2], bip[3], CFG_PORT);
    box_net_send_command(g_cfg.dserv_ip, dport, s);
    snprintf(s, sizeof s, "%%match %u.%u.%u.%u %u %s/config/* 1\n", bip[0], bip[1], bip[2], bip[3], CFG_PORT, pfx);
    box_net_send_command(g_cfg.dserv_ip, dport, s);
    snprintf(s, sizeof s, "%%match %u.%u.%u.%u %u %s/cmd/* 1\n", bip[0], bip[1], bip[2], bip[3], CFG_PORT, pfx);
    box_net_send_command(g_cfg.dserv_ip, dport, s);
    snprintf(s, sizeof s, "%%match %u.%u.%u.%u %u %s 1\n", bip[0], bip[1], bip[2], bip[3], CFG_PORT, SYNC_DP);
    box_net_send_command(g_cfg.dserv_ip, dport, s);   /* obs-period clock sync */
    printf("self-registered with dserv %u.%u.%u.%u:%u as %s\n",
           g_cfg.dserv_ip[0], g_cfg.dserv_ip[1], g_cfg.dserv_ip[2], g_cfg.dserv_ip[3], dport, pfx);
}

/* ---- config/cmd in: dispatch one 128B datapoint frame ---- */
static void on_frame(const uint8_t *frame, void *ud)
{
    uint64_t now_box = time_us_64();   /* receipt time, for the sync anchor */
    pico_config_t *cfg = (pico_config_t *) ud;
    dserv_msg_t m;
    if (dserv_msg_parse(frame, &m) != 0) return;

    /* obs-period sync edge: re-align box->dserv clock. Both begin(1) and end(0)
     * edges are anchors (two per obs). m.timestamp = dserv time of the toggle. */
    if (dserv_msg_name_eq(&m, SYNC_DP)) {
        int obs = (int) dserv_msg_as_long(&m);
        box_clock_sync(&g_clock, m.timestamp, now_box);
        pico_gpio_obs_mirror(cfg, obs);    /* LED/scope: box's live copy of obs */
        publish_sync(m.timestamp, now_box, g_clock.offset_us);
        printf("sync: obs=%d dserv=%llu box=%llu off=%lld\n", obs,
               (unsigned long long) m.timestamp,
               (unsigned long long) now_box, (long long) g_clock.offset_us);
        return;
    }

    gpio_cmd_t cmd;
    cfg_result_t r = dserv_dispatch(cfg, &m, &cmd);
    printf("dp: %.*s -> %s\n", (int) m.namelen, m.name, dserv_cfg_result_str(r));

    if (cmd.op != GPIO_OP_NONE) {
        pico_gpio_exec(cfg, &cmd);
        uint64_t t_act = time_us_64();   /* pin has just moved -> its dserv time */
        if (cmd.op == GPIO_OP_SET)
            publish_do(cmd.pin, (uint8_t) cmd.value, box_clock_stamp(&g_clock, t_act));  /* actual readback */
    }
    else if (r == CFG_PIN_MODE || r == CFG_OBS_PIN)  pico_gpio_apply_config(cfg);
    else if (r == CFG_SAVE)      printf("flash %s\n", flash_store_save(cfg) == 0 ? "ok" : "FAIL");
    else if (r == CFG_REBOOT)    { sleep_ms(50); watchdog_reboot(0, 0, 0); }
    else if (r == CFG_FACTORY)   { flash_store_erase(); memset(cfg, 0, sizeof *cfg);
                                   pico_gpio_apply_config(cfg); printf("factory erased\n"); }
}

/* ---- USB-CDC bootstrap/recovery CLI ---- */
static void cli_service(void)
{
    static char line[128];
    static int  len = 0;
    int ch = getchar_timeout_us(0);
    if (ch == PICO_ERROR_TIMEOUT) return;

    if (ch == '\r' || ch == '\n') {
        putchar('\n');
        line[len] = '\0'; len = 0;
        if (!strcmp(line, "ip")) {                 /* live address (DHCP lease or static) */
            uint8_t bip[4]; box_net_local_ip(bip);
            printf("ip %u.%u.%u.%u\n", bip[0], bip[1], bip[2], bip[3]);
            return;
        }
        char out[256]; gpio_cmd_t cmd;
        cli_action_t act = pico_cli_exec(&g_cfg, line, out, sizeof out, &cmd);
        fputs(out, stdout);
        if (act == CLI_GPIO)         pico_gpio_exec(&g_cfg, &cmd);   /* drive the pin (USB command) */
        else if (act == CLI_SAVE)    printf("flash %s\n", flash_store_save(&g_cfg) == 0 ? "ok" : "FAIL");
        else if (act == CLI_FACTORY) { flash_store_erase(); memset(&g_cfg, 0, sizeof g_cfg);
                                       pico_gpio_apply_config(&g_cfg); printf("erased\n"); }
        else if (act == CLI_REBOOT)  { sleep_ms(50); watchdog_reboot(0, 0, 0); }
        else if (act == CLI_PIN)     pico_gpio_apply_config(&g_cfg);  /* only re-apply on a pin change */
    } else if (ch == 0x08 || ch == 0x7f) {         /* backspace/DEL: edit the line, don't store the raw byte */
        if (len > 0) { len--; fputs("\b \b", stdout); }
    } else if (ch >= 0x20 && ch < 0x7f && len < (int) sizeof line - 1) {
        line[len++] = (char) ch;                   /* printable only -> no stray control bytes in config */
        putchar(ch);
    }
}

static int have_dserv_target(void)
{ return g_cfg.dserv_ip[0] || g_cfg.dserv_ip[1] || g_cfg.dserv_ip[2] || g_cfg.dserv_ip[3]; }

int main(void)
{
    stdio_init_all();
    sleep_ms(3000);

    memset(&g_cfg, 0, sizeof g_cfg);
    if (flash_store_load(&g_cfg) == 0) printf("config: loaded from flash (name=%s)\n", dserv_cfg_name(&g_cfg));
    else                               printf("config: none/invalid -> defaults\n");
    pico_gpio_apply_config(&g_cfg);

    if (box_net_init(&g_cfg) != 0) printf("net init FAILED\n");
    dserv_framer_reset(&g_framer);
    box_clock_reset(&g_clock);
    printf("box ready [%s]: config TCP :%d  |  USB-CDC CLI (type 'help')\n",
           box_net_backend_name(), CFG_PORT);

    uint32_t last_hb = 0;
    while (1) {
        box_net_poll();

        /* config/cmd in (dserv -> box) */
        int n = box_net_server_poll(CFG_PORT, g_rxbuf, RXBUF_SIZE);
        if (n < 0)      dserv_framer_reset(&g_framer);
        else if (n > 0) dserv_framer_feed(&g_framer, g_rxbuf, (uint32_t) n, on_frame, &g_cfg);

        cli_service();

        /* state out (box -> dserv), once a target is configured */
        if (have_dserv_target()) {
            uint16_t port = g_cfg.dserv_port ? g_cfg.dserv_port : 4620;
            int up = box_net_client_service(g_cfg.dserv_ip, port);
            if (up == 2) self_register();      /* just (re)connected -> self-register */
            if (up >= 1) {
                di_event_t e;
                while (pico_gpio_poll_di(&g_cfg, &e)) publish_di(&e);   /* debounced edges */
                uint32_t now = to_ms_since_boot(get_absolute_time());
                if (now - last_hb >= HEARTBEAT_MS) { last_hb = now; publish_heartbeat(); }
            }
        }
    }
}
