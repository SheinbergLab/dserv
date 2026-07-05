/*
 * box_net_usb.h -- USB-CDC transport backend for the box (plain Pico 2, no NIC).
 *
 * Implements the box_net.h seam over a TinyUSB CDC "data" interface instead of a
 * socket. The box speaks the SAME 128-byte dserv frames; a host-side dserv module
 * (modules/usbio) reads them off /dev/ttyACM* and injects them with
 * tclserver_set_point, and forwards ess/in_obs plus the config and cmd keys back. So
 * everything above the seam (dserv_msg / dserv_config / pico_persist / pico_gpio /
 * pico_clock) is byte-for-byte unchanged -- this is a pure transport swap, selected
 * with -DBOX_NET_USB.
 *
 * COEXISTENCE: opt-in build flag -> one distinct image (a plain Pico 2). It does
 * NOT touch the W6300 or lwIP builds; box_net.h merely adds an #elif branch. USB is
 * a per-image transport like the others, not a runtime fallback.
 *
 * REGISTRATION MODEL: over USB there is no dserv IP to %reg with, so the HOST module
 * owns subscription. box_net_send_command() is a no-op by default (v1: the module is
 * statically told what to forward). Define BOX_USB_FORWARD_REGISTER to instead emit
 * the box's %match lines down the CDC ('%'-prefixed, so the module's '>'-peeking
 * framer routes them to its registration parser) for plug-and-play autonomy (v2).
 *
 * BUILD REQUIREMENTS (supplied by the target, not here):
 *   - tusb_config.h:      CFG_TUD_CDC >= 1  (2 for CLI + data),
 *                         CFG_TUD_CDC_TX_BUFSIZE >= 128, CFG_TUD_CDC_RX_BUFSIZE >= 128
 *   - usb_descriptors.c:  a composite descriptor exposing the data CDC at
 *                         BOX_USB_CDC_DATA (default 1; CDC0 kept for the stdio/CLI
 *                         console). QUICK BRING-UP: build data-only with
 *                         -DBOX_USB_CDC_DATA=0 and the pico stdio-usb CLI disabled.
 *   - main loop:          unchanged -- it already calls box_net_init / _poll /
 *                         _client_service / _client_send / _server_poll per the seam.
 */
#ifndef BOX_NET_USB_H
#define BOX_NET_USB_H

#include "dserv_config.h"
#include "tusb.h"
#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "pico/error.h"
#include <stdint.h>
#include <string.h>

#ifndef BOX_USB_CDC_DATA
#define BOX_USB_CDC_DATA 1        /* CDC interface carrying binary data (CLI on 0) */
#endif

/* ---- init / per-loop service ---- */

static inline int box_net_init(const pico_config_t *cfg)
{
    (void) cfg;                            /* no IP/link config over USB */
    if (!tusb_inited()) tusb_init();       /* harmless if the CLI already brought it up */
    return 0;
}

static inline void box_net_poll(void)
{
    tud_task();                            /* service TinyUSB every superloop */
}

static inline const char *box_net_backend_name(void) { return "usb"; }

/* ---- "link to dserv": USB enumerated (mounted, not suspended) is our "connected". ----
 * We deliberately do NOT gate on DTR (tud_cdc_n_connected): not every host/opener
 * asserts it, and telemetry that silently withholds until DTR is a debugging trap.
 * Once enumerated we transmit; if no host is reading, the CDC FIFO fills and frames
 * best-effort drop (box_net_client_send). Returns 2 the tick it (re)connects
 * (-> caller runs self_register), 1 while up, 0 otherwise. */
static inline int box_net_client_service(const uint8_t dserv_ip[4], uint16_t port)
{
    (void) dserv_ip; (void) port;
    static uint8_t prev = 0;
    uint8_t now = tud_ready() ? 1 : 0;
    int r = (now && !prev) ? 2 : (now ? 1 : 0);
    prev = now;
    return r;
}

/* ---- inbound: config/cmd/ess-in_obs frames the module forwarded down the CDC ----
 * Feed the return into the same dserv_msg framer the socket backends use.
 *   >0            bytes read
 *    0            nothing this tick
 *    BOX_NET_RESET a fresh connect -- reset the framer (partial frame is stale). */
static inline int box_net_server_poll(uint16_t port, uint8_t *buf, int max)
{
    (void) port;
    static uint8_t prev = 0;
    uint8_t now = tud_ready() ? 1 : 0;
    if (now && !prev) { prev = now; return BOX_NET_RESET; }
    prev = now;
    if (!now || max <= 0) return 0;
    if (!tud_cdc_n_available(BOX_USB_CDC_DATA)) return 0;
    return (int) tud_cdc_n_read(BOX_USB_CDC_DATA, buf, (uint32_t) max);
}

/* ---- publish one 128-byte datapoint frame to the host module ----
 * Best-effort and BOUNDED: writes the whole frame (a partial frame would desync the
 * host framer), draining the CDC FIFO between chunks; gives up (<0) if the host is
 * not reading, so the superloop never stalls. 0 ok, <0 not connected / host stalled. */
static inline int box_net_client_send(const uint8_t *buf, int len)
{
    if (!tud_ready()) return -1;
    int sent = 0, guard = 0;
    while (sent < len) {
        uint32_t w = tud_cdc_n_write(BOX_USB_CDC_DATA, buf + sent, (uint32_t)(len - sent));
        sent += (int) w;
        if (w == 0) {                       /* FIFO full -- let it drain to the host */
            tud_task();
            if (++guard > 2000) return -2;  /* host not draining -> drop, stay alive */
        }
    }
    tud_cdc_n_write_flush(BOX_USB_CDC_DATA);
    return 0;
}

/* No IP over USB (only used to advertise in %reg, which we don't do here). */
static inline void box_net_local_ip(uint8_t out[4]) { out[0] = out[1] = out[2] = out[3] = 0; }

/* ---- self_register()'s %reg/%match text ----
 * Over USB there is no dserv socket to register with; the host module owns
 * forwarding. v1: no-op (module statically configured). v2: define
 * BOX_USB_FORWARD_REGISTER to emit the line down the data CDC -- '%'-prefixed, so the
 * module's byte-0 peek routes it to its registration parser and auto-wires forwards. */
static inline int box_net_send_command(const uint8_t dserv_ip[4], uint16_t port, const char *cmd)
{
    (void) dserv_ip; (void) port;
#ifdef BOX_USB_FORWARD_REGISTER
    if (cmd && tud_ready()) {
        tud_cdc_n_write(BOX_USB_CDC_DATA, (const uint8_t *) cmd, (uint32_t) strlen(cmd));
        tud_cdc_n_write_flush(BOX_USB_CDC_DATA);
    }
#else
    (void) cmd;
#endif
    return 0;
}

/* ---- USB console on CDC0 ----------------------------------------------------
 * Route stdio (printf + the stdio-based CLI) to CDC0 so the USB box has a console
 * just like the W6300 / pico2w builds -- while binary data stays on CDC1. Call
 * box_net_usb_console_init() once from main() after box_net_init(). On the host
 * you then get two ports: the lower (CDC0) is the console, the higher (CDC1) is
 * data. Independent interfaces, so the console never disturbs the data channel. */
static void box_usb_con_out(const char *buf, int len)
{
    if (!tud_ready()) return;                     /* pre-enum: drop (host isn't there yet) */
    tud_cdc_n_write(0, (const uint8_t *) buf, (uint32_t) len);
    tud_cdc_n_write_flush(0);
}
static void box_usb_con_flush(void) { tud_cdc_n_write_flush(0); }
static int box_usb_con_in(char *buf, int len)
{
    if (!tud_cdc_n_available(0)) return PICO_ERROR_NO_DATA;
    return (int) tud_cdc_n_read(0, (uint8_t *) buf, (uint32_t) len);
}
static stdio_driver_t box_usb_con_driver = {
    .out_chars    = box_usb_con_out,
    .out_flush    = box_usb_con_flush,
    .in_chars     = box_usb_con_in,
    .crlf_enabled = true,                         /* \n -> \r\n for terminals */
};
static inline void box_net_usb_console_init(void)
{
    stdio_set_driver_enabled(&box_usb_con_driver, true);
}

#endif /* BOX_NET_USB_H */
