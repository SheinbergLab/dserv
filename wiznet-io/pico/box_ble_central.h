/*
 * box_ble_central.h -- BLE central for the receiver box (BOX_BLE builds:
 * radio-capable pico2w-family boards running the USB transport). Two layers:
 *
 *   scaffold (2026-07-16): radio bring-up + scan/inventory from the console.
 *   frame pipe (2026-07-17): connect to a handheld advertising the extio pipe
 *     service (common/dserv_ble.h), subscribe to its TX characteristic, and
 *     RELAY whole 128-byte dserv frames both ways -- handheld notifications go
 *     up the box's own USB transport (name-multiplexed: the handheld appears
 *     to dserv as its own extio/<name> box), and host frames addressed to
 *     names that aren't ours come back down the write characteristic
 *     (wizchip_dserv_config.c forwards CFG_NONE frames here).
 *
 * PLACEMENT: everything here runs on CORE 0 (the console/I2C/flash core), per
 * BLE.md -- the RT core never touches the radio. cyw43 is brought up in POLL
 * mode (pico_cyw43_arch_poll + CYW43_LWIP=0), so btstack callbacks execute
 * inside box_ble_service()'s cyw43_arch_poll() on core 0's MAINLINE -- printf
 * from a callback lands in core 0's log ring legally (the SPSC rings forbid
 * printing from IRQ context, which rules out the threadsafe_background arch).
 * The pipe crosses cores through two pico queues (the g_cmd_q house pattern):
 *
 *   notifications (core 0) -> box_pipe_rxq -> rt_main relays via box_net_client_send
 *   on_frame CFG_NONE (core 1) -> box_pipe_txq -> core 0 writes to the handheld
 *
 * WATCHDOG (learned the hard way, 2026-07-16 bench: `ble enable 1` ->
 * [boot=watchdog]): core 0 is BOTH the radio host and the watchdog petter.
 * Bring-up stalls core 0 in opaque multi-second chunks it cannot pet through
 * -- so the window is widened to BOX_BLE_WDT_BRINGUP_MS for the phase (the
 * caller guarantees it is already armed -- core-1-ready gate -- so this is a
 * re-arm), each stage announces itself and DRAINS the log ring before
 * running, and each bring-up poll is followed by a pet. (Measured healthy
 * bring-up is ~925ms total -- the incident that motivated all this was
 * closed as ENVIRONMENTAL; see BLE.md.) The run window is restored at the
 * UP/FAIL transition.
 *
 * ENABLE CONTRACT: persisted cfg->ble_en (CLI `ble enable 1`), default OFF --
 * a lab of boxes advertising/scanning by default is RF noise and a pairing
 * surprise surface. Enable is LIVE (lazy bring-up once core 1 is up); disable
 * stops scanning immediately but the radio stays powered until reboot
 * (btstack teardown isn't worth the state-machine risk). cyw43_arch_init
 * failure is FAIL-SOFT: log it and carry on as a plain USB box. A WATCHDOG
 * boot skips the persisted auto-bring-up once and says so (no boot loop from
 * a saved ble_en=1 + a broken radio path; `ble enable 1` retries manually).
 * Core 0 reading cfg->ble_en is the same benign-staleness contract as
 * ain_en/ain_rate/ain_gain (g_cfg is core-1-owned after launch).
 *
 * PIPE CONTRACT (bench stage -- no pairing/bonding yet): `ble pipe 1` scans
 * for the first advertiser carrying the extio service UUID, connects,
 * requires ATT_MTU >= 131 (one whole frame per PDU -- both ends refuse the
 * pipe below that), subscribes, relays; on disconnect it rescans until
 * `ble pipe 0`. The bonding/allowlist stage replaces first-match with
 * bonded-address matching (BLE.md).
 *
 * Cross-core: core 1 (console cmd_exec) posts one-shot requests via a single
 * volatile byte (the phylink pattern); core 0 consumes them here. Status for
 * core 1's `show` trailer is exposed as volatile bytes (benign-stale reads).
 *
 * BOX_BLE_HANG_FOR_DEBUG (BLE_DEBUG=1/2 in build.sh): J-Link hang builds --
 * watchdog disabled so a bring-up wedge is attachable; =1 also forces
 * bring-up at boot. Bench-only; never deploy.
 *
 * Flash note: btstack's TLV (bond storage, set up by btstack_cyw43_init even
 * before we bond) lives at the RP2350 default PICO_FLASH_BANK_STORAGE_OFFSET
 * = top-of-flash minus 12K -> banks at -12K/-8K. Our persist sector is the
 * LAST 4K (pico_flash.h) and the A/B slots sit near the bottom (pt.json /
 * pt-pico2w.json), so nothing overlaps. Do NOT "fix" the offset to -8K: that
 * puts TLV bank 1 exactly on the persist sector.
 */
#ifndef BOX_BLE_CENTRAL_H
#define BOX_BLE_CENTRAL_H

#ifdef BOX_BLE

#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "pico/btstack_cyw43.h"     /* btstack_cyw43_init: run loop + HCI + TLV (NOT auto-run by the arch) */
#include "pico/util/queue.h"
#include "hardware/watchdog.h"
#include "dserv_config.h"
#include "dserv_msg.h"
#include "dserv_ble.h"              /* the frozen pipe UUIDs + MTU floor */
#include "box_console.h"            /* box_console_drain: stage prints must land BEFORE the stage runs */

/* Watchdog windows (ms). BRINGUP covers the opaque firmware-upload stalls;
 * RUN must stay == BOX_WDT_MS in wizchip_dserv_config.c (kept as a separate
 * define only because that one is declared below our include point). */
#define BOX_BLE_WDT_BRINGUP_MS 8000
#define BOX_BLE_WDT_RUN_MS     2000

#if defined(BOX_BLE_HANG_FOR_DEBUG) && !defined(BOX_BLE_NO_FORCE)
#define BOX_BLE_FORCE_EN 1
#else
#define BOX_BLE_FORCE_EN 0
#endif

/* ---- core 1 -> core 0 one-shot requests (single word, phylink-style) ---- */
enum { BOX_BLE_REQ_NONE = 0, BOX_BLE_REQ_STATUS, BOX_BLE_REQ_SCAN_ON, BOX_BLE_REQ_SCAN_OFF,
       BOX_BLE_REQ_RETRY, BOX_BLE_REQ_PIPE_ON, BOX_BLE_REQ_PIPE_OFF };
static volatile uint8_t box_ble_req;
static inline void box_ble_request(uint8_t r) { box_ble_req = r; }

/* Park the RT core during the chip bring-up ([1/4]+[2/4]) so cyw43_arch_init
 * runs with core 0 owning the machine solo. rt_main polls hold_req at its
 * loop top and acks via held; DI edges queue in their IRQ ring meanwhile. */
static volatile uint8_t box_ble_hold_req, box_ble_held;

/* ---- state (core-0 owned; the volatile bytes are read by core 1's `show`) ---- */
enum { BOX_BLE_OFF = 0, BOX_BLE_FAIL, BOX_BLE_POWERUP, BOX_BLE_UP };
static volatile uint8_t box_ble_state;
static volatile uint8_t box_ble_scanning;

static inline const char *box_ble_state_str(void)
{
    switch (box_ble_state) {
    case BOX_BLE_FAIL:    return "fail";
    case BOX_BLE_POWERUP: return "powerup";
    case BOX_BLE_UP:      return "up";
    default:              return "off";
    }
}

/* ---- the frame pipe (central side) ---- */
enum { PIPE_IDLE = 0, PIPE_SCAN, PIPE_CONNECT, PIPE_DISCOVER, PIPE_CHARS, PIPE_SUBSCRIBE, PIPE_STREAM };
static volatile uint8_t box_pipe_state;        /* read by `show`/status */
static uint8_t  box_pipe_mode;                 /* `ble pipe 1` latch: auto-(re)connect while set */
static bd_addr_t     pipe_addr;
static bd_addr_type_t pipe_addr_type;
static hci_con_handle_t pipe_con = HCI_CON_HANDLE_INVALID;
static gatt_client_service_t        pipe_svc;
static gatt_client_characteristic_t pipe_tx_char, pipe_rx_char;   /* handheld's TX (notify) / RX (write) */
static uint8_t  pipe_have_tx, pipe_have_rx;
static gatt_client_notification_t   pipe_listener;
static uint16_t pipe_mtu;
static uint32_t pipe_rx_frames, pipe_rx_badlen, pipe_rx_qdrop;    /* handheld -> us */
static uint32_t pipe_tx_frames, pipe_tx_qdrop;                    /* us -> handheld */
static uint8_t  pipe_tx_pending;                                  /* held frame awaiting a WNR slot */
static uint8_t  pipe_tx_frame[DSERV_MSG_LEN];

static inline const char *box_pipe_state_str(void)
{
    switch (box_pipe_state) {
    case PIPE_SCAN:      return "scan";
    case PIPE_CONNECT:   return "connect";
    case PIPE_DISCOVER:  return "discover";
    case PIPE_CHARS:     return "chars";
    case PIPE_SUBSCRIBE: return "subscribe";
    case PIPE_STREAM:    return "STREAM";
    default:             return "idle";
    }
}

/* Cross-core frame queues (init on core 0 BEFORE core 1 launches). */
#define BOX_PIPE_RXQ_DEPTH 16     /* handheld frames awaiting relay up the USB pipe */
#define BOX_PIPE_TXQ_DEPTH 8      /* host frames awaiting the radio */
static queue_t box_pipe_rxq, box_pipe_txq;
static inline void box_ble_pipe_queues_init(void)
{
    queue_init(&box_pipe_rxq, DSERV_MSG_LEN, BOX_PIPE_RXQ_DEPTH);
    queue_init(&box_pipe_txq, DSERV_MSG_LEN, BOX_PIPE_TXQ_DEPTH);
}

/* CORE 1: forward one host frame (a name that isn't ours -- CFG_NONE in
 * on_frame) toward the handheld. Just a queue add; core 0 owns the radio. */
static inline void box_ble_pipe_forward(const uint8_t *frame)
{
    if (box_pipe_state != PIPE_STREAM) return;           /* no pipe, no forward */
    if (!queue_try_add(&box_pipe_txq, frame)) pipe_tx_qdrop++;
}

/* CORE 1: drain relayed handheld frames (rt_main sends each up the box's own
 * transport; name multiplexing does the rest). */
static inline int box_ble_pipe_pop_rx(uint8_t *frame)
{ return queue_try_remove(&box_pipe_rxq, frame); }

/* Scan inventory: each address prints ONCE at discovery. Bounded. */
#define BOX_BLE_SEEN_MAX 24
typedef struct { bd_addr_t addr; uint8_t addr_type; int8_t rssi; } box_ble_seen_t;
static box_ble_seen_t box_ble_seen[BOX_BLE_SEEN_MAX];
static uint8_t  box_ble_seen_n;
static uint32_t box_ble_reports;
static bd_addr_t box_ble_local_addr;
static btstack_packet_callback_registration_t box_ble_hci_cb;
static uint32_t box_ble_t_bringup;

static inline uint32_t box_ble__ms(void) { return to_ms_since_boot(get_absolute_time()); }

/* Make queued prints visible NOW and pet the watchdog: core 0 is both the log
 * drainer and the petter, and the stage about to run may stall past the run
 * window. Called only from core-0 mainline. */
static void box_ble_checkpoint(void)
{
    for (int i = 0; i < 32 && (box_ring_used(&box_log_ring[0]) ||
                               box_ring_used(&box_log_ring[1])); i++)
        box_console_drain();
    watchdog_update();
}

/* adv payload carries a 128-bit service UUID? (little-endian on the wire) */
static int adv_has_pipe_service(const uint8_t *data, uint8_t dlen)
{
    ad_context_t ctx;
    for (ad_iterator_init(&ctx, dlen, data); ad_iterator_has_more(&ctx); ad_iterator_next(&ctx)) {
        uint8_t t = ad_iterator_get_data_type(&ctx);
        if (t != BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS &&
            t != BLUETOOTH_DATA_TYPE_INCOMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS) continue;
        uint8_t n = ad_iterator_get_data_len(&ctx);
        const uint8_t *d = ad_iterator_get_data(&ctx);
        for (uint8_t off = 0; off + 16 <= n; off += 16) {
            uint8_t be[16];
            reverse_128(d + off, be);
            if (memcmp(be, DSERV_BLE_SVC_UUID, 16) == 0) return 1;
        }
    }
    return 0;
}

/* ---- pipe TX pump: one write-without-response per free slot (core 0) ---- */
static void pipe_tx_pump(void)
{
    if (box_pipe_state != PIPE_STREAM) return;
    if (!pipe_tx_pending) {
        if (!queue_try_remove(&box_pipe_txq, pipe_tx_frame)) return;
        pipe_tx_pending = 1;
    }
    uint8_t rc = gatt_client_write_value_of_characteristic_without_response(
                     pipe_con, pipe_rx_char.value_handle, DSERV_MSG_LEN, pipe_tx_frame);
    if (rc == ERROR_CODE_SUCCESS) { pipe_tx_pending = 0; pipe_tx_frames++; }
    /* else: no credit this pass (GATT_CLIENT_BUSY etc.) -- retry next pass */
}

static void pipe_teardown(const char *why)
{
    if (box_pipe_state == PIPE_STREAM)
        gatt_client_stop_listening_for_characteristic_value_updates(&pipe_listener);
    pipe_con = HCI_CON_HANDLE_INVALID;
    pipe_have_tx = pipe_have_rx = 0;
    pipe_tx_pending = 0;
    printf("ble: pipe down (%s)%s\n", why, box_pipe_mode ? " -- rescanning" : "");
    if (box_pipe_mode) {                       /* auto-reconnect while `ble pipe 1` */
        box_pipe_state = PIPE_SCAN;
        gap_start_scan();
    } else {
        box_pipe_state = PIPE_IDLE;
        if (!box_ble_scanning) gap_stop_scan();
    }
}

/* GATT client events for the pipe (registered per query call). */
static void pipe_gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void) channel; (void) size;
    if (packet_type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {

    case GATT_EVENT_SERVICE_QUERY_RESULT:
        gatt_event_service_query_result_get_service(packet, &pipe_svc);
        break;

    case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
        gatt_client_characteristic_t ch;
        gatt_event_characteristic_query_result_get_characteristic(packet, &ch);
        if (memcmp(ch.uuid128, DSERV_BLE_TX_UUID, 16) == 0) { pipe_tx_char = ch; pipe_have_tx = 1; }
        if (memcmp(ch.uuid128, DSERV_BLE_RX_UUID, 16) == 0) { pipe_rx_char = ch; pipe_have_rx = 1; }
        break; }

    case GATT_EVENT_NOTIFICATION: {            /* a frame from the handheld */
        uint16_t len = gatt_event_notification_get_value_length(packet);
        const uint8_t *v = gatt_event_notification_get_value(packet);
        if (len != DSERV_MSG_LEN) { pipe_rx_badlen++; break; }
        if (queue_try_add(&box_pipe_rxq, v)) pipe_rx_frames++;
        else pipe_rx_qdrop++;                  /* core 1 stalled; frame dropped (repeating data self-heals) */
        break; }

    case GATT_EVENT_QUERY_COMPLETE: {
        uint8_t status = gatt_event_query_complete_get_att_status(packet);
        if (status != ATT_ERROR_SUCCESS) {
            printf("ble: pipe gatt step failed (state=%s att=0x%02x)\n", box_pipe_state_str(), status);
            gap_disconnect(pipe_con);
            break;
        }
        switch (box_pipe_state) {
        case PIPE_DISCOVER:
            if (pipe_svc.start_group_handle == 0) {
                printf("ble: pipe service not found on that device\n");
                gap_disconnect(pipe_con);
                break;
            }
            box_pipe_state = PIPE_CHARS;
            gatt_client_discover_characteristics_for_service(pipe_gatt_handler, pipe_con, &pipe_svc);
            break;
        case PIPE_CHARS:
            if (!pipe_have_tx || !pipe_have_rx) {
                printf("ble: pipe characteristics missing (tx=%u rx=%u)\n", pipe_have_tx, pipe_have_rx);
                gap_disconnect(pipe_con);
                break;
            }
            gatt_client_listen_for_characteristic_value_updates(&pipe_listener, pipe_gatt_handler,
                                                                pipe_con, &pipe_tx_char);
            box_pipe_state = PIPE_SUBSCRIBE;
            gatt_client_write_client_characteristic_configuration(pipe_gatt_handler, pipe_con,
                    &pipe_tx_char, GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
            break;
        case PIPE_SUBSCRIBE:
            gatt_client_get_mtu(pipe_con, &pipe_mtu);
            if (pipe_mtu < DSERV_BLE_MTU_MIN) {
                printf("ble: pipe MTU %u < %u -- whole-frame PDUs impossible; dropping link\n",
                       pipe_mtu, DSERV_BLE_MTU_MIN);
                gap_disconnect(pipe_con);
                break;
            }
            box_pipe_state = PIPE_STREAM;
            printf("ble: pipe STREAMING (mtu=%u) -- handheld frames now relay to dserv\n", pipe_mtu);
            break;
        default: break;
        }
        break; }

    default: break;
    }
}

/* HCI events. POLL mode: runs inside cyw43_arch_poll() on core 0's mainline,
 * so printf -> core 0's log ring is legal (never an IRQ here). */
static void box_ble_hci_handler(uint8_t packet_type, uint16_t channel,
                                uint8_t *packet, uint16_t size)
{
    (void) channel; (void) size;
    if (packet_type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {

    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            gap_local_bd_addr(box_ble_local_addr);
            box_ble_state = BOX_BLE_UP;
#ifndef BOX_BLE_HANG_FOR_DEBUG
            watchdog_enable(BOX_BLE_WDT_RUN_MS, true);   /* bring-up over: run window back on */
            printf("ble: [4/4] up in %lums total, addr %s (watchdog back to %dms)\n",
                   (unsigned long)(box_ble__ms() - box_ble_t_bringup),
                   bd_addr_to_str(box_ble_local_addr), BOX_BLE_WDT_RUN_MS);
#else
            printf("ble: [4/4] up in %lums total, addr %s (DEBUG build: watchdog stays off)\n",
                   (unsigned long)(box_ble__ms() - box_ble_t_bringup),
                   bd_addr_to_str(box_ble_local_addr));
#endif
        }
        break;

    case GAP_EVENT_ADVERTISING_REPORT: {
        box_ble_reports++;
        bd_addr_t addr;
        gap_event_advertising_report_get_address(packet, addr);
        uint8_t at   = gap_event_advertising_report_get_address_type(packet);
        int8_t  rssi = (int8_t) gap_event_advertising_report_get_rssi(packet);
        const uint8_t *data = gap_event_advertising_report_get_data(packet);
        uint8_t dlen = gap_event_advertising_report_get_data_length(packet);

        /* pipe hunt first: first advertiser carrying our service UUID wins
         * (bench policy; the bonding stage matches bonded addresses instead) */
        if (box_pipe_state == PIPE_SCAN && adv_has_pipe_service(data, dlen)) {
            memcpy(pipe_addr, addr, 6);
            pipe_addr_type = (bd_addr_type_t) at;
            box_pipe_state = PIPE_CONNECT;
            if (!box_ble_scanning) gap_stop_scan();      /* inventory scan may keep running */
            printf("ble: pipe device %s rssi=%d -- connecting\n", bd_addr_to_str(addr), rssi);
            gap_connect(pipe_addr, pipe_addr_type);
            break;
        }

        if (!box_ble_scanning) break;                    /* inventory prints only when asked */
        for (int i = 0; i < box_ble_seen_n; i++)
            if (memcmp(box_ble_seen[i].addr, addr, 6) == 0) { box_ble_seen[i].rssi = rssi; return; }
        if (box_ble_seen_n >= BOX_BLE_SEEN_MAX) return;
        box_ble_seen_t *s = &box_ble_seen[box_ble_seen_n++];
        memcpy(s->addr, addr, 6); s->addr_type = at; s->rssi = rssi;

        char name[32]; name[0] = '\0';
        ad_context_t ctx;
        for (ad_iterator_init(&ctx, dlen, data); ad_iterator_has_more(&ctx); ad_iterator_next(&ctx)) {
            uint8_t t = ad_iterator_get_data_type(&ctx);
            if (t == BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME ||
                t == BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME) {
                uint8_t n = ad_iterator_get_data_len(&ctx);
                if (n > sizeof name - 1) n = sizeof name - 1;
                memcpy(name, ad_iterator_get_data(&ctx), n); name[n] = '\0';
            }
        }
        printf("ble: found %s type=%u rssi=%d%s%s\n",
               bd_addr_to_str(addr), at, rssi, name[0] ? " name=" : "", name);
        break; }

    case HCI_EVENT_LE_META:
        if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_COMPLETE &&
            box_pipe_state == PIPE_CONNECT) {
            if (hci_subevent_le_connection_complete_get_status(packet) != 0) {
                printf("ble: pipe connect failed\n");
                pipe_teardown("connect failed");
                break;
            }
            pipe_con = hci_subevent_le_connection_complete_get_connection_handle(packet);
            memset(&pipe_svc, 0, sizeof pipe_svc);
            pipe_have_tx = pipe_have_rx = 0;
            box_pipe_state = PIPE_DISCOVER;
            gatt_client_discover_primary_services_by_uuid128(pipe_gatt_handler, pipe_con,
                                                             DSERV_BLE_SVC_UUID);
        }
        break;

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        if (pipe_con != HCI_CON_HANDLE_INVALID &&
            hci_event_disconnection_complete_get_connection_handle(packet) == pipe_con)
            pipe_teardown("disconnected");
        break;

    default: break;
    }
}

/* One-time radio bring-up (core 0, lazy: first pass with ble_en=1 once core 1
 * is up -- which also guarantees the watchdog is armed, so the widen below is
 * a re-arm). Stage-announced: each stage's line is DRAINED to the console
 * before the stage runs, so a death names its stage. */
static void box_ble_init_once(void)
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
    printf("ble: [1/4] cyw43_arch_init (wifi fw upload; core 1 parked for [1-2]/4)...\n");
    box_ble_checkpoint();
    sleep_ms(30);                             /* let core 1's ferry flush the line to the USB console
                                               * BEFORE we park it -- if [1/4] dies, the line was seen */
    box_ble_hold_req = 1;                     /* park the RT core (see box_ble_hold_req block) */
    { uint32_t hd = box_ble__ms();
      while (!box_ble_held && box_ble__ms() - hd < 500) tight_loop_contents(); }

#ifdef BOX_BLE_BREADCRUMBS
    watchdog_hw->scratch[0] = 0x01;           /* arm the wedge breadcrumbs (BLE.md */
    watchdog_hw->scratch[1] = 0xB007CB07u;    /* "slot-boot radio wedge"): main()  */
#endif                                        /* prints scratch[0] after a wdt boot.
                                               * NEVER scratch[2]/[3]: rom_reboot
                                               * parks its p0/p1 there (OTA arm!) */
    if (cyw43_arch_init() != 0) {             /* FAIL-SOFT: still a working USB box */
        box_ble_hold_req = 0;
        box_ble_state = BOX_BLE_FAIL;
#ifndef BOX_BLE_HANG_FOR_DEBUG
        watchdog_enable(BOX_BLE_WDT_RUN_MS, true);
#endif
        printf("ble: cyw43_arch_init FAILED -- no radio; continuing as a plain USB box\n");
        return;
    }
    watchdog_update();
    t1 = box_ble__ms();
    printf("ble: [1/4] done (%lums)\n", (unsigned long)(t1 - t0));
    printf("ble: [2/4] btstack init (run loop + HCI + TLV store; may format a flash bank)...\n");
    btstack_cyw43_init(cyw43_arch_async_context());   /* memory + run loop + HCI transport + TLV */
    l2cap_init();
    sm_init();                                /* pairing/bonding machinery (bonding stage) */
    gatt_client_init();                       /* the pipe's client side */
    box_ble_hci_cb.callback = &box_ble_hci_handler;
    hci_add_event_handler(&box_ble_hci_cb);
    gap_set_scan_parameters(1 /*active: fetch names*/, 0x0060 /*60ms interval*/, 0x0030 /*30ms window*/);
    box_ble_hold_req = 0;                     /* release core 1: ferry resumes, buffered lines flush */
    printf("ble: [2/4] done (%lums)\n", (unsigned long)(box_ble__ms() - t1));
    printf("ble: [3/4] hci power on (BT fw upload runs inside the next polls)...\n");
    box_ble_checkpoint();
    hci_power_control(HCI_POWER_ON);
    box_ble_state = BOX_BLE_POWERUP;          /* [4/4] prints from the WORKING transition */
}

/* Core-0 service, once per loop pass: lazy bring-up, radio poll, pipe pump,
 * and console requests. ~us when idle or disabled. wdt_boot = last boot was a
 * watchdog reset -> skip the PERSISTED auto-bring-up once (boot-loop guard). */
static inline void box_ble_service(const pico_config_t *cfg, int core1_ready, int wdt_boot)
{
    static uint8_t tried, wdt_note;
    if (box_ble_state == BOX_BLE_OFF && (cfg->ble_en || BOX_BLE_FORCE_EN) && !tried && core1_ready) {
        tried = 1;
        if (!BOX_BLE_FORCE_EN && wdt_boot && !wdt_note) {
            wdt_note = 1;
            printf("ble: enabled, but last boot was a WATCHDOG reset -- auto bring-up skipped\n"
                   "ble: type `ble enable 1` to retry (watch which [n/4] stage is last if it dies)\n");
        } else {
            box_ble_init_once();
        }
    }

    if (box_ble_state >= BOX_BLE_POWERUP) {
        uint32_t tp = box_ble__ms();
        cyw43_arch_poll();                  /* drives cyw43 + the btstack run loop (callbacks fire here) */
        if (box_ble_state == BOX_BLE_POWERUP) {
            uint32_t dp = box_ble__ms() - tp;
            watchdog_update();              /* fw-upload polls stall: pet right behind each one */
            if (dp > 250)
                printf("ble: (bring-up poll took %lums -- BT fw upload)\n", (unsigned long) dp);
        }
        pipe_tx_pump();                     /* host frames queued by core 1 -> the handheld */
    }

    /* pipe_en (persist v16): auto-arm the relay ONCE per radio-up, so a saved
     * `ble pipe 1` survives reboots with zero console touches. One-shot at the
     * up-edge: a runtime `ble pipe 0` afterwards stays off (no re-arm loop);
     * later datapoint/CLI toggles fire their own requests. */
    { static uint8_t pipe_auto_armed;
      if (box_ble_state != BOX_BLE_UP) pipe_auto_armed = 0;
      else if (!pipe_auto_armed) {
          pipe_auto_armed = 1;
          if (cfg->pipe_en && !box_pipe_mode) {
              printf("ble: pipe auto-arm (persisted pipe_en)\n");
              box_ble_request(BOX_BLE_REQ_PIPE_ON);
          }
      } }

    /* live-disable: stop the chatter now; full radio power-off happens at reboot */
    if (!cfg->ble_en && !BOX_BLE_FORCE_EN) {
        if (box_ble_scanning) { gap_stop_scan(); box_ble_scanning = 0;
                                printf("ble: scan stopped (ble disabled)\n"); }
        if (box_pipe_mode) {
            box_pipe_mode = 0;
            if (pipe_con != HCI_CON_HANDLE_INVALID) gap_disconnect(pipe_con);
            else if (box_pipe_state == PIPE_SCAN) { gap_stop_scan(); box_pipe_state = PIPE_IDLE; }
            printf("ble: pipe off (ble disabled)\n");
        }
    }

    uint8_t r = box_ble_req;
    if (r == BOX_BLE_REQ_NONE) return;
    box_ble_req = BOX_BLE_REQ_NONE;
    switch (r) {
    case BOX_BLE_REQ_STATUS:
        printf("ble: state=%s en=%u pipe=%s", box_ble_state_str(), cfg->ble_en, box_pipe_state_str());
        if (box_ble_state == BOX_BLE_UP) {
            printf(" addr=%s scanning=%u seen=%u reports=%lu",
                   bd_addr_to_str(box_ble_local_addr), box_ble_scanning,
                   box_ble_seen_n, (unsigned long) box_ble_reports);
            if (box_pipe_state == PIPE_STREAM)
                printf("\nble: pipe %s mtu=%u relay(up=%lu updrop=%lu badlen=%lu down=%lu downdrop=%lu)",
                       bd_addr_to_str(pipe_addr), pipe_mtu,
                       (unsigned long) pipe_rx_frames, (unsigned long) pipe_rx_qdrop,
                       (unsigned long) pipe_rx_badlen, (unsigned long) pipe_tx_frames,
                       (unsigned long) pipe_tx_qdrop);
        }
        printf("\n");
        if (box_ble_state == BOX_BLE_FAIL)
            printf("ble: radio init failed this boot -- reboot to retry\n");
        if (box_ble_state == BOX_BLE_OFF && !cfg->ble_en)
            printf("ble: enable with `ble enable 1` (then `save` to persist)\n");
        for (int i = 0; i < box_ble_seen_n; i++)
            printf("ble:   %s type=%u rssi=%d\n", bd_addr_to_str(box_ble_seen[i].addr),
                   box_ble_seen[i].addr_type, box_ble_seen[i].rssi);
        break;
    case BOX_BLE_REQ_RETRY:                 /* `ble enable 1` typed again after a skipped/failed OFF */
        if (box_ble_state == BOX_BLE_OFF) tried = 0;
        break;
    case BOX_BLE_REQ_SCAN_ON:
        if (box_ble_state != BOX_BLE_UP) { printf("ble: not up (state=%s)\n", box_ble_state_str()); break; }
        if (!cfg->ble_en && !BOX_BLE_FORCE_EN) { printf("ble: disabled (`ble enable 1` first)\n"); break; }
        box_ble_seen_n = 0; box_ble_reports = 0;
        gap_start_scan(); box_ble_scanning = 1;
        printf("ble: scanning (new devices print once; `ble` lists, `ble scan 0` stops)\n");
        break;
    case BOX_BLE_REQ_SCAN_OFF:
        if (box_ble_scanning) {
            box_ble_scanning = 0;
            if (box_pipe_state != PIPE_SCAN) gap_stop_scan();   /* pipe hunt may still need the scanner */
        }
        printf("ble: scan stopped (%u device%s, %lu reports)\n",
               box_ble_seen_n, box_ble_seen_n == 1 ? "" : "s", (unsigned long) box_ble_reports);
        break;
    case BOX_BLE_REQ_PIPE_ON:
        if (box_ble_state != BOX_BLE_UP) { printf("ble: not up (state=%s)\n", box_ble_state_str()); break; }
        if (box_pipe_mode) { printf("ble: pipe already %s\n", box_pipe_state_str()); break; }
        box_pipe_mode = 1;
        box_pipe_state = PIPE_SCAN;
        gap_start_scan();
        printf("ble: pipe hunting (first advertiser with the extio service wins; `ble pipe 0` stops)\n");
        break;
    case BOX_BLE_REQ_PIPE_OFF:
        if (!box_pipe_mode && box_pipe_state == PIPE_IDLE) { printf("ble: pipe already off\n"); break; }
        box_pipe_mode = 0;
        if (pipe_con != HCI_CON_HANDLE_INVALID) gap_disconnect(pipe_con);   /* teardown via disconnect evt */
        else {
            if (box_pipe_state == PIPE_SCAN && !box_ble_scanning) gap_stop_scan();
            box_pipe_state = PIPE_IDLE;
            printf("ble: pipe off\n");
        }
        break;
    default: break;
    }
}

#endif /* BOX_BLE */
#endif /* BOX_BLE_CENTRAL_H */
