/*
 * box_net_ble.h -- BLE-peripheral transport backend for the HANDHELD build
 * (-DBOX_NET_BLE; BLE.md). The box speaks the same 128-byte dserv frames; the
 * far end is not a socket or a host CDC but the RECEIVER box's BLE central,
 * which relays frames onto its own USB transport. Everything above the seam
 * is byte-for-byte unchanged -- the handheld is just an extio box whose
 * transport is a radio.
 *
 * SPLIT-CORE SHAPE (the one real difference from the other backends): the
 * radio stack lives on CORE 0 (cyw43 poll arch is single-core, and the house
 * rule keeps radios off the RT core -- box_ble_periph.h), while this shim is
 * what CORE 1's rt_main calls through the box_net seam. The two sides meet in
 * a pair of pico queues carrying whole frames (the g_cmd_q/g_ain_q pattern):
 *
 *   client_send (core 1)  -> box_ble_txq -> core 0 pumps ATT notifications
 *   ATT writes (core 0)   -> box_ble_rxq -> server_poll (core 1) -> framer
 *
 * "Connected to dserv" here means: BLE central connected + subscribed to the
 *  TX characteristic + ATT_MTU >= 131 (whole-frame PDUs). box_ble_periph.h
 * maintains that as the volatile box_ble_link byte; this shim edge-detects it
 * for the up==2 connect burst, exactly like USB's tud_ready edge.
 *
 * TinyUSB still runs -- as the CONSOLE only (CDC0 via box_console's ferry;
 * BOX_CONSOLE_TUSB). The data CDC exists but carries nothing; on a shared
 * bench host beware extio_find_data_port grabbing it (see BLE.md).
 *
 * No %reg/%match over this transport: the receiver relays frames, the host's
 * usbio/extioconf owns forwarding (same model as USB v1). send_command is a
 * no-op; OTA 'D'-push and socket pull are unsupported (update when docked).
 */
#ifndef BOX_NET_BLE_H
#define BOX_NET_BLE_H

#include "dserv_config.h"
#include "dserv_msg.h"
#include "pico/util/queue.h"
#include "tusb.h"
#include <stdint.h>
#include <string.h>

/* ---- the cross-core frame queues + link state (single-writer each) ----
 * Defined here (box_net.h includes us early) so box_ble_periph.h -- included
 * later in the TU -- sees them. Init on core 0 BEFORE core 1 launches. */
#define BOX_BLE_TXQ_DEPTH 40      /* core1 -> radio: covers a full announce burst (~25 frames) */
#define BOX_BLE_RXQ_DEPTH 8       /* radio -> core1: config/cmd trickle */
static queue_t box_ble_txq, box_ble_rxq;
static volatile uint8_t box_ble_link;          /* 1 = connected + subscribed + MTU ok (core-0 owned) */
static volatile uint32_t box_ble_tx_ok, box_ble_tx_drop, box_ble_tx_nolink;

static inline void box_net_ble_queues_init(void)
{
    queue_init(&box_ble_txq, DSERV_MSG_LEN, BOX_BLE_TXQ_DEPTH);
    queue_init(&box_ble_rxq, DSERV_MSG_LEN, BOX_BLE_RXQ_DEPTH);
}

/* ---- the box_net seam (all called from core 1) ---- */

static inline int box_net_init(const pico_config_t *cfg)
{
    (void) cfg;
    if (!tusb_inited()) tusb_init();   /* console CDCs; the radio itself is core 0's job */
    return 0;
}

static inline void box_net_poll(void) { tud_task(); }

static inline const char *box_net_backend_name(void) { return "ble"; }
static inline int box_net_phy_link(void) { return -2; }        /* no PHY here */
static inline int box_net_server_up(void) { return box_ble_link ? 1 : 0; }
static inline int box_net_client_reading(void) { return box_ble_link ? 1 : 0; }  /* subscribed = far end drains */

static inline int box_net_client_service(const uint8_t dserv_ip[4], uint16_t port)
{
    (void) dserv_ip; (void) port;
    static uint8_t prev;
    uint8_t now = box_ble_link ? 1 : 0;
    int r = (now && !prev) ? 2 : (now ? 1 : 0);   /* 2 = link just came up -> announce burst */
    prev = now;
    return r;
}

/* Inbound frames the receiver wrote to our RX characteristic. Whole frames
 * only (the radio side enforces 128B), so one frame per call keeps the
 * framer's resync trivial. Link edge -> BOX_NET_RESET (stale partials). */
static inline int box_net_server_poll(uint16_t port, uint8_t *buf, int max)
{
    (void) port;
    static uint8_t prev;
    uint8_t now = box_ble_link ? 1 : 0;
    if (now && !prev) { prev = now; return BOX_NET_RESET; }
    prev = now;
    if (max < DSERV_MSG_LEN) return 0;
    if (!queue_try_remove(&box_ble_rxq, buf)) return 0;
    return DSERV_MSG_LEN;
}

/* Publish one frame: queue it for core 0's notification pump. Whole frames,
 * never partial; a full queue drops the frame (best-effort, like a full CDC
 * FIFO) -- heartbeats repeat, so drops self-heal. */
static inline int box_net_client_send(const uint8_t *buf, int len)
{
    if (len != DSERV_MSG_LEN) return -1;
    if (!box_ble_link) { box_ble_tx_nolink++; return -1; }
    if (!queue_try_add(&box_ble_txq, buf)) { box_ble_tx_drop++; return -2; }
    box_ble_tx_ok++;
    return 0;
}

static inline void box_net_local_ip(uint8_t out[4]) { out[0] = out[1] = out[2] = out[3] = 0; }

/* No registration over the radio: the host module owns forwarding (USB v1
 * model); the receiver just relays name-addressed frames. */
static inline int box_net_send_command(const uint8_t dserv_ip[4], uint16_t port, const char *cmd)
{ (void) dserv_ip; (void) port; (void) cmd; return 0; }
static inline int box_net_send_command_start(const uint8_t dserv_ip[4], uint16_t port, const char *cmd)
{ return box_net_send_command(dserv_ip, port, cmd); }
static inline int box_net_send_command_poll(void) { return 1; }

static inline int box_net_get_binary(const uint8_t dserv_ip[4], uint16_t port,
                                     const char *key, box_net_bin_sink sink, void *ud)
{ (void) dserv_ip; (void) port; (void) key; (void) sink; (void) ud; return -1; }

#endif /* BOX_NET_BLE_H */
