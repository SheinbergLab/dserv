/*
 * wizchip_dserv_config.c -- the "box" firmware. Transport-agnostic (box_net.h):
 * builds for wired W6300 (default) or Pico 2 W WiFi (-DBOX_NET_LWIP).
 *
 * Namespace (device name = configurable prefix, e.g. devpico):
 *   <name>/config/(keys)   host->box persistent settings (pushed by dserv; saved to flash)
 *   <name>/cmd/(keys)      host->box transient actions   (pushed by dserv)
 *                       cmd/do/<n>=0|1, cmd/do/<n>/pulse_us, cmd/save|reboot|factory
 *   <name>/state/(keys)    box->dserv published status    (box connects to dserv as client)
 *                       state/di/<n> (edge-timestamped), state/do/<n> readback,
 *                       state/watchdog, state/uptime_us, state/link
 *
 * dserv relay setup:  %reg <box_ip> <CFG_PORT> 1 ; %match <name>/config keys ; %match <name>/cmd keys
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
#include "box_net.h"

#define CFG_PORT    5010
#define RXBUF_SIZE  1024
#define HEARTBEAT_MS 1000

static uint8_t g_rxbuf[RXBUF_SIZE];
static pico_config_t  g_cfg;
static dserv_framer_t g_framer;
static int32_t g_wdt;

/* ---- publish helpers (box -> dserv, best-effort) ---- */
static void publish_di(const di_event_t *e)
{
    char leaf[24], nm[64]; uint8_t f[DSERV_MSG_LEN];
    snprintf(leaf, sizeof leaf, "di/%u", e->pin);
    dserv_state_name(&g_cfg, nm, sizeof nm, leaf);
    dserv_msg_int(f, nm, e->t_us, e->level);      /* timestamp = box edge time */
    box_net_client_send(f, DSERV_MSG_LEN);
    printf("di: pin%u=%u @%lluus\n", e->pin, e->level, (unsigned long long) e->t_us);
}

static void publish_do(uint8_t pin, uint8_t level)
{
    char leaf[24], nm[64]; uint8_t f[DSERV_MSG_LEN];
    snprintf(leaf, sizeof leaf, "do/%u", pin);
    dserv_state_name(&g_cfg, nm, sizeof nm, leaf);
    dserv_msg_int(f, nm, 0, level);
    box_net_client_send(f, DSERV_MSG_LEN);
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
    const char *nm = dserv_cfg_name(&g_cfg);
    char s[96];
    snprintf(s, sizeof s, "%%reg %u.%u.%u.%u %u 1\n", bip[0], bip[1], bip[2], bip[3], CFG_PORT);
    box_net_send_command(g_cfg.dserv_ip, dport, s);
    snprintf(s, sizeof s, "%%match %u.%u.%u.%u %u %s/config/* 1\n", bip[0], bip[1], bip[2], bip[3], CFG_PORT, nm);
    box_net_send_command(g_cfg.dserv_ip, dport, s);
    snprintf(s, sizeof s, "%%match %u.%u.%u.%u %u %s/cmd/* 1\n", bip[0], bip[1], bip[2], bip[3], CFG_PORT, nm);
    box_net_send_command(g_cfg.dserv_ip, dport, s);
    printf("self-registered with dserv %u.%u.%u.%u:%u as %s\n",
           g_cfg.dserv_ip[0], g_cfg.dserv_ip[1], g_cfg.dserv_ip[2], g_cfg.dserv_ip[3], dport, nm);
}

/* ---- config/cmd in: dispatch one 128B datapoint frame ---- */
static void on_frame(const uint8_t *frame, void *ud)
{
    pico_config_t *cfg = (pico_config_t *) ud;
    dserv_msg_t m;
    if (dserv_msg_parse(frame, &m) != 0) return;
    gpio_cmd_t cmd;
    cfg_result_t r = dserv_dispatch(cfg, &m, &cmd);
    printf("dp: %.*s -> %s\n", (int) m.namelen, m.name, dserv_cfg_result_str(r));

    if (cmd.op != GPIO_OP_NONE) {
        pico_gpio_exec(cfg, &cmd);
        if (cmd.op == GPIO_OP_SET) publish_do(cmd.pin, (uint8_t) cmd.value);  /* actual readback */
    }
    else if (r == CFG_PIN_MODE)  pico_gpio_apply_config(cfg);
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
        char out[256]; gpio_cmd_t cmd;
        cli_action_t act = pico_cli_exec(&g_cfg, line, out, sizeof out, &cmd);
        fputs(out, stdout);
        if (act == CLI_GPIO)         pico_gpio_exec(&g_cfg, &cmd);   /* drive the pin (USB command) */
        else if (act == CLI_SAVE)    printf("flash %s\n", flash_store_save(&g_cfg) == 0 ? "ok" : "FAIL");
        else if (act == CLI_FACTORY) { flash_store_erase(); memset(&g_cfg, 0, sizeof g_cfg);
                                       pico_gpio_apply_config(&g_cfg); printf("erased\n"); }
        else if (act == CLI_REBOOT)  { sleep_ms(50); watchdog_reboot(0, 0, 0); }
        else if (act == CLI_PIN)     pico_gpio_apply_config(&g_cfg);  /* only re-apply on a pin change */
    } else if (len < (int) sizeof line - 1) {
        line[len++] = (char) ch;
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
