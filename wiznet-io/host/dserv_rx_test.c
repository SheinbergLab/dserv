/*
 * dserv_rx_test.c -- exercise the RX path exactly as the box will run it:
 *   build 128B frames (TX codec) -> concatenate into a byte stream ->
 *   feed through dserv_framer in awkward chunk splits -> parse -> config_apply.
 * Verifies framing reassembly across chunk boundaries + config dispatch.
 *
 *   cc -O2 -Wall -I../common -o dserv_rx_test dserv_rx_test.c && ./dserv_rx_test
 */
#include "dserv_config.h"
#include <stdio.h>
#include <string.h>

static pico_config_t g_cfg;
static int g_frames, g_fails;
static gpio_cmd_t g_last_gpio; static int g_gpio_sets, g_gpio_pulses;

static void on_frame(const uint8_t *frame, void *ud)
{
    pico_config_t *cfg = (pico_config_t *) ud;
    dserv_msg_t m;
    if (dserv_msg_parse(frame, &m) != 0) { printf("  parse FAIL\n"); g_fails++; return; }
    gpio_cmd_t cmd;
    cfg_result_t r = dserv_dispatch(cfg, &m, &cmd);
    char nm[128]; memcpy(nm, m.name, m.namelen); nm[m.namelen] = 0;
    printf("  frame %-26s type=%2u len=%u -> %s\n", nm, m.type, m.datalen,
           dserv_cfg_result_str(r));
    if (cmd.op == GPIO_OP_SET)   { g_last_gpio = cmd; g_gpio_sets++; }
    if (cmd.op == GPIO_OP_PULSE) { g_last_gpio = cmd; g_gpio_pulses++; }
    g_frames++;
}

int main(void)
{
    /* Build a stream of config datapoints (as dserv would relay them). */
    uint8_t stream[14 * DSERV_MSG_LEN];
    int off = 0;
    off += (dserv_msg_int   (stream + off, "pico/config/pin/5/mode", 0, 1), DSERV_MSG_LEN);
    off += (dserv_msg_int   (stream + off, "pico/config/pin/6/mode", 0, 3), DSERV_MSG_LEN);
    off += (dserv_msg_string(stream + off, "pico/config/pin/7/mode", 0, "out"), DSERV_MSG_LEN); /* word */
    off += (dserv_msg_int   (stream + off, "pico/config/pin/5/pulse_us", 0, 500), DSERV_MSG_LEN);
    off += (dserv_msg_int   (stream + off, "pico/config/pin/6/debounce_ms", 0, 20), DSERV_MSG_LEN);
    off += (dserv_msg_string(stream + off, "pico/config/dserv/ip", 0, "192.168.11.1"), DSERV_MSG_LEN);
    off += (dserv_msg_int   (stream + off, "pico/config/dserv/port", 0, 4620), DSERV_MSG_LEN);
    off += (dserv_msg_string(stream + off, "pico/config/net/ip", 0, "192.168.11.42"), DSERV_MSG_LEN);
    off += (dserv_msg_int   (stream + off, "pico/watchdog", 0, 7), DSERV_MSG_LEN);   /* not cmd/config */
    off += (dserv_msg_int   (stream + off, "pico/cmd/do/5", 0, 1), DSERV_MSG_LEN);         /* do set */
    off += (dserv_msg_int   (stream + off, "pico/cmd/do/6/pulse_us", 0, 250), DSERV_MSG_LEN); /* pulse */
    off += (dserv_msg_int   (stream + off, "pico/cmd/save", 0, 1), DSERV_MSG_LEN);

    printf("RX path: %d frames, fed in 37-byte chunks (crosses frame boundaries)\n",
           off / DSERV_MSG_LEN);

    dserv_framer_t fr; dserv_framer_reset(&fr);
    memset(&g_cfg, 0, sizeof g_cfg);
    for (int i = 0; i < off; i += 37)                 /* deliberately not 128 */
        dserv_framer_feed(&fr, stream + i, (i + 37 <= off) ? 37 : (off - i),
                          on_frame, &g_cfg);

    /* Verify resulting config */
    printf("\nresulting config:\n");
    printf("  pin5 mode=%u pulse_us=%u\n", g_cfg.pin_mode[5], g_cfg.do_pulse_us[5]);
    printf("  pin6 mode=%u\n", g_cfg.pin_mode[6]);
    printf("  net_ip=%u.%u.%u.%u\n", g_cfg.net_ip[0], g_cfg.net_ip[1], g_cfg.net_ip[2], g_cfg.net_ip[3]);
    printf("  dserv=%u.%u.%u.%u:%u\n", g_cfg.dserv_ip[0], g_cfg.dserv_ip[1], g_cfg.dserv_ip[2], g_cfg.dserv_ip[3], g_cfg.dserv_port);
    printf("  applied_count=%u\n", g_cfg.applied_count);

    printf("  gpio: sets=%d pulses=%d last: op=%d pin=%u val=%u\n",
           g_gpio_sets, g_gpio_pulses, g_last_gpio.op, g_last_gpio.pin, g_last_gpio.value);

    int ok = g_frames == 12 && !g_fails
          && g_cfg.pin_mode[5] == 1 && g_cfg.do_pulse_us[5] == 500
          && g_cfg.pin_mode[6] == 3 && g_cfg.debounce_ms[6] == 20
          && g_cfg.pin_mode[7] == 1     /* word "out" -> 1 via datapoint */
          && g_cfg.net_ip[0] == 192 && g_cfg.net_ip[3] == 42
          && g_cfg.dserv_ip[3] == 1 && g_cfg.dserv_port == 4620
          && g_cfg.applied_count == 8   /* 8 config sets; watchdog/gpio/save don't count */
          && g_gpio_sets == 1 && g_gpio_pulses == 1
          && g_last_gpio.op == GPIO_OP_PULSE && g_last_gpio.pin == 6 && g_last_gpio.value == 250;

    printf(ok ? "\nALL PASS\n" : "\nFAILED\n");
    return ok ? 0 : 1;
}
