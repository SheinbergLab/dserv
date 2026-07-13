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

static inline int box_net_phy_link(void) { return -2; }   /* no PHY on the USB transport */

/* Config path liveness == enumerated: the host module reads/writes the data CDC
 * whenever the device is mounted; there is no separate dserv connect-back. */
static inline int box_net_server_up(void) { return tud_ready() ? 1 : 0; }

/* Host is actually DRAINING the data CDC: usbio open()s the tty (asserting DTR)
 * only on its ~2s discovery poll -- WELL after we enumerate -- so tud_ready
 * (enumerated) is true long before anyone reads. tud_cdc_n_connected reflects
 * DTR on the data CDC = the host has the port open. Used to hold the one-shot
 * connect burst until it will actually land (di/heartbeat stay ungated: they
 * repeat, so a few early drops self-heal, but the burst fires once). DTR is a
 * proven signal here -- box_console uses it on CDC0 for terminal-attach. */
static inline int box_net_client_reading(void)
{ return tud_ready() && tud_cdc_n_connected(BOX_USB_CDC_DATA); }

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

/* TX accounting (console `txstats`): ok / had-to-wait / dropped / not-ready.
 * Defined once in the app TU (wizchip_dserv_config.c) so every build links. */
extern volatile uint32_t box_usb_tx_ok, box_usb_tx_wait, box_usb_tx_drop, box_usb_tx_notready;

/* ---- publish one 128-byte datapoint frame to the host module ----
 * Best-effort, BOUNDED, and ATOMIC: the frame is written only once the CDC
 * FIFO has room for ALL of it -- the old chunked loop could give up mid-frame,
 * leaving a PARTIAL frame on the wire that desynced the host framer (seen
 * live 2026-07-11 as lost DI edges + host resync skips). Waiting pumps
 * tud_task so a draining host frees space; a stalled host costs a bounded
 * spin and one whole dropped frame. 0 ok, <0 not connected / host stalled. */
static inline int box_net_client_send(const uint8_t *buf, int len)
{
    if (!tud_ready()) { box_usb_tx_notready++; return -1; }
    int guard = 0;
    while (tud_cdc_n_write_available(BOX_USB_CDC_DATA) < (uint32_t) len) {
        tud_task();                         /* FIFO full -- let it drain to the host */
        if (++guard > 2000) { box_usb_tx_drop++; return -2; }  /* whole frame, never partial */
    }
    if (guard) box_usb_tx_wait++;
    tud_cdc_n_write(BOX_USB_CDC_DATA, buf, (uint32_t) len);
    tud_cdc_n_write_flush(BOX_USB_CDC_DATA);
    box_usb_tx_ok++;
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

/* Async flavor: the CDC write completes (or best-effort drops) immediately, so
 * start does the work and poll reports instant success -- the periodic USB
 * re-registration makes any drop self-healing. */
static inline int box_net_send_command_start(const uint8_t dserv_ip[4], uint16_t port, const char *cmd)
{ return box_net_send_command(dserv_ip, port, cmd); }
static inline int box_net_send_command_poll(void) { return 1; }

/* No raw binary pull over USB: the Stage-0 OTA fetch is a transient dserv socket
 * ('<' get), which only the W6300 backend has. The USB path is instead the
 * chunk-push model (cmd/ota/chunk over the CDC frame channel; see OTA.md). Stub
 * to -1 so pico_ota reports "not supported on this transport" cleanly. */
static inline int box_net_get_binary(const uint8_t dserv_ip[4], uint16_t port,
                                     const char *key, box_net_bin_sink sink, void *ud)
{ (void) dserv_ip; (void) port; (void) key; (void) sink; (void) ud; return -1; }

/* The CDC0 console (stdio driver + drain loops) that used to live here moved to
 * box_console.h: printf now lands in a per-core lock-free ring drained by core 0,
 * and core 1's box_console_cdc0_ferry() moves console bytes to/from CDC0
 * fire-and-forget -- same DTR gating, no drain loops on any hot path. */

#endif /* BOX_NET_USB_H */
