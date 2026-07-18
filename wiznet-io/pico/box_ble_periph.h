/*
 * box_ble_periph.h -- the HANDHELD's radio (BOX_NET_BLE builds): BLE
 * peripheral advertising the extio frame-pipe service (common/dserv_ble.h,
 * pico/extio_pipe.gatt), pumping whole 128-byte dserv frames between the ATT
 * server and box_net_ble.h's cross-core queues.
 *
 * CORE 0 ONLY, poll arch -- same placement, staged bring-up, watchdog dance,
 * core-1 park, fail-soft, and boot-loop guard as the receiver's
 * box_ble_central.h (see the essays there; they are not repeated). The two
 * headers deliberately share their state names (box_ble_state, the hold
 * flags, the request word): a build includes exactly ONE of them (BOX_BLE =
 * receiver, BOX_NET_BLE = handheld), and rt_main/cmd_exec reference the
 * shared names under a combined ifdef.
 *
 * Link contract (drives box_net_ble.h's box_ble_link): connected AND the
 * receiver subscribed to TX notifications AND ATT_MTU >= 131 so every frame
 * is one PDU. The MTU exchange is client-initiated; we check both at the
 * exchange event and again at subscribe time (order varies by stack).
 *
 * No pairing/bonding yet: the pipe is open (bench stage). The allowlist +
 * LE Secure Connections come with the bonding stage (BLE.md).
 */
#ifndef BOX_BLE_PERIPH_H
#define BOX_BLE_PERIPH_H

#ifdef BOX_NET_BLE

#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "pico/btstack_cyw43.h"
#include "hardware/watchdog.h"
#include "dserv_config.h"
#include "dserv_ble.h"
#include "box_console.h"
#include "extio_pipe.h"             /* generated ATT DB (pico_btstack_make_gatt_header) */

#define BOX_BLE_WDT_BRINGUP_MS 8000
#define BOX_BLE_WDT_RUN_MS     2000    /* == BOX_WDT_MS in wizchip_dserv_config.c */

/* generated handle names are a mouthful (and keep the .gatt's lowercase) --
 * alias the three we use */
#define HH_TX_VALUE_HANDLE ATT_CHARACTERISTIC_d5e70002_8f2c_4b6a_9ae5_3c7a10a5b2c1_01_VALUE_HANDLE
#define HH_TX_CCC_HANDLE   ATT_CHARACTERISTIC_d5e70002_8f2c_4b6a_9ae5_3c7a10a5b2c1_01_CLIENT_CONFIGURATION_HANDLE
#define HH_RX_VALUE_HANDLE ATT_CHARACTERISTIC_d5e70003_8f2c_4b6a_9ae5_3c7a10a5b2c1_01_VALUE_HANDLE

/* ---- shared-name state (see header block): requests, state, park flags ---- */
enum { BOX_BLE_REQ_NONE = 0, BOX_BLE_REQ_STATUS, BOX_BLE_REQ_RETRY };
static volatile uint8_t box_ble_req;
static inline void box_ble_request(uint8_t r) { box_ble_req = r; }

enum { BOX_BLE_OFF = 0, BOX_BLE_FAIL, BOX_BLE_POWERUP, BOX_BLE_UP };
static volatile uint8_t box_ble_state;         /* UP = stack working (advertising / connected) */
static volatile uint8_t box_ble_hold_req, box_ble_held;   /* rt_main parks on these ([1-2]/4) */

static inline const char *box_ble_state_str(void)
{
    switch (box_ble_state) {
    case BOX_BLE_FAIL:    return "fail";
    case BOX_BLE_POWERUP: return "powerup";
    case BOX_BLE_UP:      return "up";
    default:              return "off";
    }
}

/* ---- connection state (core-0 owned) ---- */
static hci_con_handle_t hh_con = HCI_CON_HANDLE_INVALID;
static uint8_t  hh_subscribed, hh_mtu_ok, hh_can_send_pending;
static uint16_t hh_mtu;
static uint32_t hh_rx_drop, hh_notified;
static bd_addr_t hh_local_addr;
static btstack_packet_callback_registration_t hh_hci_cb;
static uint32_t box_ble_t_bringup;

static char    hh_name[8 + PICO_NAME_MAX];     /* "extio-" + cfg name (adv + GAP name) */
static uint8_t hh_adv_data[21];                /* flags(3) + 128-bit svc uuid list(18) */
static uint8_t hh_scan_rsp[2 + sizeof hh_name];

static inline uint32_t box_ble__ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void box_ble_checkpoint(void)           /* drain our prints + pet (core-0 mainline) */
{
    for (int i = 0; i < 32 && (box_ring_used(&box_log_ring[0]) ||
                               box_ring_used(&box_log_ring[1])); i++)
        box_console_drain();
    watchdog_update();
}

static void hh_link_update(void)
{
    uint8_t up = (hh_con != HCI_CON_HANDLE_INVALID) && hh_subscribed && hh_mtu_ok;
    if (up != box_ble_link) {
        box_ble_link = up;
        printf("ble: pipe %s%s\n", up ? "UP" : "down",
               up ? " (subscribed, whole-frame MTU)" : "");
    }
}

/* kick the notification pump: ask for a can-send slot if frames are waiting */
static inline void hh_pump_kick(void)
{
    if (box_ble_link && !hh_can_send_pending && queue_get_level(&box_ble_txq)) {
        hh_can_send_pending = 1;
        att_server_request_can_send_now_event(hh_con);
    }
}

static uint16_t hh_att_read(hci_con_handle_t ch, uint16_t att_handle,
                            uint16_t offset, uint8_t *buffer, uint16_t buffer_size)
{
    (void) ch;
    if (att_handle == ATT_CHARACTERISTIC_GAP_DEVICE_NAME_01_VALUE_HANDLE)
        return att_read_callback_handle_blob((const uint8_t *) hh_name,
                                             (uint16_t) strlen(hh_name),
                                             offset, buffer, buffer_size);
    return 0;
}

static int hh_att_write(hci_con_handle_t ch, uint16_t att_handle, uint16_t mode,
                        uint16_t offset, uint8_t *buffer, uint16_t size)
{
    (void) offset;
    if (mode != ATT_TRANSACTION_MODE_NONE) return 0;
    if (att_handle == HH_TX_CCC_HANDLE) {              /* receiver (un)subscribes */
        hh_subscribed = (little_endian_read_16(buffer, 0) &
                         GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION) ? 1 : 0;
        if (hh_subscribed && !hh_mtu_ok) {             /* exchange may precede subscribe */
            hh_mtu = att_server_get_mtu(ch);
            hh_mtu_ok = hh_mtu >= DSERV_BLE_MTU_MIN;
        }
        printf("ble: receiver %ssubscribed (mtu=%u%s)\n", hh_subscribed ? "" : "un",
               hh_mtu, hh_mtu_ok ? "" : " -- TOO SMALL for whole frames");
        hh_link_update();
        hh_pump_kick();
        return 0;
    }
    if (att_handle == HH_RX_VALUE_HANDLE) {            /* a frame from the receiver */
        if (size == DSERV_MSG_LEN) {
            if (!queue_try_add(&box_ble_rxq, buffer)) hh_rx_drop++;
        }
        return 0;
    }
    return 0;
}

static void hh_packet_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size)
{
    (void) channel; (void) size;
    if (packet_type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {

    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
        gap_local_bd_addr(hh_local_addr);
        gap_advertisements_set_params(0x0030, 0x0030, 0 /*ADV_IND*/, 0, NULL, 0x07, 0);
        gap_advertisements_set_data((uint8_t) sizeof hh_adv_data, hh_adv_data);
        gap_scan_response_set_data((uint8_t)(2 + strlen(hh_name)), hh_scan_rsp);
        gap_advertisements_enable(1);
        box_ble_state = BOX_BLE_UP;
#ifndef BOX_BLE_HANG_FOR_DEBUG
        watchdog_enable(BOX_BLE_WDT_RUN_MS, true);
        printf("ble: [4/4] up in %lums, addr %s -- advertising as \"%s\" (watchdog back to %dms)\n",
               (unsigned long)(box_ble__ms() - box_ble_t_bringup),
               bd_addr_to_str(hh_local_addr), hh_name, BOX_BLE_WDT_RUN_MS);
#else
        printf("ble: [4/4] up in %lums, addr %s -- advertising as \"%s\" (DEBUG: watchdog stays off)\n",
               (unsigned long)(box_ble__ms() - box_ble_t_bringup),
               bd_addr_to_str(hh_local_addr), hh_name);
#endif
        break;

    case HCI_EVENT_LE_META:
        if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
            hh_con = hci_subevent_le_connection_complete_get_connection_handle(packet);
            printf("ble: central connected\n");
        }
        break;

    case ATT_EVENT_MTU_EXCHANGE_COMPLETE:
        hh_mtu = att_event_mtu_exchange_complete_get_MTU(packet);
        hh_mtu_ok = hh_mtu >= DSERV_BLE_MTU_MIN;
        printf("ble: mtu=%u (%s)\n", hh_mtu, hh_mtu_ok ? "whole-frame ok" : "TOO SMALL -- pipe held down");
        hh_link_update();
        break;

    case ATT_EVENT_CAN_SEND_NOW: {
        hh_can_send_pending = 0;
        uint8_t f[DSERV_MSG_LEN];
        if (box_ble_link && queue_try_remove(&box_ble_txq, f)) {
            att_server_notify(hh_con, HH_TX_VALUE_HANDLE, f, DSERV_MSG_LEN);
            hh_notified++;
            hh_pump_kick();                    /* more queued -> next slot */
        }
        break; }

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        hh_con = HCI_CON_HANDLE_INVALID;
        hh_subscribed = 0; hh_mtu_ok = 0; hh_mtu = 0; hh_can_send_pending = 0;
        hh_link_update();
        printf("ble: central disconnected -- advertising again\n");
        gap_advertisements_enable(1);
        break;

    default: break;
    }
}

/* One-time radio bring-up: same staged/parked/watchdog-widened shape as the
 * receiver (see box_ble_central.h for the whys, incl. the 2026-07-16/17
 * incident notes). */
static void box_ble_periph_init_once(const pico_config_t *cfg)
{
    uint32_t t0 = box_ble_t_bringup = box_ble__ms(), t1;
#ifdef BOX_BLE_HANG_FOR_DEBUG
    watchdog_disable();
    printf("ble: DEBUG-HANG build -- watchdog DISABLED; a wedge now hangs for the J-Link\n");
#else
    watchdog_enable(BOX_BLE_WDT_BRINGUP_MS, true);
    printf("ble: radio bring-up (watchdog widened to %dms for the fw uploads)\n",
           BOX_BLE_WDT_BRINGUP_MS);
#endif
    /* adv payloads (static storage: btstack keeps the pointers) */
    snprintf(hh_name, sizeof hh_name, DSERV_BLE_NAME_PFX "%s", dserv_cfg_name(cfg));
    hh_adv_data[0] = 2;  hh_adv_data[1] = BLUETOOTH_DATA_TYPE_FLAGS; hh_adv_data[2] = 0x06;
    hh_adv_data[3] = 17; hh_adv_data[4] = BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS;
    reverse_128(DSERV_BLE_SVC_UUID, &hh_adv_data[5]);          /* adv carries UUIDs little-endian */
    hh_scan_rsp[0] = (uint8_t)(1 + strlen(hh_name));
    hh_scan_rsp[1] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
    memcpy(&hh_scan_rsp[2], hh_name, strlen(hh_name));

    printf("ble: [1/4] cyw43_arch_init (wifi fw upload; core 1 parked for [1-2]/4)...\n");
    box_ble_checkpoint();
    sleep_ms(30);                              /* let the ferry flush before parking core 1 */
    box_ble_hold_req = 1;
    { uint32_t hd = box_ble__ms();
      while (!box_ble_held && box_ble__ms() - hd < 500) tight_loop_contents(); }

#ifdef BOX_BLE_BREADCRUMBS
    /* Arm the wedge breadcrumbs (see BLE.md "slot-boot radio wedge"): scratch[0]
     * names the last stage reached inside the SDK bring-up; scratch[1] marks the
     * crumbs valid. Survives the watchdog reset; main() prints it next boot.
     * NEVER scratch[2]/[3]: the bootrom parks rom_reboot's p0/p1 there until
     * the reset fires -- crumbing [2] corrupted the OTA arm's update base. */
    watchdog_hw->scratch[0] = 0x01;
    watchdog_hw->scratch[1] = 0xB007CB07u;
#endif
    if (cyw43_arch_init() != 0) {              /* FAIL-SOFT: box keeps its console; no transport though */
        box_ble_hold_req = 0;
        box_ble_state = BOX_BLE_FAIL;
#ifndef BOX_BLE_HANG_FOR_DEBUG
        watchdog_enable(BOX_BLE_WDT_RUN_MS, true);
#endif
        printf("ble: cyw43_arch_init FAILED -- no radio; handheld has NO transport this boot\n");
        return;
    }
    watchdog_update();
    /* (crumbs stay armed: the BT fw upload rides later polls through the same
     * bus path, so scratch[2] keeps naming the last radio-bus stage reached) */
    t1 = box_ble__ms();
    printf("ble: [1/4] done (%lums)\n", (unsigned long)(t1 - t0));
    printf("ble: [2/4] btstack init (run loop + HCI + ATT server)...\n");
    btstack_cyw43_init(cyw43_arch_async_context());
    l2cap_init();
    sm_init();
    att_server_init(profile_data, hh_att_read, hh_att_write);
    hh_hci_cb.callback = &hh_packet_handler;
    hci_add_event_handler(&hh_hci_cb);
    att_server_register_packet_handler(hh_packet_handler);
    box_ble_hold_req = 0;                      /* release core 1: ferry resumes */
    printf("ble: [2/4] done (%lums)\n", (unsigned long)(box_ble__ms() - t1));
    printf("ble: [3/4] hci power on (BT fw upload runs inside the next polls)...\n");
    box_ble_checkpoint();
    hci_power_control(HCI_POWER_ON);
    box_ble_state = BOX_BLE_POWERUP;           /* [4/4] prints from the WORKING transition */
}

/* Core-0 service, once per loop pass. The radio IS this box's transport, so
 * bring-up ignores cfg->ble_en (it runs regardless) but keeps the wdt-boot
 * skip-once guard (a radio-wedge boot loop would take the console with it). */
static inline void box_ble_periph_service(const pico_config_t *cfg, int core1_ready, int wdt_boot)
{
    static uint8_t tried, wdt_note;
    if (box_ble_state == BOX_BLE_OFF && !tried && core1_ready) {
        tried = 1;
        if (wdt_boot && !wdt_note) {
            wdt_note = 1;
            printf("ble: last boot was a WATCHDOG reset -- radio bring-up skipped\n"
                   "ble: type `ble enable 1` to retry (watch which [n/4] stage is last if it dies)\n");
        } else {
            box_ble_periph_init_once(cfg);
        }
    }

    if (box_ble_state >= BOX_BLE_POWERUP) {
        uint32_t tp = box_ble__ms();
        cyw43_arch_poll();
        if (box_ble_state == BOX_BLE_POWERUP) {
            uint32_t dp = box_ble__ms() - tp;
            watchdog_update();
            if (dp > 250)
                printf("ble: (bring-up poll took %lums -- BT fw upload)\n", (unsigned long) dp);
        }
        hh_pump_kick();                        /* frames queued by core 1 -> notification slots */
    }

    uint8_t r = box_ble_req;
    if (r == BOX_BLE_REQ_NONE) return;
    box_ble_req = BOX_BLE_REQ_NONE;
    switch (r) {
    case BOX_BLE_REQ_STATUS:
        printf("ble: state=%s pipe=%s", box_ble_state_str(), box_ble_link ? "UP" : "down");
        if (box_ble_state == BOX_BLE_UP)
            printf(" addr=%s name=%s con=%d sub=%u mtu=%u tx(ok=%lu drop=%lu nolink=%lu) notified=%lu rxdrop=%lu",
                   bd_addr_to_str(hh_local_addr), hh_name,
                   hh_con != HCI_CON_HANDLE_INVALID, hh_subscribed, hh_mtu,
                   (unsigned long) box_ble_tx_ok, (unsigned long) box_ble_tx_drop,
                   (unsigned long) box_ble_tx_nolink, (unsigned long) hh_notified,
                   (unsigned long) hh_rx_drop);
        printf("\n");
        if (box_ble_state == BOX_BLE_FAIL)
            printf("ble: radio init failed this boot -- reboot to retry\n");
        break;
    case BOX_BLE_REQ_RETRY:
        if (box_ble_state == BOX_BLE_OFF) tried = 0;
        break;
    default: break;
    }
}

#endif /* BOX_NET_BLE */
#endif /* BOX_BLE_PERIPH_H */
