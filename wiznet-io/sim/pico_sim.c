/*
 * pico_sim.c -- a standalone "wiznet-io box" that runs on the Mac/Linux.
 *
 * It runs the SAME transport-independent code as the RP2350 firmware
 * (common/dserv_msg.h codec+framer, common/dserv_config.h dispatch); only the
 * transport differs: POSIX BSD sockets here vs. the W6300 ioLibrary on-device.
 * That lets you develop and test the whole protocol against a real dserv on
 * localhost:4620 with no board.
 *
 *   cc -O2 -Wall -I../common -o pico_sim pico_sim.c
 *
 * Modes:
 *   ./pico_sim --selftest              offline loopback build->frame->apply
 *   ./pico_sim --listen 5010           act as the config channel: dserv pushes
 *                                      pico/config keys here (binary) and apply
 *                                      -> on dserv:  %reg <thisip> 5010 1
 *                                                    dsMirrorAddMatch ... pico/config/[star]
 *   ./pico_sim --send-config <ip> <port>  inject a config set (no dserv needed)
 *   ./pico_sim --watchdog 127.0.0.1    connect to dserv:4620 and push heartbeats
 *
 * The --listen and --watchdog paths call the identical functions the firmware
 * calls; only send()/recv() wrap sockets instead of the W6300.
 */
#include "dserv_config.h"
#include "pico_persist.h"
#include "pico_cli.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static pico_config_t g_cfg;

/* --- file-backed persistence (stands in for RP2350 flash) --- */
#define CFG_FILE "/tmp/pico_cfg.bin"

static void persist_save(void)
{
    uint8_t blob[PICO_PERSIST_BLOB_MAX];
    uint32_t n = pico_persist_serialize(&g_cfg, blob, sizeof blob);
    FILE *f = fopen(CFG_FILE, "wb");
    if (f) { fwrite(blob, 1, n, f); fclose(f); printf("  [persist] wrote %u bytes to %s\n", n, CFG_FILE); }
    else     printf("  [persist] save failed\n");
}

static void persist_load(void)
{
    uint8_t blob[PICO_PERSIST_BLOB_MAX];
    FILE *f = fopen(CFG_FILE, "rb");
    if (f) {
        uint32_t n = (uint32_t) fread(blob, 1, sizeof blob, f); fclose(f);
        if (pico_persist_deserialize(blob, n, &g_cfg) == 0) {
            printf("  [persist] loaded config from %s\n", CFG_FILE); return;
        }
        printf("  [persist] %s invalid -> defaults\n", CFG_FILE);
    } else
        printf("  [persist] no saved config -> defaults\n");
    memset(&g_cfg, 0, sizeof g_cfg);
}

static void dump_cfg(void)
{
    printf("  [cfg] applied=%u  dserv=%u.%u.%u.%u:%u  net_ip=%u.%u.%u.%u\n",
           g_cfg.applied_count,
           g_cfg.dserv_ip[0], g_cfg.dserv_ip[1], g_cfg.dserv_ip[2], g_cfg.dserv_ip[3],
           g_cfg.dserv_port,
           g_cfg.net_ip[0], g_cfg.net_ip[1], g_cfg.net_ip[2], g_cfg.net_ip[3]);
}

/* print-only stand-in for the device GPIO layer (pico/pico_gpio.h) */
static void sim_gpio_exec(const gpio_cmd_t *cmd)
{
    if (cmd->op == GPIO_OP_SET)        printf("  [gpio] pin%u <- %u\n", cmd->pin, cmd->value);
    else if (cmd->op == GPIO_OP_PULSE) printf("  [gpio] pin%u pulse %uus\n", cmd->pin, cmd->value);
}

/* framer callback: dispatch one 128B frame (same routing as firmware) */
static void on_frame(const uint8_t *frame, void *ud)
{
    (void) ud;
    dserv_msg_t m;
    if (dserv_msg_parse(frame, &m) != 0) { printf("  bad frame\n"); return; }
    gpio_cmd_t cmd;
    cfg_result_t r = dserv_dispatch(&g_cfg, &m, &cmd);
    char nm[128]; memcpy(nm, m.name, m.namelen); nm[m.namelen] = 0;
    printf("  rx %-28s type=%2u len=%u -> %s\n", nm, m.type, m.datalen,
           dserv_cfg_result_str(r));
    if (cmd.op != GPIO_OP_NONE)   sim_gpio_exec(&cmd);
    else if (r == CFG_SAVE)     { printf("  [save] persisting config\n"); persist_save(); dump_cfg(); }
}

static int run_selftest(void)
{
    uint8_t f[DSERV_MSG_LEN];
    dserv_framer_t fr; dserv_framer_reset(&fr);
    memset(&g_cfg, 0, sizeof g_cfg);
    printf("selftest: build -> frame -> apply (no network)\n");

    dserv_msg_int   (f, "pico/config/pin/5/mode", 0, 1);      dserv_framer_feed(&fr, f, DSERV_MSG_LEN, on_frame, 0);
    dserv_msg_int   (f, "pico/config/pin/5/pulse_us", 0, 250);dserv_framer_feed(&fr, f, DSERV_MSG_LEN, on_frame, 0);
    dserv_msg_string(f, "pico/config/dserv/ip", 0, "192.168.11.1"); dserv_framer_feed(&fr, f, DSERV_MSG_LEN, on_frame, 0);
    dserv_msg_int   (f, "pico/config/dserv/port", 0, 4620);   dserv_framer_feed(&fr, f, DSERV_MSG_LEN, on_frame, 0);
    dserv_msg_int   (f, "pico/cmd/save", 0, 1);            dserv_framer_feed(&fr, f, DSERV_MSG_LEN, on_frame, 0);

    int ok = g_cfg.pin_mode[5] == 1 && g_cfg.do_pulse_us[5] == 250
          && g_cfg.dserv_ip[3] == 1 && g_cfg.dserv_port == 4620
          && g_cfg.applied_count == 4;
    printf(ok ? "SELFTEST PASS\n" : "SELFTEST FAIL\n");
    return ok ? 0 : 1;
}

static int run_listen(int port)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(port);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) { perror("bind/listen"); return 1; }

    printf("config channel listening on :%d\n", port);
    printf("on dserv:  %%reg <this-host-ip> %d 1   then a match on pico/config/*\n", port);

    persist_load();                 /* boot with saved config, as the box would */
    dserv_framer_t fr; dserv_framer_reset(&fr);

    for (;;) {
        struct sockaddr_in ca; socklen_t cl = sizeof ca;
        int cs = accept(ls, (struct sockaddr *)&ca, &cl);
        if (cs < 0) { perror("accept"); break; }
        printf("dserv connected from %s\n", inet_ntoa(ca.sin_addr));
        dserv_framer_reset(&fr);
        uint8_t chunk[512];
        ssize_t n;
        while ((n = recv(cs, chunk, sizeof chunk, 0)) > 0)
            dserv_framer_feed(&fr, chunk, (uint32_t) n, on_frame, 0);
        printf("dserv disconnected\n");
        close(cs);
    }
    close(ls);
    return 0;
}

static int run_watchdog(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in d = {0};
    d.sin_family = AF_INET; d.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &d.sin_addr) != 1) { fprintf(stderr, "bad host\n"); return 1; }
    if (connect(fd, (struct sockaddr *)&d, sizeof d) != 0) { perror("connect"); return 1; }
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    printf("watchdog -> dserv %s:%d\n", host, port);

    uint8_t f[DSERV_MSG_LEN];
    for (int i = 0;; i++) {
        dserv_msg_int(f, "pico/state/watchdog", 0, i);
        if (write(fd, f, DSERV_MSG_LEN) != DSERV_MSG_LEN) { perror("write"); break; }
        printf("  watchdog %d\n", i);
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 }; nanosleep(&ts, NULL);
    }
    close(fd);
    return 0;
}

/* Inject a config set over a real socket, then close. Stands in for dserv's
 * binary relay so you can test the --listen path without dserv. */
static int run_send_config(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in d = {0};
    d.sin_family = AF_INET; d.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &d.sin_addr) != 1) { fprintf(stderr, "bad host\n"); return 1; }
    if (connect(fd, (struct sockaddr *)&d, sizeof d) != 0) { perror("connect"); return 1; }

    uint8_t f[DSERV_MSG_LEN];
    dserv_msg_int   (f, "pico/config/pin/6/mode", 0, 3);      write(fd, f, DSERV_MSG_LEN);
    dserv_msg_int   (f, "pico/config/pin/6/pulse_us", 0, 750);write(fd, f, DSERV_MSG_LEN);
    dserv_msg_string(f, "pico/config/dserv/ip", 0, "10.0.0.5"); write(fd, f, DSERV_MSG_LEN);
    dserv_msg_int   (f, "pico/config/dserv/port", 0, 4620);   write(fd, f, DSERV_MSG_LEN);
    dserv_msg_int   (f, "pico/cmd/save", 0, 1);            write(fd, f, DSERV_MSG_LEN);
    printf("sent 5 config frames to %s:%d\n", host, port);
    close(fd);
    return 0;
}

/* USB-CDC-style bootstrap/recovery REPL over stdin. On the device this reads
 * chars from USB-CDC; here it reads stdin. Same pico_cli_exec + persistence. */
static int run_cli(void)
{
    persist_load();
    printf("pico box CLI (bootstrap/recovery). type 'help'. ctrl-D to exit.\n");
    char line[128], out[256]; gpio_cmd_t cmd;
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        cli_action_t act = pico_cli_exec(&g_cfg, line, out, sizeof out, &cmd);
        fputs(out, stdout);
        if (act == CLI_GPIO)    sim_gpio_exec(&cmd);
        else if (act == CLI_SAVE) persist_save();
        else if (act == CLI_FACTORY) { memset(&g_cfg, 0, sizeof g_cfg); remove(CFG_FILE);
                                       printf("  [factory] storage erased\n"); }
        else if (act == CLI_REBOOT)  { printf("  [reboot] (sim exits)\n"); break; }
    }
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);   /* line-buffered so redirected logs flush */
    if (argc >= 2 && !strcmp(argv[1], "--selftest")) return run_selftest();
    if (argc >= 2 && !strcmp(argv[1], "--cli"))      return run_cli();
    if (argc >= 3 && !strcmp(argv[1], "--listen"))   return run_listen(atoi(argv[2]));
    if (argc >= 4 && !strcmp(argv[1], "--send-config"))
        return run_send_config(argv[2], atoi(argv[3]));
    if (argc >= 3 && !strcmp(argv[1], "--watchdog"))
        return run_watchdog(argv[2], argc >= 4 ? atoi(argv[3]) : 4620);

    fprintf(stderr,
        "usage:\n"
        "  %s --selftest\n"
        "  %s --cli                     (USB-CDC-style bootstrap/recovery REPL + persist)\n"
        "  %s --listen <port>           (config channel; dserv pushes pico/config/*)\n"
        "  %s --send-config <ip> <port> (inject config w/o dserv)\n"
        "  %s --watchdog <host> [port]  (push heartbeats to dserv:4620)\n",
        argv[0], argv[0], argv[0], argv[0], argv[0]);
    return 2;
}
