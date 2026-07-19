/*
 * wizchip_dserv_config.c -- the "box" firmware. Transport-agnostic (box_net.h):
 * builds for wired W6300 (default), Pico 2 W WiFi (-DBOX_NET_LWIP), plain Pico 2
 * USB-CDC (-DBOX_NET_USB), or the strap-selected dual image (-DBOX_NET_DUAL).
 *
 * Namespace: all boxes group under extio/<name>/... (BOX_CLASS="extio" + device name):
 *   extio/<name>/config/(keys)  host->box persistent settings (pushed by dserv; saved to flash)
 *   extio/<name>/cmd/(keys)     host->box transient actions   (pushed by dserv)
 *                       cmd/do/<n>=0|1, cmd/do/<n>/pulse_us, cmd/save|reboot|factory
 *                       cmd/do/<n>/at=<us>   pulse at beginobs+us (box-timed, deterministic)
 *                       cmd/timer/<n>/at=<us> post state/timer/<n> at beginobs+us
 *   extio/<name>/state/(keys)   box->dserv published status   (box connects to dserv as client)
 *
 * DUAL-CORE SPLIT (RP2350): core 1 is the real-time core. It owns every
 * latency-sensitive device and runs only us-bounded work per pass: the
 * transport (W6300 SPI or TinyUSB -- ALL tud_* calls live on core 1), the DI
 * edge IRQs, the pulse/sched alarm pool (box_alarm_pool), config/cmd
 * dispatch, all publishes, and the obs clock sync -- whose anchor error is
 * bounded by core 1's worst-case pass time, which is the whole point.
 * Core 0 boots the box, launches core 1, then does the blockable rest:
 * console line editing + log-ring draining (printf on EITHER core writes a
 * lock-free ring, ~1us -- box_console.h), ADS1115 + fuel-gauge I2C, and
 * flash saves. STAGE 2: the wired/usb/dual images are copy_to_ram binaries
 * (RP2350 SRAM) + PICO_FLASH_ASSUME_CORE1_SAFE, so a core-0 flash save no
 * longer parks core 1 at all -- no DI-edge loss on `save` -- and XIP cache
 * misses stop existing as a jitter source. (pico2w still uses the lockout:
 * its image + cyw43 firmware is too big for RAM.)
 *
 * The cores share nothing but SPSC byte rings, three queues, and a couple of
 * volatile words. g_cfg is written ONLY by core 1 once it's launched; core 0
 * reads just ain_en/ain_rate/ain_gain (+ ble_en/pipe_en on BOX_BLE builds --
 * the radio also lives on core 0, box_ble_central.h) (benign staleness).
 *   core0 -> core1: console lines (g_cmd_q), AIN samples (g_ain_q)
 *   core1 -> core0: flash ops carrying a config snapshot (g_save_q), log rings
 *
 * Clock alignment: the box also subscribes to ess/in_obs. Each obs begin/end
 * edge carries dserv's timestamp; the box snaps a box->dserv offset from it
 * (pico_clock.h) and stamps the DI/DO events it publishes in dserv time, so they
 * interleave with local events as if the box were a local device. By default
 * the anchor's box time is the frame's ARRIVAL (error = transport delay); wire
 * the rig host's physical obs TTL to a box pin (`sync pin N`) and the anchor
 * becomes the IRQ-latched EDGE time instead -- transport drops out of the
 * error budget, leaving only the host's pin-write-to-timestamp skew
 * (state/sync/source says which mode each anchor used; state/sync/transport_us
 * logs the measured frame-behind-edge delay). An optional obs-mirror output
 * (config/obs/pin, or CLI `obs pin N`) is driven to the live ess/in_obs value
 * -- an LED to eyeball obs tracking, or a scope tap against the TTL input for
 * an end-to-end software-path latency self-test.
 *
 * dserv relay setup:  %reg <box_ip> <CFG_PORT> 1 ; %match <name>/config keys ; %match <name>/cmd keys ; %match ess/in_obs
 * dserv target for state:  set <name>/config/dserv/ip + dserv/port, then save.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"          /* reset_usb_boot() -> reboot into USB BOOTSEL */

#include "dserv_config.h"
#include "pico_persist.h"
#include "pico_cli.h"
#include "pico_flash.h"
#include "pico_gpio.h"
#include "pico_group.h"         /* DI chord groups: settle machine (portable) */
#include "pico_clock.h"
#include "box_net.h"
#include "box_console.h"        /* ring-buffered printf + console byte plumbing */
#ifdef BOX_FUEL_MAX17048
#include "pico_fuel.h"          /* on-board fuel gauge: compiled per board  */
#endif
#include "pico_ain.h"           /* ADS1115 analog-in: always compiled, runtime-enabled (ain_en) */
#include "pico_oled.h"          /* SSD1306 status display: always compiled, runtime-enabled (oled_en) */
#include "pico_ota.h"           /* Stage-0 OTA receiver: pull image -> scratch flash -> sha verify */
#include "pico_ota_slot.h"      /* Stage-1 probe: bootrom A/B partition + boot-info (read-only) */
#ifdef BOX_BLE
#include "box_ble_central.h"    /* BLE central (receiver): radio on CORE 0, poll mode, fail-soft */
#endif
#ifdef BOX_NET_BLE
#include "box_ble_periph.h"     /* BLE peripheral (handheld): radio on CORE 0; transport = box_net_ble.h */
#endif
#include "pico_status_led.h"    /* handheld WS2812 status LED (BOX_STATUS_LED = thingplus-handheld only) */

/* USB data-CDC publish accounting (box_net_usb.h increments; `txstats` prints).
 * Defined here unconditionally so every transport build links. */
volatile uint32_t box_usb_tx_ok, box_usb_tx_wait, box_usb_tx_drop, box_usb_tx_notready;

#define CFG_PORT    5010
#define RXBUF_SIZE  1024
#define HEARTBEAT_MS 1000
#define SYNC_DP     "ess/in_obs"   /* obs begin/end edge -> box->dserv clock anchor */
#define CLI_LINE_MAX 128

/* Hardware sync anchor sanity window: a latched TTL edge pairs with an
 * ess/in_obs frame only if the edge is at most this old at frame arrival.
 * Must exceed the worst transport+stack delay (ms-class on USB) and stay
 * well under the shortest obs on/off cadence (seconds), so a stale edge
 * from the previous toggle can never anchor the current one. */
#define SYNC_EDGE_WINDOW_US 250000

#ifndef BOX_FW_VERSION
#define BOX_FW_VERSION "dev"       /* build.sh bakes `git describe` here */
#endif
#ifndef BOX_BUILD_TARGET
#define BOX_BUILD_TARGET "dev"     /* build.sh bakes $TARGET -- shelf image match key */
#endif
#ifndef BOX_BOARD_ID
#define BOX_BOARD_ID "pico2"       /* build.sh bakes $PICO_BOARD -- OTA compat filter */
#endif

alarm_pool_t *box_alarm_pool;       /* core-1 alarm pool (pulse/sched/DHCP-tick IRQs on the RT core);
                                     * extern'd by pico_gpio.h + box_net_w6300.h */

static uint8_t g_rxbuf[RXBUF_SIZE];
static pico_config_t  g_cfg;
static dserv_framer_t g_framer;
#ifdef BOX_USB_OTA_DOCKED
static uint8_t        g_usbrxbuf[RXBUF_SIZE];  /* docked handheld: CDC1 host-OTA bytes -> on_frame */
static dserv_framer_t g_usb_framer;            /* its own framer (zero-init = reset) */
#endif
static box_clock_t    g_clock;      /* box->dserv time offset, snapped at obs edges */

/* Stamp a box-clock time for an event we're about to publish. Normal boxes map
 * box->dserv time here (box_clock_stamp; 0 until synced -> dserv arrival-stamps).
 * The BLE HANDHELD instead emits its RAW time_us_64 unmapped: the radio hop is a
 * clock boundary it can't cross alone (no obs edge reaches it), so the RECEIVER
 * rewrites hh->dserv when it relays the frame (pipe_rewrite_ts, echo-sync). One
 * seam, so every publish_* below is transport-correct without per-site #ifdefs.
 * See BLE.md "Time: stamp at source, one clock boundary, rewrite once". */
#if defined(BOX_NET_BLE)
static inline uint64_t event_stamp(uint64_t t_us) { return t_us; }              /* raw; receiver maps it */
#else
static inline uint64_t event_stamp(uint64_t t_us) { return box_clock_stamp(&g_clock, t_us); }
#endif

#ifdef BOX_BLE
/* RECEIVER only: the handheld->receiver clock, learned by echo-sync (the radio
 * boundary; box_ble_central.h feeds it). Unsynced until echo converges, so
 * box_clock_stamp returns 0. */
static box_clock_t g_hh_clock;

/* On-change cache for the state/ble/* telemetry (bonds/encrypted/pairing). File
 * scope so the connect-burst path can reset it to -1: a state change that fires
 * while the USB transport is mid-(re)connect gets its publish DROPPED by
 * box_net_client_send, but the cache still records it as sent -> dserv keeps the
 * stale value forever (the boot-reconnect enc-datapoint bug). Re-forcing a
 * publish on the up==2 burst re-syncs over a transport that's actually up. */
static int g_ble_tlm_lb = -1, g_ble_tlm_le = -1, g_ble_tlm_lp = -1;

/* Rewrite a relayed handheld frame's timestamp at the radio boundary: raw
 * handheld time -> dserv time, translated exactly once (BLE.md "Time"). Two
 * affine hops -- hh->receiver via g_hh_clock (echo-sync), then receiver->dserv
 * via our own g_clock -- because the frame is forwarded WHOLE and never runs
 * through our publish path, so neither mapping applies on its own. A ts of 0 is
 * the handheld asking for arrival-stamp: left as-is. EITHER clock unsynced
 * yields 0 => dserv arrival-stamps => exactly today's behavior, so this is a
 * safe no-op until BOTH echo-sync and a receiver obs anchor are live. */
static void pipe_rewrite_ts(uint8_t *frame)
{
    dserv_msg_t m;
    if (dserv_msg_parse(frame, &m) != 0 || m.timestamp == 0) return;
    uint64_t rx = box_clock_stamp(&g_hh_clock, m.timestamp);   /* hh -> receiver */
    uint64_t ds = rx ? box_clock_stamp(&g_clock, rx) : 0;      /* receiver -> dserv */
    dserv_msg_set_timestamp(frame, ds);
}

/* Echo-sync estimator (Increment 2, CORE 1). Drains raw echo samples from the
 * core-0 sampler, keeps the LOWEST-RTT one per ECHO_BUCKET_MS window (min-RTT
 * filter: the floor sample has the least conn-interval quantization, so its
 * midpoint<->h_recv pair is the cleanest anchor -- ~200us on the rig), and feeds
 * it to box_clock_sync(&g_hh_clock, ...) as a TRUSTED anchor -- offset snap +
 * rate teach, the exact EMA discipline validated for box->dserv. g_hh_clock is
 * then written AND read (pipe_rewrite_ts) on core 1 only: no cross-core struct
 * tearing. A bucket whose best RTT is still far above the running floor (a
 * congestion burst) is SKIPPED rather than snap the offset to a noisy value. */
#define ECHO_BUCKET_MS       1000    /* one anchor/s -> spacing > box_clock's 0.5s pair-min */
#define ECHO_FLOOR_MARGIN_US 8000    /* accept a bucket only within this of the running floor */
static void echo_estimator_service(void)
{
    static uint32_t bucket_ms, floor_us = 0xFFFFFFFFu, best_rtt;
    static uint8_t  have_best;
    static uint64_t best_mid, best_hrecv;

    echo_sample_t s;
    while (queue_try_remove(&g_echo_q, &s)) {          /* keep the min-RTT sample this bucket */
        uint64_t mid = s.r0 + s.rtt / 2;               /* receiver-time midpoint of the round trip */
        if (!have_best || s.rtt < best_rtt) { have_best = 1; best_rtt = s.rtt; best_mid = mid; best_hrecv = s.h_recv; }
    }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (!bucket_ms) bucket_ms = now;
    if (now - bucket_ms < ECHO_BUCKET_MS || !have_best) return;
    bucket_ms = now;

    if (best_rtt < floor_us) floor_us = best_rtt;      /* running floor */
    if (best_rtt <= floor_us + ECHO_FLOOR_MARGIN_US)   /* clean enough -> trusted anchor */
        box_clock_sync(&g_hh_clock, best_mid, best_hrecv, 1);
    have_best = 0;

    char nm[64]; uint8_t f[DSERV_MSG_LEN];             /* once/s sync health */
    uint8_t synced = g_hh_clock.synced ? (g_hh_clock.rate_valid ? 2 : 1) : 0;
    g_echo_synced = synced;                            /* mirror for core 0's adaptive-latency manager */
    dserv_state_name(&g_cfg, nm, sizeof nm, "echo/synced");
    dserv_msg_int(f, nm, 0, synced);
    box_net_client_send(f, DSERV_MSG_LEN);
    dserv_state_name(&g_cfg, nm, sizeof nm, "echo/rate_ppb");
    dserv_msg_int(f, nm, 0, g_hh_clock.rate_ppb);
    box_net_client_send(f, DSERV_MSG_LEN);
}
#endif

static uint64_t g_obs_begin_us;     /* box-clock time of the last beginobs -> anchor for scheduled events */
static int32_t g_wdt;

/* ---- cross-core plumbing (queues made on core 0 before core 1 launches) ---- */
typedef struct { uint64_t t_us; int16_t val; uint8_t ch; } ain_sample_t;
typedef struct { uint8_t erase_only; pico_config_t cfg; } save_req_t;
static queue_t g_cmd_q;             /* core0 console line -> core1 executes (owns cfg/pins) */
static queue_t g_ain_q;             /* core0 I2C sample   -> core1 stamps + publishes      */
static queue_t g_save_q;            /* core1 save/erase   -> core0 writes flash            */
static volatile int g_core1_ready;
#ifdef BOX_FUEL_MAX17048
static volatile int g_fuel_soc = -1;   /* core0 reads the gauge 1 Hz; core1 heartbeat publishes */
#endif
#ifdef BOX_NET_DUAL
static int g_xport;                 /* boot transport (strap/policy), set on core 0 before launch */
static uint8_t g_auto_sense;        /* auto policy: still watching the PHY for an eth upgrade
                                     * (set at boot on core 0; owned by core 1 after launch) */
#endif
static uint32_t g_core1_stack[1024];   /* 4KB: cmd_exec's out[1024] is the deep frame */

/* Hot-path telemetry (di:/dp:/sync:) is OFF by default. With the log ring a
 * print costs ~1us on the RT core, but it's still per-event chatter -- toggle
 * live from the console: `debug 1` / `debug 0` (default off, not persisted). */
static volatile int g_log_verbose = 0;
#define DBG(...) do { if (g_log_verbose) printf(__VA_ARGS__); } while (0)

/* ---- STAGE 3: dual-core hardware watchdog ----
 * Core 0 arms the RP2350 watchdog once core 1 is up, then pets it each pass --
 * but ONLY while core 1's loop heartbeat keeps advancing. Either core wedging
 * (loop stuck, deadlock, IRQ storm starving the RT loop) therefore becomes a
 * BOX_WDT_MS self-reboot instead of a power cycle. Deliberately NOT armed
 * before core 1 comes up: a deterministic boot wedge keeps the console alive
 * for diagnosis instead of boot-looping. Worst legit core-1 pass is the lazy
 * W6300 bringup (~200ms reset pulse) -- 10x inside the window. pause_on_debug
 * keeps SWD sessions from tripping it. `wdt 0` (console) switches core 0 to
 * unconditional petting for bench experiments that intentionally stall core 1;
 * `wdt 1` restores the gate. Boot cause is printed at startup and published
 * once per connect as state/boot = watchdog|soft|power. */
#define BOX_WDT_MS 2000
static volatile uint32_t g_rt_beat;       /* core-1 loop heartbeat */
static volatile uint8_t  g_wdt_gate = 1;  /* 1 = pet only while core 1 advances */
static const char       *g_boot_reason = "power";
#ifdef BOX_BLE_BREADCRUMBS
static uint32_t          g_ble_crumb;        /* last-run radio-path breadcrumb (0 = none) */
#endif

/* ---- status snapshot for the core-0 OLED (single-writer: core 1) ----
 * Byte fields only, so cross-core reads are benign-stale, never torn --
 * the same contract as core 0's ain_en/rate/gain reads. */
typedef struct {
    uint8_t ip[4];
    uint8_t usb, sensing;       /* active transport / auto still watching   */
    uint8_t cli_up, srv_up;     /* state client / dserv config connect-back */
    uint8_t obs;                /* live ess/in_obs                          */
    uint8_t di_n, di_pins[4], di_lvls;   /* first 4 configured DIs (logical) */
} box_status_t;
static volatile box_status_t g_stat;

/* ======================================================================== *
 *  CORE 1 -- real-time: transport, DI, alarms, dispatch, publishes, sync
 * ======================================================================== */

/* ---- publish helpers (box -> dserv, best-effort) ---- */
static void publish_di(const di_event_t *e)
{
    char leaf[24], nm[64]; uint8_t f[DSERV_MSG_LEN];
    snprintf(leaf, sizeof leaf, "di/%u", e->pin);
    dserv_state_name(&g_cfg, nm, sizeof nm, leaf);
    uint8_t lvl = (uint8_t) di_logical(&g_cfg, e->pin, e->level);   /* publish logical level */
    /* edge time -> dserv (normal box) or raw handheld time (BLE, receiver maps it);
     * 0 => dserv arrival-stamps pre-sync */
    dserv_msg_int(f, nm, event_stamp(e->t_us), lvl);
    box_net_client_send(f, DSERV_MSG_LEN);
    DBG("di: pin%u=%u @%lluus\n", e->pin, lvl, (unsigned long long) e->t_us);
}

/* ---- DI chord groups (pico_group.h): one atomic bitmask per group ----
 * state/group/<label> (int), stamped at the FIRST edge of the settled episode
 * so downstream RT is the true movement onset while the value is the completed
 * chord. t_us == 0 => a (re)connect seed, arrival-stamped like publish_di_levels. */
static group_rt_t g_grp[PICO_NGROUPS];

static void publish_group(int g, uint8_t bits, uint64_t t_us)
{
    char gn[PICO_LABEL_MAX + 4], leaf[40], nm[64]; uint8_t f[DSERV_MSG_LEN];
    dserv_group_name(&g_cfg, g, gn, sizeof gn);
    snprintf(leaf, sizeof leaf, "group/%s", gn);
    dserv_state_name(&g_cfg, nm, sizeof nm, leaf);
    dserv_msg_int(f, nm, t_us ? event_stamp(t_us) : 0, bits);
    box_net_client_send(f, DSERV_MSG_LEN);
    DBG("grp: %s=0x%x @%lluus\n", gn, bits, (unsigned long long) t_us);
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
 * offset, one datapoint each, all stamped at the obs edge's dserv time. Plus
 * which anchor fed the clock (hw = latched TTL edge, sw = frame arrival) and,
 * when hw, the measured frame-behind-edge delay -- free per-anchor transport
 * latency telemetry (receipt - physical edge). */
static void publish_sync(uint64_t dserv_us, uint64_t box_us, int64_t offset_us,
                         int hw, int64_t transport_us)
{
    char nm[64]; uint8_t f[DSERV_MSG_LEN];
    dserv_state_name(&g_cfg, nm, sizeof nm, "sync/dserv_us");
    dserv_msg_int64(f, nm, dserv_us, (int64_t) dserv_us);  box_net_client_send(f, DSERV_MSG_LEN);
    dserv_state_name(&g_cfg, nm, sizeof nm, "sync/box_us");
    dserv_msg_int64(f, nm, dserv_us, (int64_t) box_us);    box_net_client_send(f, DSERV_MSG_LEN);
    dserv_state_name(&g_cfg, nm, sizeof nm, "sync/offset_us");
    dserv_msg_int64(f, nm, dserv_us, offset_us);           box_net_client_send(f, DSERV_MSG_LEN);
    dserv_state_name(&g_cfg, nm, sizeof nm, "sync/source");
    dserv_msg_string(f, nm, dserv_us, hw ? "hw" : "sw");   box_net_client_send(f, DSERV_MSG_LEN);
    if (transport_us >= 0) {
        dserv_state_name(&g_cfg, nm, sizeof nm, "sync/transport_us");
        dserv_msg_int64(f, nm, dserv_us, transport_us);    box_net_client_send(f, DSERV_MSG_LEN);
    }
    if (g_clock.rate_valid) {                              /* learned crystal rate (hw anchors) */
        dserv_state_name(&g_cfg, nm, sizeof nm, "sync/rate_ppb");
        dserv_msg_int(f, nm, dserv_us, g_clock.rate_ppb);  box_net_client_send(f, DSERV_MSG_LEN);
    }
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
#ifdef BOX_FUEL_MAX17048
    int soc = g_fuel_soc;                                /* sampled by core 0, 1 Hz */
    if (soc >= 0) {
        dserv_state_name(&g_cfg, nm, sizeof nm, "battery");
        dserv_msg_int(f, nm, 0, soc);                    box_net_client_send(f, DSERV_MSG_LEN);
    }
#endif
}

static void publish_ain(int ch, int16_t v, uint64_t t_us)  /* state/ain/<ch>, stamped at acquisition */
{
    char leaf[16], nm[64]; uint8_t f[DSERV_MSG_LEN];
    snprintf(leaf, sizeof leaf, "ain/%d", ch);
    dserv_state_name(&g_cfg, nm, sizeof nm, leaf);
    dserv_msg_int(f, nm, event_stamp(t_us), v);
    box_net_client_send(f, DSERV_MSG_LEN);
}

/* ---- scheduled events: "fire at beginobs + delta_us" (do/<n>/at, timer/<n>/at) ----
 * A hardware alarm (on the core-1 pool -> IRQ on the RT core) fires at the
 * box-clock instant beginobs_box + delta. Its callback (IRQ) does the
 * timing-critical part -- the non-blocking DO pulse -- and marks the slot
 * FIRED; the main loop then posts state/timer/<tid> stamped at the fire time
 * (network I/O must not run in the IRQ). The physical output timing is thus
 * local + deterministic; the notification is for the host's awareness. */
#define SCHED_MAX 8
typedef enum { SCH_FREE = 0, SCH_ARMED, SCH_FIRED } sch_state_t;
static struct {
    volatile sch_state_t st;
    uint8_t  pin;                    /* pulse pin, or 0xFF for timer-only */
    uint8_t  tid;                    /* -> state/timer/<tid>              */
    uint32_t width;                  /* pulse width us                    */
    uint64_t fire_us;                /* box-clock fire time (for the stamp) */
} g_sched[SCHED_MAX];

static int64_t sched_cb(alarm_id_t id, void *user)     /* runs in the core-1 timer IRQ */
{
    (void) id;
    int s = (int)(uintptr_t) user;
    if (g_sched[s].st != SCH_ARMED) return 0;
    if (g_sched[s].pin != 0xFF) {                       /* timing-critical: the pulse (non-blocking) */
        gpio_cmd_t c = { GPIO_OP_PULSE, g_sched[s].pin, g_sched[s].width };
        pico_gpio_exec(&g_cfg, &c);
    }
    g_sched[s].st = SCH_FIRED;                          /* main loop posts state/timer/<tid> */
    return 0;                                           /* one-shot */
}

static void sched_arm(int pin, int tid, uint32_t width, uint32_t delta_us)
{
    if (g_obs_begin_us == 0) { printf("sched: no beginobs yet, ignoring\n"); return; }
    int s = -1;
    for (int i = 0; i < SCHED_MAX; i++) if (g_sched[i].st == SCH_FREE) { s = i; break; }
    if (s < 0) { printf("sched: table full\n"); return; }
    g_sched[s].pin = (uint8_t) pin; g_sched[s].tid = (uint8_t) tid; g_sched[s].width = width;
    g_sched[s].fire_us = g_obs_begin_us + delta_us;
    g_sched[s].st = SCH_ARMED;
    if (box_alarm_pool)
        alarm_pool_add_alarm_at(box_alarm_pool, from_us_since_boot(g_sched[s].fire_us),
                                sched_cb, (void *)(uintptr_t) s, true);
    else
        add_alarm_at(from_us_since_boot(g_sched[s].fire_us), sched_cb, (void *)(uintptr_t) s, true);
}

static void publish_timer(int tid, uint64_t fire_us)   /* state/timer/<tid>, stamped at the fire time */
{
    char leaf[20], nm[64]; uint8_t f[DSERV_MSG_LEN];
    snprintf(leaf, sizeof leaf, "timer/%d", tid);
    dserv_state_name(&g_cfg, nm, sizeof nm, leaf);
    dserv_msg_int(f, nm, event_stamp(fire_us), 1);
    box_net_client_send(f, DSERV_MSG_LEN);
}

static void sched_publish_fired(void)                  /* call from the core-1 loop when connected */
{
    for (int i = 0; i < SCHED_MAX; i++)
        if (g_sched[i].st == SCH_FIRED) {
            publish_timer(g_sched[i].tid, g_sched[i].fire_us);
            g_sched[i].st = SCH_FREE;
        }
}

/* ---- self-registration: tell dserv to push us config+cmd (box-initiated) ----
 * Registration = %reg + 3 %match lines, one transient connection each, run as a
 * NON-BLOCKING sequence on this core: reg_request() arms it, reg_service()
 * advances one us-bounded step per pass via box_net_send_command_start/_poll.
 * (The old blocking sequence cost ~2-10ms -- worst ~0.35s per command -- inside
 * a single RT pass; now the wall time is the same but spread across passes, so
 * DI capture and the sync anchor never feel it.) Order is preserved: %reg lands
 * before the %match lines that attach to it. dserv answers "<rc> " per command,
 * so failures are counted and the watchdog below retries until all accepted. */
#define REG_NCMDS 4
static char    g_reg_cmds[REG_NCMDS][112];
static uint8_t g_reg_n, g_reg_idx, g_reg_fails, g_reg_quiet, g_reg_active;
static uint8_t g_reg_want_full, g_reg_want_matches, g_reg_want_loud;

static void reg_request(int full, int quiet)
{
    if (full) g_reg_want_full = 1; else g_reg_want_matches = 1;
    if (!quiet) g_reg_want_loud = 1;
}

#ifdef BOX_NET_DUAL
static void reg_reset(void)   /* transport switched: abandon any half-run sequence */
{
    g_reg_active = 0;
    g_reg_want_full = g_reg_want_matches = g_reg_want_loud = 0;
}
#endif

static void reg_build(int full)
{
    uint8_t bip[4]; box_net_local_ip(bip);
    char pfx[64]; dserv_cfg_prefix(&g_cfg, pfx, sizeof pfx);   /* extio/<name> */
    int n = 0;
    if (full)
        snprintf(g_reg_cmds[n++], sizeof g_reg_cmds[0], "%%reg %u.%u.%u.%u %u 1\n",
                 bip[0], bip[1], bip[2], bip[3], CFG_PORT);
    snprintf(g_reg_cmds[n++], sizeof g_reg_cmds[0], "%%match %u.%u.%u.%u %u %s/config/* 1\n",
             bip[0], bip[1], bip[2], bip[3], CFG_PORT, pfx);
    snprintf(g_reg_cmds[n++], sizeof g_reg_cmds[0], "%%match %u.%u.%u.%u %u %s/cmd/* 1\n",
             bip[0], bip[1], bip[2], bip[3], CFG_PORT, pfx);
    snprintf(g_reg_cmds[n++], sizeof g_reg_cmds[0], "%%match %u.%u.%u.%u %u %s 1\n",
             bip[0], bip[1], bip[2], bip[3], CFG_PORT, SYNC_DP);   /* obs clock sync */
    g_reg_n = (uint8_t) n; g_reg_idx = 0; g_reg_fails = 0;
}

static void reg_service(void)
{
    if (!g_reg_active) {
        if (!g_reg_want_full && !g_reg_want_matches) return;
        reg_build(g_reg_want_full);                 /* IP snapshot at sequence start */
        g_reg_quiet = !g_reg_want_loud;
        g_reg_want_full = g_reg_want_matches = g_reg_want_loud = 0;
        if (box_net_send_command_start(g_cfg.dserv_ip, dserv_cfg_port(&g_cfg), g_reg_cmds[0]) != 0)
            return;                                 /* channel busy; a later request retries */
        g_reg_active = 1;
        return;
    }
    int r = box_net_send_command_poll();
    if (r == 0) return;                             /* current command in flight */
    if (r < 0) g_reg_fails++;
    if (++g_reg_idx < g_reg_n) {
        if (box_net_send_command_start(g_cfg.dserv_ip, dserv_cfg_port(&g_cfg), g_reg_cmds[g_reg_idx]) != 0) {
            g_reg_fails += g_reg_n - g_reg_idx;     /* can't continue; watchdog re-runs it */
            g_reg_active = 0;
        }
        return;
    }
    g_reg_active = 0;
    if (!g_reg_quiet) {
        char pfx[64]; dserv_cfg_prefix(&g_cfg, pfx, sizeof pfx);
        printf("self-registered with dserv %u.%u.%u.%u:%u as %s%s\n",
               g_cfg.dserv_ip[0], g_cfg.dserv_ip[1], g_cfg.dserv_ip[2], g_cfg.dserv_ip[3],
               dserv_cfg_port(&g_cfg), pfx,
               g_reg_fails ? " (INCOMPLETE, watchdog will retry)" : "");
    }
}

/* Box identity card, published at every (re)connect: active transport, boot
 * cause, firmware version, own IP, and WHICH dserv this box feeds -- the last
 * one because a box on Ethernet reports to its configured target, and a stale
 * card on some other host's page is only explicable if the box says where its
 * live stream went. Everything a fleet-status web page needs from
 * extio/<name>/state/ alone. */
static void publish_ident(void)
{
    char nm[64], s[24]; uint8_t f[DSERV_MSG_LEN];
    dserv_state_name(&g_cfg, nm, sizeof nm, "transport");
    dserv_msg_string(f, nm, 0, box_net_backend_name());
    box_net_client_send(f, DSERV_MSG_LEN);
    dserv_state_name(&g_cfg, nm, sizeof nm, "boot");
    dserv_msg_string(f, nm, 0, g_boot_reason);
    box_net_client_send(f, DSERV_MSG_LEN);
    dserv_state_name(&g_cfg, nm, sizeof nm, "fw");
    dserv_msg_string(f, nm, 0, BOX_FW_VERSION);
    box_net_client_send(f, DSERV_MSG_LEN);
    dserv_state_name(&g_cfg, nm, sizeof nm, "build");   /* shelf image match key */
    dserv_msg_string(f, nm, 0, BOX_BUILD_TARGET);
    box_net_client_send(f, DSERV_MSG_LEN);
    dserv_state_name(&g_cfg, nm, sizeof nm, "board");   /* OTA compat filter */
    dserv_msg_string(f, nm, 0, BOX_BOARD_ID);
    box_net_client_send(f, DSERV_MSG_LEN);
    uint8_t ip[4]; box_net_local_ip(ip);           /* 0.0.0.0 over USB: transport says why */
    snprintf(s, sizeof s, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    dserv_state_name(&g_cfg, nm, sizeof nm, "ip");
    dserv_msg_string(f, nm, 0, s);
    box_net_client_send(f, DSERV_MSG_LEN);
    int usb_target = 0;
#if defined(BOX_NET_USB)
    usb_target = 1;
#elif defined(BOX_NET_DUAL)
    usb_target = box_net_is_usb();
#endif
#ifdef BOX_NET_BLE
    (void) usb_target;
    snprintf(s, sizeof s, "ble-receiver");     /* whichever central relays us to dserv */
#else
    if (usb_target) snprintf(s, sizeof s, "usb-host");
    else snprintf(s, sizeof s, "%u.%u.%u.%u:%u", g_cfg.dserv_ip[0], g_cfg.dserv_ip[1],
                  g_cfg.dserv_ip[2], g_cfg.dserv_ip[3], dserv_cfg_port(&g_cfg));
#endif
    dserv_state_name(&g_cfg, nm, sizeof nm, "dserv");
    dserv_msg_string(f, nm, 0, s);
    box_net_client_send(f, DSERV_MSG_LEN);
}

/* Stage-1 OTA slot probe, done ONCE at boot (core 1 init) into g_slot; published
 * at every (re)connect so the fleet sees each box's partition state remotely.
 *   state/ota/pt      partition count (0 = unpartitioned card, today's baseline)
 *   state/ota/boot    "<boot_type> part=<n>[ buy_pending]"
 *   state/ota/target  inactive slot a new image targets: "part=<t> 0x<base>+<size>"
 *                     or "none" (no PT yet). This IS the future OTA write target. */
static pico_ota_slot_t g_slot;
static volatile uint8_t g_slot_probe_req;   /* cmd/ota/probe -> run once in the RT loop */
static uint8_t g_slot_have;                  /* a probe has completed -> slotinfo is real */
/* ---- TBYB trial state (captured at boot; self-test + buy on core 0) ---- */
static uint8_t  g_boot_type;                 /* rom boot_type (4 = FLASH_UPDATE) */
static int8_t   g_boot_partition = -1;       /* partition we booted from         */
static uint8_t  g_boot_trial;                /* 1 = FLASH_UPDATE + buy-pending -> on trial */
static uint8_t  g_ota_bought;                /* explicit_buy attempted this boot */
static volatile int16_t g_ota_buy_rc = 99;   /* explicit_buy return code (0 = committed); volatile: read via J-Link */
static volatile uint8_t g_net_up;            /* core 1: dserv state-client connected (box->dserv publishing) */
static volatile uint8_t g_srv_up;            /* core 1: dserv connected back to our config server (registered) */
static volatile uint8_t g_ota_arm_req;       /* cmd/ota/arm (or J-Link poke) -> flash-update reboot */
static void publish_ota_str(const char *leaf, const char *val);   /* defined in the Stage-0 block */
static void publish_ota_slotinfo(void)
{
    if (!g_slot_have) return;                /* don't publish stale zeros before a probe */
    char nm[64], v[48]; uint8_t f[DSERV_MSG_LEN];
    dserv_state_name(&g_cfg, nm, sizeof nm, "ota/pt");
    dserv_msg_int(f, nm, 0, g_slot.pt_count);
    box_net_client_send(f, DSERV_MSG_LEN);

    snprintf(v, sizeof v, "%s %s%s", pico_ota_boot_type_str(g_slot.boot_type),
             pico_ota_boot_part_str(g_slot.boot_partition),
             pico_ota_buy_pending(&g_slot) ? " buy_pending" : "");
    dserv_state_name(&g_cfg, nm, sizeof nm, "ota/boot");
    dserv_msg_string(f, nm, 0, v);
    box_net_client_send(f, DSERV_MSG_LEN);

    if (g_slot.target_valid)
        snprintf(v, sizeof v, "part=%d 0x%06lx+%lu", g_slot.target_part,
                 (unsigned long) g_slot.target_base, (unsigned long) g_slot.target_size);
    else
        snprintf(v, sizeof v, "none rc=%ld", (long) g_slot.last_rc);
    dserv_state_name(&g_cfg, nm, sizeof nm, "ota/target");
    dserv_msg_string(f, nm, 0, v);
    box_net_client_send(f, DSERV_MSG_LEN);

    /* boot_diagnostic: raw hex (both halfwords: lo=slot0/partA, hi=slot1/partB)
     * + decoded tokens for the SLOT WE BOOTED. Decodes WHY it did/didn't launch --
     * the tool for the copy_to_ram-from-slot wedge (a failed slot's own diagnostic
     * is read via `picotool info -a` in BOOTSEL, since it can't run to publish). */
    char bd[80], dd[56];
    /* hi halfword = slot1/partB diag, lo = slot0/partA (bootrom doc). We boot from
     * a SLOT, so boot_partition is the -3 sentinel for slot1 (NOT 1 -- the old
     * `== 1` never matched, so a slot-1 boot decoded slot0's diagnostic). */
    uint16_t half = (g_slot.boot_partition == BOOT_PARTITION_SLOT1) ? (uint16_t)(g_slot.boot_diag >> 16)
                                                                    : (uint16_t) g_slot.boot_diag;
    pico_ota_bootdiag_str(half, dd, sizeof dd);
    snprintf(bd, sizeof bd, "%08lx [%s]", (unsigned long) g_slot.boot_diag, dd);
    dserv_state_name(&g_cfg, nm, sizeof nm, "ota/bootdiag");
    dserv_msg_string(f, nm, 0, bd);
    box_net_client_send(f, DSERV_MSG_LEN);
}

/* On-demand bootrom PT/boot probe (core 1, RT loop -> watchdog-armed). Runs the
 * ROM partition calls only when explicitly asked via cmd/ota/probe, so boot is
 * never on the hook for a misbehaving ROM func; if one wedges here the armed
 * watchdog reboots and the box comes back up un-probed and reachable. */
static void ota_slot_service_core1(void)
{
    if (!g_slot_probe_req) return;
    g_slot_probe_req = 0;
    printf("ota probe: running bootrom PT/boot query...\n");
    pico_ota_slot_probe(&g_slot);
    g_slot_have = 1;
    printf("ota probe: pt=%d boot=%s part=%d%s target_valid=%d part=%d 0x%06lx+%lu rc=%ld\n",
           g_slot.pt_count, pico_ota_boot_type_str(g_slot.boot_type), g_slot.boot_partition,
           pico_ota_buy_pending(&g_slot) ? " BUY_PENDING" : "", g_slot.target_valid,
           g_slot.target_part, (unsigned long) g_slot.target_base,
           (unsigned long) g_slot.target_size, (long) g_slot.last_rc);
    publish_ota_slotinfo();
}

/* Arm a TBYB update: probe for the inactive slot, then flash-update-reboot into
 * it (buy-pending). Triggered by cmd/ota/arm or a J-Link poke of g_ota_arm_req.
 * Does NOT return (NO_RETURN reboot). Bench-only for now: the full flow will arm
 * after a slot write + sha verify. Core 1 (owns the ROM probe path). */
static void ota_arm_service_core1(void)
{
    if (!g_ota_arm_req) return;
    g_ota_arm_req = 0;
    pico_ota_slot_probe(&g_slot);                 /* resolve the inactive slot (target_*) */
    if (!g_slot.target_valid) {
        printf("ota arm: no inactive slot (pt=%d rc=%ld) -- need a partition table\n",
               g_slot.pt_count, (long) g_slot.last_rc);
        publish_ota_str("ota/state", "fail");
        publish_ota_str("ota/result", "no_slot");
        return;
    }
    /* rom_reboot(FLASH_UPDATE) wants the XIP address of the slot, not the raw
     * storage offset (pico-examples ota_update: code_start_addr + XIP_BASE). */
    uint32_t update_base = (uint32_t) XIP_BASE + g_slot.target_base;
    printf("ota arm: FLASH_UPDATE reboot -> slot part %d @0x%08lx (xip) in 1s\n",
           g_slot.target_part, (unsigned long) update_base);
    publish_ota_str("ota/state", "armed");
    pico_ota_arm_update(update_base, 1000);          /* NO_RETURN: box reboots into the slot
                                                      * after ~1s (core 0 drains the log meanwhile) */
}

/* Seed state/di/<n> for every configured input at (re)connect, so a UI shows a
 * box's buttons at their settled levels immediately -- not only after the first
 * edge happens to be published. Timestamp 0 = arrival-stamped (a seed, not an
 * edge; edges keep their box-clock stamps via publish_di). */
static void publish_di_levels(void)
{
    for (int i = 0; i < PICO_NPINS; i++)
        if (g_cfg.pin_mode[i] == 2 || g_cfg.pin_mode[i] == 3) {   /* in / in_pullup */
            char leaf[16], nm[64]; uint8_t f[DSERV_MSG_LEN];
            snprintf(leaf, sizeof leaf, "di/%d", i);
            dserv_state_name(&g_cfg, nm, sizeof nm, leaf);
            uint8_t lvl = (uint8_t) di_logical(&g_cfg, i, di_pub_level[i]);
            dserv_msg_int(f, nm, 0, lvl);
            box_net_client_send(f, DSERV_MSG_LEN);
        }
}

/* Re-derive every group's state from the current (debounced, logical) pin
 * levels. Call after boot's apply_config and any pin-mode/group config change
 * -- a mid-episode change abandons the episode rather than emitting from a
 * stale member set. */
static void groups_reset_all(void)
{
    uint8_t logical[PICO_NPINS];
    for (int i = 0; i < PICO_NPINS; i++)
        logical[i] = (uint8_t) di_logical(&g_cfg, i, di_pub_level[i]);
    for (int g = 0; g < PICO_NGROUPS; g++)
        group_reset(&g_grp[g], &g_cfg, g, logical);
}

/* ---- manifest: the box's self-description, announced at every (re)connect
 * (next to publish_ident) and re-announced on any live label/desc/group
 * change. Per-item datapoints (house style; each fits the 128-byte frame):
 *   state/desc                      free-form description
 *   state/label/<n>                 pin role ("" = cleared; sent for any pin
 *                                   that is labeled OR configured, so a live
 *                                   relabel can't leave a stale value behind)
 *   state/group/<name>/pins         "2,3,4,5" ascending = published bit order
 *   state/group/<name>/settle_ms    chord window
 *   state/pins/in, state/pins/out   csv of configured input / output pins, so a
 *                                   UI renders EXACTLY the active DIO -- a pin
 *                                   turned off drops out here even though its
 *                                   last di/do datapoint lingers retained in
 *                                   dserv (which would otherwise show a ghost).
 * Consumers: extioconf decode, ess joystick bit-map, fleet page. */
static void publish_manifest(void)
{
    char leaf[48], nm[64], s[96]; uint8_t f[DSERV_MSG_LEN];

    dserv_state_name(&g_cfg, nm, sizeof nm, "desc");
    dserv_msg_string(f, nm, 0, g_cfg.desc);
    box_net_client_send(f, DSERV_MSG_LEN);

    int ki = 0, ko = 0; char in_csv[96], out_csv[96];
    in_csv[0] = out_csv[0] = '\0';
    for (int i = 0; i < PICO_NPINS; i++) {
        if (g_cfg.pin_mode[i] == 2 || g_cfg.pin_mode[i] == 3)
            ki += snprintf(in_csv + ki, sizeof in_csv - ki, "%s%d", ki ? "," : "", i);
        else if (g_cfg.pin_mode[i] == 1)
            ko += snprintf(out_csv + ko, sizeof out_csv - ko, "%s%d", ko ? "," : "", i);
    }
    dserv_state_name(&g_cfg, nm, sizeof nm, "pins/in");
    dserv_msg_string(f, nm, 0, in_csv);   box_net_client_send(f, DSERV_MSG_LEN);
    dserv_state_name(&g_cfg, nm, sizeof nm, "pins/out");
    dserv_msg_string(f, nm, 0, out_csv);  box_net_client_send(f, DSERV_MSG_LEN);

    /* special-function pins (mirror OUT / TTL sync IN); -1 = off, so disabling
     * updates the retained value instead of leaving a ghost. */
    dserv_state_name(&g_cfg, nm, sizeof nm, "obs_pin");
    dserv_msg_int(f, nm, 0, obs_mirror_enabled(&g_cfg) ? obs_mirror_pin(&g_cfg) : -1);
    box_net_client_send(f, DSERV_MSG_LEN);
    dserv_state_name(&g_cfg, nm, sizeof nm, "sync_pin");
    dserv_msg_int(f, nm, 0, sync_input_enabled(&g_cfg) ? sync_input_pin(&g_cfg) : -1);
    box_net_client_send(f, DSERV_MSG_LEN);

    for (int i = 0; i < PICO_NPINS; i++) {
        if (!g_cfg.pin_label[i][0] && !g_cfg.pin_mode[i]) continue;
        snprintf(leaf, sizeof leaf, "label/%d", i);
        dserv_state_name(&g_cfg, nm, sizeof nm, leaf);
        dserv_msg_string(f, nm, 0, g_cfg.pin_label[i]);
        box_net_client_send(f, DSERV_MSG_LEN);
    }

    for (int g = 0; g < PICO_NGROUPS; g++) {
        if (!g_cfg.group_pins[g]) continue;
        char gn[PICO_LABEL_MAX + 4];
        dserv_group_name(&g_cfg, g, gn, sizeof gn);
        dserv_pins_str(g_cfg.group_pins[g], s, sizeof s);
        snprintf(leaf, sizeof leaf, "group/%s/pins", gn);
        dserv_state_name(&g_cfg, nm, sizeof nm, leaf);
        dserv_msg_string(f, nm, 0, s);
        box_net_client_send(f, DSERV_MSG_LEN);
        snprintf(leaf, sizeof leaf, "group/%s/settle_ms", gn);
        dserv_state_name(&g_cfg, nm, sizeof nm, leaf);
        dserv_msg_int(f, nm, 0, g_cfg.group_settle_ms[g]);
        box_net_client_send(f, DSERV_MSG_LEN);
    }
}

/* Seed state/group/<name> at (re)connect with the settled bitmask, so a UI
 * shows the chord state immediately -- the group twin of publish_di_levels. */
static void publish_group_levels(void)
{
    for (int g = 0; g < PICO_NGROUPS; g++)
        if (g_cfg.group_pins[g]) publish_group(g, g_grp[g].cur, 0);
}

/* Connect burst: identity card + manifest + input/group seeds -- the one-shot,
 * retained datapoints a fresh UI needs (fw, pins/in, labels, di seeds).
 *
 * On USB these must NOT be fired-and-forgotten at the up==2 enumeration instant:
 * the host's usbio opens the data tty only on its ~2s discovery poll, so at
 * up==2 nobody is draining the CDC yet and the whole burst silently drops (edge
 * publishes like di/<n> land later, once the host reads -- which is why a box
 * would show di but no fw/pins). Gate on box_net_client_reading() (USB: DTR on
 * the data CDC = host has the port open; a plain FIFO-write probe FALSE-POSITIVES
 * because one 128B frame just buffers). Not reading -> report not-delivered so
 * the caller retries on the next heartbeat until it lands (self-terminating).
 * Eth is always reading once connected.
 *
 * `force` bypasses the DTR gate: DTR gating was abandoned once during bring-up
 * (some host/opener may not assert it), so as belt-and-suspenders the caller
 * FORCES a send every ~5s while still pending -- if the host is actually
 * reading, a forced burst lands even without DTR; if not, it best-effort drops
 * (di/heartbeat stay ungated, so this can never silence the box). */
static int announce_burst(int force)
{
#if defined(BOX_NET_USB) || defined(BOX_NET_DUAL) || defined(BOX_NET_BLE)
    if (!force && !box_net_client_reading()) return 0;   /* host not draining yet (CDC open / BLE subscribed) */
#else
    (void) force;
#endif
    publish_ident(); publish_manifest();
    publish_di_levels(); publish_group_levels();
    publish_ota_slotinfo();
    return 1;
}

/* ---- registration self-heal (socket transports; USB re-registers on its own timer) ----
 * dserv's config pushes ride a dserv->box connection created ONCE per %reg:
 * add_new_send_client() tries the connect-back a single time and NEVER retries.
 * So any race around a dserv restart -- our %reg processed while the box's old
 * server socket still held the dead connection, a registration command dropped
 * on the wire, dserv listening before its tables were live -- leaves the box
 * publishing happily but DEAF: no config pushes, no ess/in_obs, no clock sync.
 * The box-side signal is unambiguous: state client up, config server socket
 * empty. Re-register while that persists (idempotent on dserv: %reg replaces
 * the send-client, %match inserts into a dict). While healthy, re-send the
 * %match set every MATCH_REFRESH_MS (outside an obs) -- a lost match is
 * otherwise invisible: connection up, config flows, but in_obs never arrives. */
#define REG_RETRY_MS     5000
#define MATCH_REFRESH_MS 30000
static volatile int g_in_obs;           /* live ess/in_obs copy (sync frames)   */
#if !defined(BOX_NET_USB) && !defined(BOX_NET_BLE)  /* USB/BLE: host module / receiver owns forwarding */
static uint32_t g_reg_down_ms, g_match_fresh_ms;
static uint16_t g_rereg_count;

static void reg_watchdog_service(void)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (!box_net_server_up()) {
        g_match_fresh_ms = now;
        if (!g_reg_down_ms) { g_reg_down_ms = now; return; }
        if (now - g_reg_down_ms < REG_RETRY_MS) return;
        g_reg_down_ms = now;
        reg_request(1, 1);
        if (++g_rereg_count <= 3 || (g_rereg_count % 12) == 0)   /* first few, then 1/min */
            printf("reg: config link down -> re-registering (x%u)\n", g_rereg_count);
    } else {
        g_reg_down_ms = 0;
        if (g_rereg_count) { printf("reg: config link restored\n"); g_rereg_count = 0; }
        if (now - g_match_fresh_ms >= MATCH_REFRESH_MS) {
            g_match_fresh_ms = now;
            if (!g_in_obs) reg_request(0, 1);     /* refresh matches; never near a sync edge */
        }
    }
}
#endif /* !BOX_NET_USB && !BOX_NET_BLE */

/* ---- `phylink` diagnostic: watch the W6300 PHY link live (bench tool for the
 * auto-transport policy: shows autonegotiation settle time after power-on and
 * cable plug/unplug, debounce-free). `phylink` = one-shot, `phylink 1/0` =
 * periodic watcher (100ms sample, prints transitions + a 1s heartbeat). */
static volatile int g_phylink_watch;

static void phylink_service(void)
{
    static uint32_t next_ms, beat_ms;
    static int last = -3;
    if (!g_phylink_watch) { last = -3; return; }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((int32_t)(now - next_ms) < 0) return;
    next_ms = now + 100;
    int l = box_net_phy_link();
    if (l != last || now - beat_ms >= 1000) {
        printf("phy: %s (t=%lu ms)%s\n",
               l == 1 ? "link up" : l == 0 ? "link down" : l == -2 ? "no PHY" : "chip error",
               (unsigned long) now, l != last ? "  <- change" : "");
        last = l; beat_ms = now;
    }
}

/* ---- flash ops: core 1 owns g_cfg, so it snapshots; core 0 does the write ---- */
static void save_request(int erase_only)
{
    static save_req_t r;                /* ~1 KB since v13: static, off the 4KB core-1 stack
                                         * (cmd_exec's out[1024] is already the deep frame);
                                         * core 1 is single-threaded, so no reentrancy */
    r.erase_only = (uint8_t) erase_only;
    if (!erase_only) r.cfg = g_cfg;                 /* coherent: only this core writes g_cfg */
    if (!queue_try_add(&g_save_q, &r)) printf("flash busy, retry\n");
}

/* Reboot from core 1, letting any queued flash op land first (the wait covers
 * core 0's ~50ms erase; on non-copy_to_ram builds that op also parks this core,
 * which self-paces the same wait). */
static void box_reboot(int bootsel)
{
    absolute_time_t dl = make_timeout_time_ms(800);
    while (queue_get_level(&g_save_q) && absolute_time_diff_us(get_absolute_time(), dl) > 0)
        tight_loop_contents();
    sleep_ms(50);                                   /* let the result text drain */
    if (bootsel) reset_usb_boot(0, 0);
    else         watchdog_reboot(0, 0, 0);
}

/* ======================================================================== *
 *  STAGE-0 OTA: pull a firmware image over the dserv link -> scratch flash.
 *
 *  `cmd/ota/begin "<sha256hex> <size>"` (matched in on_frame BEFORE dispatch)
 *  arms a pull; the RT loop runs it (gated !in_obs) via box_net_get_binary,
 *  which streams the raw value through pico_ota_sink into scratch flash and
 *  hashes it. pico_ota.h owns the sink/sha/geometry; this file owns the two
 *  box-specific bits it can't: (1) flash writes must run on CORE 0 (core 1's
 *  flash_safe_execute is NOT_PERMITTED under ASSUME_CORE1_SAFE), so the
 *  pico_ota_flash_t hooks marshal each op to core 0 via a two-flag handshake;
 *  (2) publishing state/ota/{state,progress,result}. Delivery proof only --
 *  no partition table / boot switch yet (OTA.md Stage 1 adds A/B + TBYB).
 * ======================================================================== */
static pico_ota_t g_ota;                 /* ~320B: static, off the 4KB core-1 stack */
static volatile uint8_t  g_ota_pending;  /* on_frame -> ota_service_core1 */
static uint8_t  g_ota_sha[PICO_OTA_SHA_BYTES];
static uint32_t g_ota_size;
static char     g_ota_key[80];           /* datapoint the box pulls: <prefix>/ota/image */

/* core1 <-> core0 flash handshake: core 1's sink hook blocks here while core 0
 * (ota_flash_service_core0) does the actual erase/program. Single in-flight op;
 * core 1 is stalled in the pull for the whole transfer, so no queue needed. */
static struct {
    volatile uint32_t off;
    const uint8_t    *page;   /* NULL => erase the sector at off */
    volatile uint8_t  req;
    volatile uint8_t  done;
    volatile int8_t   rc;
} g_otaf;

static int ota_flash_submit(uint32_t off, const uint8_t *page)   /* core 1 */
{
    g_otaf.off = off; g_otaf.page = page; g_otaf.rc = 0; g_otaf.done = 0;
    __dmb();                                  /* publish payload before the request flag */
    g_otaf.req = 1;
    absolute_time_t dl = make_timeout_time_ms(2000);   /* core-0 stall guard (erase ~ms) */
    while (!g_otaf.done)
        if (absolute_time_diff_us(get_absolute_time(), dl) <= 0) return -1;
    __dmb();                                  /* read rc after done is observed */
    return g_otaf.rc;
}
static int ota_erase_hook(uint32_t off)                       { return ota_flash_submit(off, NULL); }
static int ota_program_hook(uint32_t off, const uint8_t *page){ return ota_flash_submit(off, page); }
static const pico_ota_flash_t g_ota_flash_ops = { ota_erase_hook, ota_program_hook };

/* ---- state/ota publish helpers (box -> dserv) ---- */
static void publish_ota_str(const char *leaf, const char *val)
{
    char nm[64]; uint8_t f[DSERV_MSG_LEN];
    dserv_state_name(&g_cfg, nm, sizeof nm, leaf);
    dserv_msg_string(f, nm, 0, val);
    box_net_client_send(f, DSERV_MSG_LEN);
}
/* RELIABLE publish: a single state send from the RX path (or right before the
 * arm reboot) can be dropped when the TX FIFO is momentarily full, so a
 * terminal OTA outcome (armed/ok/committed/fail) would never reach dserv even
 * though it happened. Re-send N times so at least one lands -- the ack survives
 * the same way (re-sent every frame). Cheap: only fires on outcome transitions. */
static void publish_ota_str_n(const char *leaf, const char *val, int n)
{
    for (int i = 0; i < n; i++) publish_ota_str(leaf, val);
}
static void publish_ota_progress(void)
{
    char nm[64]; uint8_t f[DSERV_MSG_LEN];
    dserv_state_name(&g_cfg, nm, sizeof nm, "ota/progress");
    dserv_msg_int(f, nm, 0, pico_ota_progress_pct(&g_ota));
    box_net_client_send(f, DSERV_MSG_LEN);
}

/* Parse `cmd/ota/begin` payload "<64 hex sha256> <decimal size>". 0 ok, -1. */
static int ota_begin_parse(const char *s, uint8_t sha[PICO_OTA_SHA_BYTES], uint32_t *size)
{
    while (*s == ' ') s++;
    if (pico_ota_parse_sha(s, sha) != 0) return -1;
    s += 64;
    while (*s == ' ') s++;
    if (*s < '0' || *s > '9') return -1;
    *size = (uint32_t) strtoul(s, NULL, 10);
    return *size ? 0 : -1;
}

/* box_net_bin_sink wrapper: core-1 hash+flash (in pico_ota_sink) + throttled
 * progress telemetry. Runs during the blocking pull; SN4 (state) and SN6 (pull)
 * sockets are independent and core 1 is single-threaded, so interleaving the
 * progress send between recvs is safe. */
static uint32_t g_ota_pub_at;
static int ota_sink(void *ud, const uint8_t *data, uint32_t len)
{
    int r = pico_ota_sink(ud, data, len);
    if (r == 0 && g_ota.received >= g_ota_pub_at) {
        g_ota_pub_at = g_ota.received + 32768;   /* ~5 updates over a 150KB image */
        publish_ota_progress();
    }
    return r;
}

/* Run one armed OTA to completion (core 1; blocks ~seconds). The full Stage-1
 * flow: resolve the INACTIVE A/B slot -> pull the image straight into it (streamed
 * through the core-0/core-1 flash handshake) -> sha-verify -> ARM a flash-update
 * (TBYB trial) reboot into it. The box then self-tests + buys (ota_buy_service),
 * or the watchdog reverts to the current slot (rollback). Gated !in_obs; the HW
 * watchdog is pet-unconditionally for the bounded RT stall (same escape as `wdt 0`). */
static void ota_service_core1(void)
{
    if (!g_ota_pending) return;
    g_ota_pending = 0;

    if (g_in_obs) {                               /* never mid-observation */
        publish_ota_str("ota/state", "fail");
        publish_ota_str("ota/result", "in_obs");
        printf("ota: refused -- in_obs\n");
        return;
    }

    /* Resolve the inactive slot to write into. No PT -> no A/B target -> refuse
     * (the box must be partition-migrated first; Stage-0 scratch is retired). */
    pico_ota_slot_probe(&g_slot);
    if (!g_slot.target_valid) {
        publish_ota_str("ota/state", "fail");
        publish_ota_str("ota/result", "no_slot");
        printf("ota: refused -- no inactive slot (pt=%d rc=%ld); box not partitioned\n",
               g_slot.pt_count, (long) g_slot.last_rc);
        return;
    }

    printf("ota: staging %u bytes -> slot part %d @0x%06lx (cap %lu) from %s\n",
           g_ota_size, g_slot.target_part, (unsigned long) g_slot.target_base,
           (unsigned long) g_slot.target_size, g_ota_key);
    uint8_t saved_gate = g_wdt_gate; g_wdt_gate = 0;   /* bounded stall: pet unconditionally */
    pico_ota_begin(&g_ota, &g_ota_flash_ops, g_slot.target_base, g_slot.target_size,
                   g_ota_sha, g_ota_size);
    g_ota_pub_at = 0;
    publish_ota_str("ota/state", pico_ota_state_str(g_ota.state));   /* staging */
    publish_ota_progress();

    int rc = box_net_get_binary(g_cfg.dserv_ip, dserv_cfg_port(&g_cfg),
                                g_ota_key, ota_sink, &g_ota);
    pico_ota_finish(&g_ota, rc);
    g_wdt_gate = saved_gate;

    publish_ota_progress();
    publish_ota_str("ota/state", pico_ota_state_str(g_ota.state));
    publish_ota_str("ota/result", g_ota.state == PICO_OTA_DONE_OK ? "ok" : pico_ota_err_str(g_ota.err));
    printf("ota: %s (%s) recv=%u/%u datalen_rc=%d\n",
           pico_ota_state_str(g_ota.state), pico_ota_err_str(g_ota.err),
           g_ota.received, g_ota_size, rc);

    /* Verified into the inactive slot -> ARM: flash-update reboot into it as a
     * TBYB trial. NO_RETURN, so this is the last thing we do. */
    if (g_ota.state == PICO_OTA_DONE_OK) {
        uint32_t update_base = (uint32_t) XIP_BASE + g_slot.target_base;
        publish_ota_str("ota/state", "armed");
        printf("ota: verified -> FLASH_UPDATE reboot into part %d @0x%08lx (1s)\n",
               g_slot.target_part, (unsigned long) update_base);
        pico_ota_arm_update(update_base, 1000);   /* NO_RETURN: reboots into the trial */
    }
}

/* ---- USB chunk delivery (OTA over USB-CDC) --------------------------------
 * An Ethernet box PULLS the image over a transient socket (box_net_get_binary);
 * a USB box has no socket, so the host PUSHES it as a stream of 'D' frames --
 * 128B frames (marker DSERV_OTA_CHAR, accepted by the framer) whose payload is
 * raw image bytes fed into the SAME pico_ota sink the eth pull uses. Strictly
 * sequential: seq_off must equal the sink cursor (g_ota.received). A per-frame
 * crc32 (== host pico_crc32) rejects a desync'd frame early; the whole-image
 * sha is the final gate. The box acks the contiguous cursor so the host paces +
 * resumes. cmd/ota/begin sets it up (RT loop), 'D' frames feed it, size-reached
 * verifies + arms -- same trial/buy/rollback machinery as eth. See OTA.md.
 *   'D' frame (128B): [0]='D' [1..4]=seq u32 [5..6]=len u16 [7..10]=crc32 u32
 *                     [11..127]=data (1..117 valid, rest zero) */
#if defined(BOX_NET_USB) || defined(BOX_NET_DUAL) || defined(BOX_USB_OTA_DOCKED)
/* Pure-USB build is always USB (box_net_is_usb() is a dual-only symbol); a dual
 * build decides at runtime by the strap-selected transport. The docked handheld
 * (BOX_USB_OTA_DOCKED) has no socket and radio 'D'-push is excluded, so any
 * cmd/ota/begin it sees can only have arrived over CDC1 -> always the USB push. */
#if defined(BOX_NET_USB) || defined(BOX_USB_OTA_DOCKED)
#  define OTA_IS_USB() 1
#else
#  define OTA_IS_USB() box_net_is_usb()
#endif
#define OTA_DF_OFF_SEQ    1
#define OTA_DF_OFF_LEN    5
#define OTA_DF_OFF_CRC    7
#define OTA_DF_OFF_DATA   11
#define OTA_DF_DATA_MAX   (DSERV_MSG_LEN - OTA_DF_OFF_DATA)   /* 117 */
#define OTA_ACK_EVERY     4096u          /* ack the contiguous cursor at least this often */
#define OTA_USB_TIMEOUT_US 10000000u     /* abort a stalled push after 10s of silence */

static volatile uint8_t  g_ota_usb_active;     /* receiving D-frames (begin..finish/abort) */
static volatile uint8_t  g_ota_usb_begin_req;  /* on_frame -> RT loop probes slot + pico_ota_begin */
static uint32_t          g_ota_usb_last_us;    /* last D-frame arrival (stall timeout) */
static uint32_t          g_ota_ack_at;         /* next `received` threshold to ack */

static void publish_ota_ack(uint32_t off)
{
    char nm[64]; uint8_t f[DSERV_MSG_LEN];
    dserv_state_name(&g_cfg, nm, sizeof nm, "ota/ack");
    dserv_msg_int(f, nm, 0, (int32_t) off);
    box_net_client_send(f, DSERV_MSG_LEN);
}

/* Finalize a completed USB push: verify sha + (on ok) arm the TBYB trial. Same
 * finish/arm as the eth path (ota_service_core1).
 *
 * The watchdog gate is NOT held off across finalize. The final flush is a single
 * page program (~ms, far under the 2s WDT), so a hang here SELF-RECOVERS via the
 * watchdog (boot=watchdog) instead of dead-consoling. Holding g_wdt_gate=0 across
 * finalize was what turned a rare finalize stall into a permanent wedge -- the
 * "tail-stall" the box exhibited was here, not at the mythical offset 143208
 * (which was only the last periodic ack). See wiznet-io/OTA.md "USB OTA". */
static void ota_usb_finalize(void)
{
    g_ota_usb_active = 0;
    pico_ota_finish(&g_ota, 0);
    publish_ota_progress();
    printf("ota(usb): %s (%s) recv=%u/%u\n", pico_ota_state_str(g_ota.state),
           pico_ota_err_str(g_ota.err), g_ota.received, g_ota_size);
    if (g_ota.state == PICO_OTA_DONE_OK) {
        uint32_t update_base = (uint32_t) XIP_BASE + g_slot.target_base;
        /* Reliable (n>1): this is the terminal state before a NO_RETURN reboot --
         * a dropped single send left dserv stuck at "staging" even on success. The
         * trial then publishes "committed" once it self-tests + buys. */
        publish_ota_str_n("ota/result", "ok", 6);
        publish_ota_str_n("ota/state", "armed", 6);
        printf("ota(usb): verified -> FLASH_UPDATE reboot into part %d @0x%08lx (1s)\n",
               g_slot.target_part, (unsigned long) update_base);
        pico_ota_arm_update(update_base, 1000);   /* NO_RETURN: reboots into the trial */
    } else {
        publish_ota_str_n("ota/result", pico_ota_err_str(g_ota.err), 6);
        publish_ota_str_n("ota/state", "fail", 6);
    }
}

/* One 'D' data frame (core 1 via on_frame). */
static void ota_data_frame(const uint8_t *frame)
{
    if (!g_ota_usb_active) return;                 /* stray/late frame, no active push */
    uint32_t seq, crc; uint16_t len;
    memcpy(&seq, frame + OTA_DF_OFF_SEQ, 4);
    memcpy(&len, frame + OTA_DF_OFF_LEN, 2);
    memcpy(&crc, frame + OTA_DF_OFF_CRC, 4);
    /* Any reject -> re-ack the cursor so the host resends from there (idempotent). */
    if (len == 0 || len > OTA_DF_DATA_MAX)               { publish_ota_ack(g_ota.received); return; }
    if (seq != g_ota.received)                           { publish_ota_ack(g_ota.received); return; }
    if (pico_crc32(frame + OTA_DF_OFF_DATA, len) != crc) { publish_ota_ack(g_ota.received); return; }

    uint8_t saved = g_wdt_gate; g_wdt_gate = 0;    /* the sink may flash-flush (bounded); pet unconditionally */
    int r = pico_ota_sink(&g_ota, frame + OTA_DF_OFF_DATA, len);
    g_wdt_gate = saved;
    g_ota_usb_last_us = (uint32_t) time_us_64();

    if (r != 0) {                                  /* flash/overflow -> abort */
        g_ota_usb_active = 0;
        pico_ota_finish(&g_ota, -1);
        publish_ota_str("ota/state", "fail");
        publish_ota_str("ota/result", pico_ota_err_str(g_ota.err));
        return;
    }
    if (g_ota.received >= g_ota_size)  { ota_usb_finalize(); return; }   /* all bytes -> verify + arm */
    if (g_ota.received >= g_ota_ack_at) {
        g_ota_ack_at = g_ota.received + OTA_ACK_EVERY;
        publish_ota_ack(g_ota.received);
    }
}

/* RT-loop service: honor a USB begin request (probe slot + pico_ota_begin, kept
 * OUT of on_frame) and abort a stalled push. */
static void ota_usb_service_core1(void)
{
    /* Announce the OTA COMMIT once, from core 1 (safe TinyUSB access), after the
     * TRIAL boot self-tests + buys. ota_buy_service_core0 sets g_ota_bought on
     * core 0 but must NOT touch the CDC itself. This is THE definitive "your OTA
     * worked" signal in dserv -- visible even when base and trial share a version
     * (so state/fw doesn't change). buy_rc!=0 => the trial failed to commit. */
    static uint8_t announced_commit;
    if (g_ota_bought && !announced_commit) {
        announced_commit = 1;
        if (g_ota_buy_rc == 0) { publish_ota_str_n("ota/result", "committed", 6);
                                 publish_ota_str_n("ota/state",  "committed", 6); }
        else                   { publish_ota_str_n("ota/result", "buy_failed", 6);
                                 publish_ota_str_n("ota/state",  "fail", 6); }
    }
    if (g_ota_usb_begin_req) {
        g_ota_usb_begin_req = 0;
        if (g_in_obs) {
            publish_ota_str("ota/state", "fail"); publish_ota_str("ota/result", "in_obs");
            printf("ota(usb): refused -- in_obs\n"); return;
        }
        pico_ota_slot_probe(&g_slot);
        if (!g_slot.target_valid) {
            publish_ota_str("ota/state", "fail"); publish_ota_str("ota/result", "no_slot");
            printf("ota(usb): refused -- no inactive slot; box not partitioned\n"); return;
        }
        pico_ota_begin(&g_ota, &g_ota_flash_ops, g_slot.target_base, g_slot.target_size,
                       g_ota_sha, g_ota_size);
        g_ota_ack_at      = OTA_ACK_EVERY;
        g_ota_usb_last_us = (uint32_t) time_us_64();
        dserv_framer_reset(&g_framer);             /* discard any stale partial before the D-stream */
        g_ota_usb_active  = 1;
        publish_ota_str("ota/state", pico_ota_state_str(g_ota.state));   /* staging */
        publish_ota_ack(0);                        /* host waits for this before streaming */
        printf("ota(usb): staging %u bytes -> slot part %d @0x%06lx (cap %lu)\n",
               g_ota_size, g_slot.target_part, (unsigned long) g_slot.target_base,
               (unsigned long) g_slot.target_size);
        return;
    }
    if (g_ota_usb_active &&
        (uint32_t)(time_us_64() - g_ota_usb_last_us) > OTA_USB_TIMEOUT_US) {
        g_ota_usb_active = 0;
        pico_ota_finish(&g_ota, -1);
        publish_ota_str("ota/state", "fail");
        publish_ota_str("ota/result", "timeout");
        printf("ota(usb): stalled -- aborted at %u/%u\n", g_ota.received, g_ota_size);
    }
}
#endif /* BOX_NET_USB || BOX_NET_DUAL || BOX_USB_OTA_DOCKED */

/* ---- config/cmd in: dispatch one 128B datapoint frame ---- */
static void on_frame(const uint8_t *frame, void *ud)
{
#if defined(BOX_NET_USB) || defined(BOX_NET_DUAL) || defined(BOX_USB_OTA_DOCKED)
    if (frame[0] == DSERV_OTA_CHAR) { ota_data_frame(frame); return; }   /* USB OTA data, not a datapoint */
#endif
    uint64_t now_box = time_us_64();   /* receipt time, for the sync anchor */
    pico_config_t *cfg = (pico_config_t *) ud;
    dserv_msg_t m;
    if (dserv_msg_parse(frame, &m) != 0) return;

    /* obs-period sync edge: re-align box->dserv clock. Both begin(1) and end(0)
     * edges are anchors (two per obs). m.timestamp = dserv time of the toggle.
     *
     * Anchor choice: with a TTL sync input wired (sync pin N), the rig host's
     * physical obs edge was already IRQ-latched here BEFORE this frame arrived
     * -- pair the frame's dserv timestamp with THAT box time and the transport
     * delay drops out of the error budget (the frame is just the timestamp's
     * courier). The edge must match this toggle's polarity and be fresh
     * (SYNC_EDGE_WINDOW_US), else fall back to frame-arrival time exactly as
     * before -- an unwired/broken TTL degrades gracefully, visibly via
     * state/sync/source. */
    if (dserv_msg_name_eq(&m, SYNC_DP)) {
        int obs = (int) dserv_msg_as_long(&m);
        g_in_obs = obs;                    /* gate for the %match refresh (never mid-obs) */
        uint64_t anchor_box = now_box;     /* sw fallback: frame receipt time */
        int hw = 0;
        if (pico_gpio_sync_pin >= 0) {
            uint64_t e = pico_gpio_sync_edge_get(obs);   /* rising for obs=1, falling for obs=0 */
            if (e && now_box - e < SYNC_EDGE_WINDOW_US) { anchor_box = e; hw = 1; }
        }
        box_clock_sync(&g_clock, m.timestamp, anchor_box, hw);  /* hw anchors also teach the rate */
        if (obs) g_obs_begin_us = anchor_box;  /* beginobs -> anchor for scheduled events
                                                * (hw: pulses land on the PHYSICAL obs timeline) */
        pico_gpio_obs_mirror(cfg, obs);    /* LED/scope: box's live copy of obs */
        { char onm[64]; uint8_t of[DSERV_MSG_LEN];   /* box's live obs state for UIs -- honest per-box
                                                      * (only updates while THIS box receives edges) */
          dserv_state_name(&g_cfg, onm, sizeof onm, "in_obs");
          dserv_msg_int(of, onm, m.timestamp, obs);
          box_net_client_send(of, DSERV_MSG_LEN); }
        publish_sync(m.timestamp, anchor_box, g_clock.offset_us,
                     hw, hw ? (int64_t)(now_box - anchor_box) : -1);
        DBG("sync: obs=%d dserv=%llu box=%llu off=%lld %s\n", obs,
            (unsigned long long) m.timestamp,
            (unsigned long long) anchor_box, (long long) g_clock.offset_us,
            hw ? "hw" : "sw");
        return;
    }

    /* OTA arm: <prefix>/cmd/ota/begin -- intercepted here (dserv_dispatch's
     * cmd router doesn't know it) so the RT loop can run the transfer. Sets a
     * flag only; the pull itself is deferred to ota_service_core1 (blocking). */
    {
        char oname[80];
        int pl = dserv_cfg_prefix(cfg, oname, sizeof oname);
        if (pl > 0 && pl < (int)(sizeof oname) - 16) {
            snprintf(oname + pl, sizeof oname - (size_t) pl, "/cmd/ota/probe");
            if (dserv_msg_name_eq(&m, oname)) { g_slot_probe_req = 1; return; }  /* on-demand slot probe */
            snprintf(oname + pl, sizeof oname - (size_t) pl, "/cmd/ota/arm");
            if (dserv_msg_name_eq(&m, oname)) { g_ota_arm_req = 1; return; }      /* TBYB flash-update reboot */
            snprintf(oname + pl, sizeof oname - (size_t) pl, "/cmd/ota/begin");
            if (dserv_msg_name_eq(&m, oname)) {
                char val[96]; dserv_msg_copy_cstr(&m, val, sizeof val);
                if (ota_begin_parse(val, g_ota_sha, &g_ota_size) == 0) {
#if defined(BOX_NET_USB) || defined(BOX_NET_DUAL) || defined(BOX_USB_OTA_DOCKED)
                    if (OTA_IS_USB()) {              /* no socket to pull over -> host pushes 'D' frames */
                        g_ota_usb_begin_req = 1;     /* RT loop probes the slot + pico_ota_begin */
                        printf("ota: armed (%u bytes, USB push)\n", g_ota_size);
                        return;
                    }
#endif
                    snprintf(oname + pl, sizeof oname - (size_t) pl, "/ota/image");
                    strncpy(g_ota_key, oname, sizeof g_ota_key - 1);
                    g_ota_key[sizeof g_ota_key - 1] = '\0';
                    g_ota_pending = 1;
                    printf("ota: armed (%u bytes, pull %s)\n", g_ota_size, g_ota_key);
                } else {
                    printf("ota: bad begin payload '%s'\n", val);
                    publish_ota_str("ota/state", "fail");
                    publish_ota_str("ota/result", "bad_args");
                }
                return;
            }
        }
    }

    gpio_cmd_t cmd;
    cfg_result_t r = dserv_dispatch(cfg, &m, &cmd);
    DBG("dp: %.*s -> %s\n", (int) m.namelen, m.name, dserv_cfg_result_str(r));

#ifdef BOX_BLE
    /* Not our name (CFG_NONE) = a frame for a box BEHIND us -- the handheld.
     * extioconf forwards the handheld's config and cmd keys down our one data
     * pipe once it auto-discovers the relayed telemetry; we ferry them onward. */
    if (r == CFG_NONE) { box_ble_pipe_forward(frame); return; }
#endif

    if (cmd.op == GPIO_OP_SCHED_PULSE) {                /* do/<n>/at -> pulse + timer at beginobs+delta */
        uint32_t w = cfg->do_pulse_us[cmd.pin] ? cfg->do_pulse_us[cmd.pin] : 1000;
        sched_arm(cmd.pin, cmd.pin, w, cmd.value);
    }
    else if (cmd.op == GPIO_OP_SCHED_TIMER) {           /* timer/<n>/at -> notify-only at beginobs+delta */
        sched_arm(0xFF, cmd.pin, 0, cmd.value);
    }
    else if (cmd.op != GPIO_OP_NONE) {
        pico_gpio_exec(cfg, &cmd);
        uint64_t t_act = time_us_64();   /* pin has just moved -> its dserv time */
        if (cmd.op == GPIO_OP_SET)
            publish_do(cmd.pin, (uint8_t) cmd.value, event_stamp(t_act));  /* actual readback */
    }
    else if (r == CFG_PIN_MODE || r == CFG_OBS_PIN || r == CFG_SYNC_PIN) {
        pico_gpio_apply_config(cfg); groups_reset_all();
        publish_manifest();   /* active-pin set OR obs/sync pin changed -> re-announce */
    }
    else if (r == CFG_GROUP)     { groups_reset_all(); publish_manifest(); }
    else if (r == CFG_LABEL || r == CFG_DESC)          publish_manifest();
#ifdef BOX_BLE
    else if (r == CFG_PIPE_EN)   /* ble/pipe datapoint: apply live (remote `ble pipe 1|0`
                                  * over dserv -- no console); `cmd/save` persists (v16) */
        box_ble_request(cfg->pipe_en ? BOX_BLE_REQ_PIPE_ON : BOX_BLE_REQ_PIPE_OFF);
    else if (r == CFG_BLE_PAIR) {   /* cmd/ble/pair <secs>: remote pairing window (== console `ble pair`).
                                     * Window var is one volatile word -> core-safe to set from core 1. */
        uint32_t secs = (uint32_t) dserv_msg_as_long(&m);
        box_ble_pair_window(secs);
        printf("ble: pairing window %lus (remote); a NEW handheld will be adopted + bonded\n",
               (unsigned long) secs);
    }
    else if (r == CFG_BLE_FORGET)   /* cmd/ble/forget: clear bonds (== console `ble forget`; core-0 db work) */
        box_ble_request(BOX_BLE_REQ_FORGET);
#endif
    else if (r == CFG_SAVE)      save_request(0);       /* core 0 writes; prints flash ok/FAIL */
    else if (r == CFG_REBOOT)    box_reboot(0);
    else if (r == CFG_BOOTSEL) {   /* require a truthy value: a bare `set cmd/bootsel 0`
                                    * (or a stale re-broadcast) must NOT strand the box in
                                    * BOOTSEL -- it drops off dserv until physically reflashed,
                                    * a far worse footgun than reboot/save. The typed console
                                    * `bootsel` (CLI_BOOTSEL below) stays unconditional. */
        if (dserv_msg_as_long(&m)) { printf("entering BOOTSEL\n"); box_reboot(1); }
        else printf("cmd/bootsel: value 0 ignored (write 1 to enter BOOTSEL)\n");
    }
    else if (r == CFG_FACTORY)   { save_request(1); memset(cfg, 0, sizeof *cfg);
                                   pico_gpio_apply_config(cfg); printf("factory erased\n"); }
}

/* ---- console line execution (core 1: it owns g_cfg, the pins, the net) ---- */
static void cmd_exec(const char *line)
{
    if (!strcmp(line, "ip")) {                 /* live address (DHCP lease or static) */
        uint8_t bip[4]; box_net_local_ip(bip);
        printf("ip %u.%u.%u.%u\n", bip[0], bip[1], bip[2], bip[3]);
        return;
    }
    if (!strcmp(line, "debug 1") || !strcmp(line, "debug 0")) {   /* live hot-path log toggle */
        g_log_verbose = (line[6] == '1');
        printf("debug log %s\n", g_log_verbose ? "on" : "off");
        return;
    }
    if (!strcmp(line, "txstats")) {            /* USB data-CDC publish accounting */
        printf("tx ok=%lu wait=%lu drop=%lu notready=%lu\n",
               (unsigned long) box_usb_tx_ok, (unsigned long) box_usb_tx_wait,
               (unsigned long) box_usb_tx_drop, (unsigned long) box_usb_tx_notready);
        return;
    }
    if (!strcmp(line, "wdt 0") || !strcmp(line, "wdt 1")) {   /* watchdog core-1 gate (bench escape) */
        g_wdt_gate = (line[4] == '1');
        printf("watchdog: %s\n", g_wdt_gate
               ? "core-1 gated (either-core wedge -> self-reboot)"
               : "pet-always (core-1 gate OFF -- bench mode, core-0 wedge still reboots)");
        return;
    }
    if (!strcmp(line, "wdt test")) {           /* validate the trip: wedge THIS core for real */
        printf("watchdog test: wedging core 1 now -> expect self-reboot in ~%dms,\n"
               "  then a '[boot=watchdog]' greeting when the console re-attaches\n", BOX_WDT_MS);
        /* A genuine wedge: we never return to the loop top, so the heartbeat
         * stops and core 0 stops petting. But keep the CONSOLE plumbing alive
         * while we die -- tud_task + the CDC0 ferry -- or the message above
         * sits in the ring and never reaches a USB terminal. */
        for (;;) {
#ifdef BOX_CONSOLE_TUSB
            tud_task();
            box_console_cdc0_ferry();
#endif
            tight_loop_contents();
        }
    }
    if (!strncmp(line, "phylink", 7)) {        /* PHY link diagnostic (bench tool) */
        if      (!strcmp(line, "phylink 1")) { g_phylink_watch = 1; printf("phylink watch on (100ms samples)\n"); }
        else if (!strcmp(line, "phylink 0")) { g_phylink_watch = 0; printf("phylink watch off\n"); }
        else {
            int l = box_net_phy_link();
            printf("phy: %s\n", l == 1 ? "link up" : l == 0 ? "link down" :
                                l == -2 ? "no PHY on this transport" : "chip error");
        }
        return;
    }
#if defined(BOX_BLE) || defined(BOX_NET_BLE)
    /* Runtime radio commands -> one-shot requests consumed by core 0 (the
     * radio's core); replies print from there via the log ring. `ble enable
     * 0|1` is NOT intercepted: it's persisted config, pico_cli handles it --
     * but a re-typed enable ALSO pokes RETRY (no return!) so a bring-up that
     * was skipped after a watchdog boot can be re-armed from the console. */
    if (!strcmp(line, "ble"))        { box_ble_request(BOX_BLE_REQ_STATUS);   return; }
    if (!strcmp(line, "ble enable 1")) box_ble_request(BOX_BLE_REQ_RETRY);    /* falls through to pico_cli */
#endif
#ifdef BOX_BLE
    if (!strcmp(line, "ble scan 1")) { box_ble_request(BOX_BLE_REQ_SCAN_ON);  return; }
    if (!strcmp(line, "ble scan 0")) { box_ble_request(BOX_BLE_REQ_SCAN_OFF); return; }
    /* `ble pipe 1|0`: fire the live request AND fall through to pico_cli, which
     * sets cfg->pipe_en (persist v16) -- `save` then makes the relay auto-arm
     * at every boot (box_ble_service one-shot). Mirrors the `ble enable` split. */
    if (!strcmp(line, "ble pipe 1")) box_ble_request(BOX_BLE_REQ_PIPE_ON);
    if (!strcmp(line, "ble pipe 0")) box_ble_request(BOX_BLE_REQ_PIPE_OFF);
    { int secs;                                    /* `ble pair <secs>`: open a pairing window */
      if (sscanf(line, "ble pair %d", &secs) == 1 && secs > 0) {
          box_ble_pair_window((uint32_t) secs);    /* window var is core-safe (one volatile word) */
          printf("ble: pairing window open %ds -- a NEW handheld will be adopted + bonded\n", secs);
          return;
      } }
    if (!strcmp(line, "ble forget")) { box_ble_request(BOX_BLE_REQ_FORGET); return; }   /* clear bonds */
    if (!strcmp(line, "ble bonds"))  { box_ble_request(BOX_BLE_REQ_BONDS);  return; }   /* list bonds */
#endif
#ifdef BOX_STATUS_LED
    if (!strncmp(line, "led", 3) && (line[3] == '\0' || line[3] == ' ')) {   /* bench: force LED color/off/auto */
        status_led_cli(line[3] ? line + 4 : "");
        return;
    }
#endif
#ifdef BOX_NET_DUAL
    if (!strcmp(line, "mode")) {               /* live status; `mode <x>` (with arg) sets policy */
        printf("mode=%s active=%s%s\n", dserv_xmode_str(g_cfg.transport_mode),
               box_net_is_usb() ? "usb" : "eth",
               g_auto_sense ? " (auto: sensing for eth link)" : "");
        return;
    }
#endif
    char out[1024]; gpio_cmd_t cmd;   /* fits the full help (~419 B) + a many-pin `show`; 256 truncated both */
    cli_action_t act = pico_cli_exec(&g_cfg, line, out, sizeof out, &cmd);
    fputs(out, stdout);
    if (!strcmp(line, "show")) {       /* live status trailer: transport / boot / uptime / fw */
        printf("  transport=%s%s boot=%s up=%lus fw=%s\n",
#ifdef BOX_NET_DUAL
               box_net_is_usb() ? "usb" : "eth",
               g_auto_sense ? " (auto: sensing for eth link)" : "",
#else
               box_net_backend_name(), "",
#endif
               g_boot_reason, (unsigned long)(time_us_64() / 1000000u), BOX_FW_VERSION);
#ifdef BOX_BLE
        printf("  ble=%s%s pipe=%s\n", box_ble_state_str(),
               box_ble_scanning ? " (scanning)" : "", box_pipe_state_str());
#endif
#ifdef BOX_NET_BLE
        printf("  ble=%s pipe=%s\n", box_ble_state_str(), box_ble_link ? "UP" : "down");
#endif
        printf("  persist=%s magic=%08lx ver=%u len=%u (want %08x v<=%u)\n",
               flash_load_diag_rc == 0 ? "loaded" : flash_load_diag_rc == 99 ? "never-ran" : "FAILED",
               (unsigned long) flash_load_diag_magic, flash_load_diag_ver, flash_load_diag_len,
               PICO_PERSIST_MAGIC, PICO_PERSIST_VERSION);
#ifdef BOX_BLE_BREADCRUMBS
        if (g_ble_crumb)
            printf("  bledbg: last-run radio crumb=0x%08lx\n", (unsigned long) g_ble_crumb);
#endif
    }
    if (act == CLI_GPIO)         pico_gpio_exec(&g_cfg, &cmd);   /* drive the pin (console command) */
    else if (act == CLI_SAVE)    save_request(0);
    else if (act == CLI_FACTORY) { save_request(1); memset(&g_cfg, 0, sizeof g_cfg);
                                   pico_gpio_apply_config(&g_cfg); printf("erased\n"); }
    else if (act == CLI_REBOOT)  box_reboot(0);
    else if (act == CLI_BOOTSEL) box_reboot(1);
    else if (act == CLI_PIN)   { pico_gpio_apply_config(&g_cfg); groups_reset_all(); publish_manifest(); }
    else if (act == CLI_GROUP) { groups_reset_all(); publish_manifest(); }  /* label/group/desc changed */
}

static int have_dserv_target(void)
{
#if defined(BOX_NET_USB) || defined(BOX_NET_BLE)
    return 1;   /* USB/BLE: the host module / relaying receiver is always the target -- no IP */
#elif defined(BOX_NET_DUAL)
    if (box_net_is_usb()) return 1;   /* USB mode: host module is always the target */
    return g_cfg.dserv_ip[0] || g_cfg.dserv_ip[1] || g_cfg.dserv_ip[2] || g_cfg.dserv_ip[3];
#else
    return g_cfg.dserv_ip[0] || g_cfg.dserv_ip[1] || g_cfg.dserv_ip[2] || g_cfg.dserv_ip[3];
#endif
}

/* Refresh the OLED status snapshot (core 1, 5 Hz; ~us of volatile stores). */
static void status_update(int up)
{
    static uint32_t next_ms;
    if (!g_cfg.oled_en) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((int32_t)(now - next_ms) < 0) return;
    next_ms = now + 200;
    uint8_t ip[4]; box_net_local_ip(ip);
    for (int i = 0; i < 4; i++) g_stat.ip[i] = ip[i];
#if defined(BOX_NET_USB)
    g_stat.usb = 1; g_stat.sensing = 0;
#elif defined(BOX_NET_DUAL)
    g_stat.usb = (uint8_t) box_net_is_usb();
    g_stat.sensing = g_auto_sense;
#else
    g_stat.usb = 0; g_stat.sensing = 0;
#endif
    g_stat.cli_up = up > 0;
    g_stat.srv_up = box_net_server_up() == 1;
    g_stat.obs = (uint8_t) g_in_obs;
    int n = 0;
    for (int i = 0; i < PICO_NPINS && n < 4; i++)
        if (g_cfg.pin_mode[i] == 2 || g_cfg.pin_mode[i] == 3) {   /* in / in_pullup */
            uint8_t lvl = di_active_low(&g_cfg, i) ? !di_pub_level[i] : di_pub_level[i];
            g_stat.di_pins[n] = (uint8_t) i;
            if (lvl) g_stat.di_lvls |= (uint8_t)(1 << n); else g_stat.di_lvls &= (uint8_t) ~(1 << n);
            n++;
        }
    g_stat.di_n = (uint8_t) n;
}

#ifdef BOX_NET_DUAL
/* ---- transport auto-selection (GP28 strap open + `mode auto`, the default) ----
 * Boot USB immediately (always works: CDCs enumerate, console+data live), bring
 * the W6300 up lazily WITHOUT the vendored PHY-link wait, then sense
 * CW_GET_PHYLINK from the loop, debounced -- the PHY needs 1-3s of
 * autonegotiation after reset, which is exactly what the old boot-instant probe
 * missed. AUTO_DEBOUNCE consecutive up-reads -> swap the vtable to Ethernet
 * between passes (transport is single-owner on this core: no coordination),
 * start DHCP, and let the up==2 path self-register. No link inside the boot
 * window -> stay USB, and keep sensing at a slow cadence ONLY while no USB host
 * is mounted: a wall-powered box upgrades whenever a cable shows up, but an
 * active USB session is never yanked out from under dserv. No auto-downgrade
 * ever -- a mid-session cable pull is an Ethernet outage, not a mode change. */
#define AUTO_SAMPLE_MS   100
#define AUTO_IDLE_MS     2000
#define AUTO_DEBOUNCE    5
#define AUTO_WINDOW_MS   10000
static uint8_t  g_auto_ups;             /* (g_auto_sense lives with the top-of-file globals) */
static uint32_t g_auto_next_ms, g_auto_window_end_ms;

static void auto_sense_service(void)
{
    if (!g_auto_sense) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((int32_t)(now - g_auto_next_ms) < 0) return;
    int in_window = (int32_t)(now - g_auto_window_end_ms) < 0;
    if (!in_window && box_net_usb_vt.server_up()) {   /* tud_ready: live USB host -> don't switch under it */
        g_auto_ups = 0;
        g_auto_next_ms = now + AUTO_IDLE_MS;
        return;
    }
    int l = box_net_phy_link();
    if (l < 0) {                                      /* chip absent/dead: resolve usb for good */
        printf("auto: W6300 not responding -> usb\n");
        g_auto_sense = 0;
        return;
    }
    g_auto_ups = (l == 1) ? (uint8_t)(g_auto_ups + 1) : 0;
    /* tight cadence in the boot window or once a link is showing; slow when idle */
    g_auto_next_ms = now + ((in_window || g_auto_ups) ? AUTO_SAMPLE_MS : AUTO_IDLE_MS);
    if (g_auto_ups < AUTO_DEBOUNCE) return;

    printf("auto: eth link up (t=%lu ms) -> switching usb->eth\n", (unsigned long) now);
    box_net_select(XPORT_ETH, &g_cfg);
    if (box_net_init(&g_cfg) != 0) {                  /* can't happen if phy_link worked; belt+suspenders */
        printf("auto: eth init failed -> staying usb\n");
        box_net_select(XPORT_USB, &g_cfg);
        g_auto_sense = 0;
        return;
    }
    dserv_framer_reset(&g_framer);
    reg_reset();                 /* abandon any USB-mode sequence; eth up==2 re-registers */
    g_auto_sense = 0;
}
#endif /* BOX_NET_DUAL */

/* The RT core. Everything here must stay us-bounded per pass: no console
 * sinks (printf goes to the ring), no I2C, no flash -- see the file header. */
static void rt_main(void)
{
#if !PICO_FLASH_ASSUME_CORE1_SAFE
    /* Lockout victim: core 0 flash ops park us safely. copy_to_ram builds set
     * the flag to 1 instead (this core never touches flash/XIP, so saves don't
     * park it at all). MUST be `#if !`, NOT `#ifndef`: the SDK's pico/flash.h
     * defines the macro TO 0 as a PICO_CONFIG default, so `#ifndef` silently
     * skipped this registration on every XIP build -> every save returned
     * NOT_PERMITTED (-4). Latent since the 2026-07-07 Stage-2 conversion
     * (which retired the lockout path on all then-shipping targets); exposed
     * 2026-07-17 by the radio builds ("flash FAIL" on the first thingplus
     * handheld save). */
    flash_safe_execute_core_init();
#endif
    box_alarm_pool = alarm_pool_create_with_unused_hardware_alarm(24);  /* pulse/sched/DHCP IRQs -> this core;
                                                                         * 24 = 8 sched + many pulses + DHCP tick */
#ifdef BOX_NET_DUAL
    box_net_dual_usb_init();            /* tusb_init HERE: the USB IRQ must land on core 1 */
#endif
    pico_gpio_apply_config(&g_cfg);     /* HERE: DI edge IRQs must land on core 1 */
    groups_reset_all();                 /* chord groups start from the seeded levels */
    if (box_net_init(&g_cfg) != 0) printf("net init FAILED\n");
    /* NOTE: the bootrom PT/boot probe is DELIBERATELY NOT run here. With a real
     * partition table present, rom_get_uf2_target_partition wedged core 1 during
     * a slot boot, and running it before g_core1_ready (below, which arms the
     * watchdog) made that an unrecoverable early-boot hang. It is now on-demand
     * only (cmd/ota/probe -> ota_slot_service_core1), so boot ALWAYS completes
     * reachable and a misbehaving ROM call is caught by the armed watchdog. */
    dserv_framer_reset(&g_framer);
    box_clock_reset(&g_clock);
#ifdef BOX_NET_DUAL
    g_auto_window_end_ms = to_ms_since_boot(get_absolute_time()) + AUTO_WINDOW_MS;
#endif
    g_core1_ready = 1;                  /* core 0 announces the ready banner */

    uint32_t last_hb = 0;
    uint8_t  announce_pending = 0;      /* connect burst deferred until the host reads (USB tty race) */
    uint8_t  announce_hb = 0;           /* heartbeats since defer -> forces a DTR-bypass retry every ~5s */
#if defined(BOX_NET_USB) || defined(BOX_NET_DUAL) || defined(BOX_NET_BLE)
    uint8_t  prev_reading = 0;          /* DTR/subscription edge: far end (re)attached -> re-announce */
    uint16_t announce_backstop = 0;     /* ~60s re-announce backstop if a reconnect edge is missed */
#endif
#if defined(BOX_NET_DUAL) || defined(BOX_USB_FORWARD_REGISTER)
    uint32_t reg_tick = 0;              /* delayed USB autoregistration cadence */
#endif
    while (1) {
        g_rt_beat++;               /* heartbeat: core 0 pets the HW watchdog only while this advances */
#if defined(BOX_BLE) || defined(BOX_NET_BLE)
        if (box_ble_hold_req) {    /* radio bring-up: park so core 0 owns the chip solo (IRQs stay live) */
            box_ble_held = 1;
            while (box_ble_hold_req) tight_loop_contents();
            box_ble_held = 0;
        }
#endif
#ifdef BOX_NET_DUAL
        box_net_dual_usb_task();   /* service TinyUSB every pass: CDC0 console always, CDC1 data in USB mode */
#endif
#ifdef BOX_CONSOLE_TUSB
        box_console_cdc0_ferry();  /* CDC0 <-> core-0 console rings, fire-and-forget */
        if (box_console_attach_evt) {   /* terminal just attached: it missed all pre-DTR output */
            box_console_attach_evt = 0;
            printf("[extio %s %s | transport=%s | boot=%s | up %lus | 'help' for commands]\n",
                   dserv_cfg_name(&g_cfg), BOX_FW_VERSION,
#ifdef BOX_NET_DUAL
                   box_net_is_usb() ? "usb" : "eth",
#else
                   box_net_backend_name(),
#endif
                   g_boot_reason, (unsigned long)(time_us_64() / 1000000u));
        }
#endif
        box_net_poll();
#ifdef BOX_NET_DUAL
        auto_sense_service();      /* auto policy: watch the PHY, upgrade usb->eth on link */
#endif
        phylink_service();         /* `phylink 1` bench watcher (no-op when off) */

        /* config/cmd in (dserv -> box) */
        int n = box_net_server_poll(CFG_PORT, g_rxbuf, RXBUF_SIZE);
        if (n < 0)      dserv_framer_reset(&g_framer);
        else if (n > 0) dserv_framer_feed(&g_framer, g_rxbuf, (uint32_t) n, on_frame, &g_cfg);

#ifdef BOX_USB_OTA_DOCKED
        /* Docked USB-OTA ingest (handheld): when a host opens the data CDC (CDC1)
         * it can push a firmware OTA the same '(D)'-frame way the USB box does.
         * A SECOND framer feeds the SAME on_frame; tinyusb is on this core, so no
         * cross-core hop. Idle cost = one tud_cdc_n_available() when undocked. */
        if (tud_cdc_n_connected(BOX_USB_CDC_DATA) && tud_cdc_n_available(BOX_USB_CDC_DATA)) {
            uint32_t un = tud_cdc_n_read(BOX_USB_CDC_DATA, g_usbrxbuf, RXBUF_SIZE);
            if (un) dserv_framer_feed(&g_usb_framer, g_usbrxbuf, un, on_frame, &g_cfg);
        }
#endif

        char cline[CLI_LINE_MAX];  /* console lines assembled by core 0 */
        while (queue_try_remove(&g_cmd_q, cline)) cmd_exec(cline);

        reg_service();             /* advance any in-flight registration, one step per pass */

        /* state out (box -> dserv), once a target is configured */
        if (!have_dserv_target()) {
            status_update(0);                  /* OLED snapshot still refreshes unconfigured */
            g_net_up = g_srv_up = 0;
        } else {
            uint16_t port = dserv_cfg_port(&g_cfg);
            int up = box_net_client_service(g_cfg.dserv_ip, port);
            status_update(up);
            g_net_up = (up >= 1);                       /* dserv state-client connected (box->dserv) */
            g_srv_up = box_net_server_up() == 1;        /* dserv connected back (registered/reachable) */
#if defined(BOX_NET_USB) || defined(BOX_NET_DUAL) || defined(BOX_NET_BLE)
            /* Host re-opened the data CDC (DTR 0->1) = dserv/usbio reconnected
             * after a restart. A USB box never de-enumerates, so there's no
             * up==2 to re-fire on -- this DTR edge is the USB equivalent of
             * eth's reconnect burst, so the manifest self-heals a dserv restart
             * with no reboot. (Eth via the vtable returns 1 constant -> one
             * boot edge, harmless; eth's real reconnect goes through up==2.) */
            int reading = box_net_client_reading();
            if (reading && !prev_reading) { announce_pending = 1; announce_hb = 0; }
            prev_reading = (uint8_t) reading;
#endif
            if (up == 2) {                     /* connect burst; USB may need the host tty first */
                announce_hb = 0;
                if (!announce_burst(0)) announce_pending = 1;  /* -> retried on the heartbeat until it lands */
#ifdef BOX_BLE
                g_ble_tlm_lb = g_ble_tlm_le = g_ble_tlm_lp = -1;   /* re-publish state/ble/* over the fresh link */
#endif
            }
#if defined(BOX_NET_DUAL)
            if (up == 2 && !box_net_is_usb()) reg_request(1, 0);  /* Eth: reg on connect; USB: delayed autoreg below */
            if (up >= 1 && !box_net_is_usb()) reg_watchdog_service();
#elif !defined(BOX_USB_FORWARD_REGISTER) && !defined(BOX_NET_BLE)
            if (up == 2) reg_request(1, 0);    /* wired/wifi: register on connect. (USB autoreg emits later,
                                                * NOT on connect -- writing CDC1 before macOS finishes creating
                                                * its tty drops the data port; the delayed periodic below is safe.
                                                * BLE: no registration at all -- the receiver relays, the host
                                                * module owns forwarding.) */
#endif
#if !defined(BOX_NET_USB) && !defined(BOX_NET_DUAL) && !defined(BOX_NET_BLE)
            if (up >= 1) reg_watchdog_service();   /* wired/wifi: heal lost registrations */
#endif
            if (up >= 1) {
                ota_service_core1();       /* run an armed OTA pull (blocks; gated !in_obs) */
                ota_slot_service_core1();  /* run an on-demand bootrom PT/boot probe */
                ota_arm_service_core1();   /* cmd/ota/arm -> flash-update reboot into inactive slot */
#if defined(BOX_NET_USB) || defined(BOX_NET_DUAL) || defined(BOX_USB_OTA_DOCKED)
                ota_usb_service_core1();   /* USB push: begin-setup + stall timeout (data arrives via on_frame) */
#endif
                di_event_t e;
                while (pico_gpio_poll_di(&g_cfg, &e)) {   /* debounced edges */
                    int lvl = di_logical(&g_cfg, e.pin, e.level);
                    int quiet = 0;                     /* member of a quiet group? */
                    for (int g = 0; g < PICO_NGROUPS; g++)
                        if (g_cfg.group_pins[g] &&
                            group_feed(&g_grp[g], &g_cfg, g, e.pin, lvl, e.t_us) &&
                            g_cfg.group_quiet[g]) quiet = 1;
                    if (!quiet) publish_di(&e);
                }
                group_out_t go[2];                     /* settle windows expire between edges */
                uint64_t gnow = time_us_64();
                for (int g = 0; g < PICO_NGROUPS; g++) {
                    if (!g_cfg.group_pins[g]) continue;
                    int gn = group_poll(&g_grp[g], &g_cfg, g, gnow, go);
                    for (int k = 0; k < gn; k++) publish_group(g, go[k].bits, go[k].t_us);
                }
                ain_sample_t s;                        /* acquired on core 0, stamped here */
                while (queue_try_remove(&g_ain_q, &s)) publish_ain(s.ch, s.val, s.t_us);
#ifdef BOX_BLE
                { uint8_t pf[DSERV_MSG_LEN];           /* handheld frames from the radio (core 0) ->
                                                        * ts rewritten hh->dserv at the boundary, then
                                                        * straight up our own transport; the name
                                                        * inside each frame keeps the boxes distinct */
                  while (box_ble_pipe_pop_rx(pf)) { pipe_rewrite_ts(pf); box_net_client_send(pf, DSERV_MSG_LEN); } }
                echo_estimator_service();              /* min-RTT anchors -> g_hh_clock (feeds pipe_rewrite_ts) */
                /* echo-sync probe telemetry (Increment 1, measure-only): core 0
                 * samples RTT + implied offset into volatiles, we publish on
                 * change so `dservctl listen extio/<rx>/state/echo/*` captures the
                 * distribution. Read once each; a rare int64 tear is harmless for
                 * verification (the console printf is the cross-check). */
                { static uint32_t last_echo_seq;
                  if (g_echo_seq != last_echo_seq) {
                      last_echo_seq = g_echo_seq;
                      char en[64]; uint8_t ef[DSERV_MSG_LEN];
                      dserv_state_name(&g_cfg, en, sizeof en, "echo/rtt_us");
                      dserv_msg_int(ef, en, 0, (int32_t) g_echo_rtt_us);  box_net_client_send(ef, DSERV_MSG_LEN);
                      dserv_state_name(&g_cfg, en, sizeof en, "echo/offset_us");
                      dserv_msg_int64(ef, en, 0, g_echo_off_us);          box_net_client_send(ef, DSERV_MSG_LEN);
                  } }
                /* bonding telemetry for the fleet page: publish on change. bonds
                 * comes from the core-0 mirror (btstack is core-0 only); pairing
                 * = seconds left in the `ble pair` window (0 = closed). */
                { int nb = g_ble_bonds, ne = pipe_encrypted ? 1 : 0, np = pair_window_left_s();
                  /* advance the cache ONLY when the transport can actually take the
                   * publish (box_net_client_reading) -- else a change during a tty
                   * race / reconnect would be recorded as sent while the frame
                   * dropped, sticking dserv at the stale value. This on-change
                   * state can't self-heal like the ~3/s echo telemetry can. */
                  if ((nb != g_ble_tlm_lb || ne != g_ble_tlm_le || np != g_ble_tlm_lp)
                      && box_net_client_reading()) {
                      g_ble_tlm_lb = nb; g_ble_tlm_le = ne; g_ble_tlm_lp = np;
                      char bn[64]; uint8_t bf[DSERV_MSG_LEN];
                      dserv_state_name(&g_cfg, bn, sizeof bn, "ble/bonds");
                      dserv_msg_int(bf, bn, 0, nb);  box_net_client_send(bf, DSERV_MSG_LEN);
                      dserv_state_name(&g_cfg, bn, sizeof bn, "ble/encrypted");
                      dserv_msg_int(bf, bn, 0, ne);  box_net_client_send(bf, DSERV_MSG_LEN);
                      dserv_state_name(&g_cfg, bn, sizeof bn, "ble/pairing");
                      dserv_msg_int(bf, bn, 0, np);  box_net_client_send(bf, DSERV_MSG_LEN);
                  } }
#endif
                sched_publish_fired();                 /* post state/timer/<n> for fired schedules */
                uint32_t now = to_ms_since_boot(get_absolute_time());
                if (now - last_hb >= HEARTBEAT_MS) {
                    last_hb = now; publish_heartbeat();
#if defined(BOX_NET_USB) || defined(BOX_NET_DUAL) || defined(BOX_NET_BLE)
                    if (++announce_backstop >= 60) {    /* ~60s backstop: re-announce if a DTR edge was missed */
                        announce_backstop = 0; announce_pending = 1;
                    }
#endif
                    if (announce_pending &&              /* deferred burst: DTR-gated, forced every ~5s */
                        announce_burst(++announce_hb % 5 == 0)) announce_pending = 0;
#if defined(BOX_NET_DUAL)
                    /* USB mode only: first emit ~5s in (after macOS has both CDC ttys), then every ~3s */
                    if (box_net_is_usb() && ++reg_tick >= 5 && reg_tick % 3 == 0) reg_request(1, 1);
#elif defined(BOX_USB_FORWARD_REGISTER)
                    /* first emit ~5s in (after macOS has both CDC ttys), then every ~3s, quietly */
                    if (++reg_tick >= 5 && reg_tick % 3 == 0) reg_request(1, 1);
#endif
                }
            }
        }
    }
}

/* ======================================================================== *
 *  CORE 0 -- boot, console, log drain, slow I2C, flash
 * ======================================================================== */

/* Line editor only: chars in from UART/CDC0, echo out via the log ring,
 * finished lines queued to core 1 (which owns config/pins/net). */
static void console_service(void)
{
    static char line[CLI_LINE_MAX];
    static int  len = 0;
    int ch = box_console_getc();
    if (ch < 0) return;

    if (ch == '\r' || ch == '\n') {
        putchar('\n');
        line[len] = '\0'; len = 0;
        if (line[0] && !queue_try_add(&g_cmd_q, line)) printf("console busy, retry\n");
    } else if (ch == 0x08 || ch == 0x7f) {         /* backspace/DEL: edit the line, don't store the raw byte */
        if (len > 0) { len--; fputs("\b \b", stdout); }
    } else if (ch >= 0x20 && ch < 0x7f && len < (int) sizeof line - 1) {
        line[len++] = (char) ch;                   /* printable only -> no stray control bytes in config */
        putchar(ch);
    }
}

static int16_t ain_last_pub[AIN_NCH];
static void ain_service_core0(void)     /* ADS1115 I2C on core 0; publish-worthy samples -> queue */
{
    if (!g_cfg.ain_en) return;
    int fresh = ain_service(&g_cfg);    /* non-blocking single-shot state machine (+failure trip) */
    for (int ch = 0; ch < AIN_NCH; ch++)
        if ((fresh & (1 << ch)) && abs(ain_get(ch) - ain_last_pub[ch]) > AIN_DEADBAND) {
            ain_last_pub[ch] = ain_get(ch);
            ain_sample_t s = { time_us_64(), ain_last_pub[ch], (uint8_t) ch };
            queue_try_add(&g_ain_q, &s);           /* full -> drop; the next sample supersedes */
        }
}

#ifdef BOX_FUEL_MAX17048
static void fuel_service(void)          /* 1 Hz gauge read; heartbeat (core 1) publishes it */
{
    static uint32_t last;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last < 1000) return;
    last = now;
    g_fuel_soc = fuel_soc_pct();
}
#endif

static save_req_t g_save_req;           /* static: keep the config blob off the loop stack */
static void save_service(void)          /* execute queued flash ops (core 0; copy_to_ram
                                         * builds don't park core 1 -- see pico_flash.h) */
{
    if (!queue_try_remove(&g_save_q, &g_save_req)) return;
    if (g_save_req.erase_only) flash_store_erase();   /* core 1 already announced it */
    else {
        int rc = flash_store_save(&g_save_req.cfg);
        if (rc == 0) printf("flash ok\n");
        else printf("flash FAIL (rc=%d victim1=%d; -4=core1 not lockout-registered, -1=lockout timeout,"
                    " -100=serialize)\n", rc, (int) multicore_lockout_victim_is_initialized(1));
    }
}

/* Execute one OTA flash op requested by core 1's sink hook (core 0: it's the
 * only core allowed to flash_safe_execute under ASSUME_CORE1_SAFE). Two-flag
 * handshake with ota_flash_submit; core 1 spins on `done`. */
static void ota_flash_service_core0(void)
{
    if (!g_otaf.req) return;
    __dmb();                                          /* read payload after req is seen */
    g_otaf.req = 0;
    int rc = g_otaf.page ? flash_ota_program(g_otaf.off, g_otaf.page)
                         : flash_ota_erase(g_otaf.off);
    g_otaf.rc = (int8_t) rc;
    __dmb();                                          /* publish rc before done */
    g_otaf.done = 1;
}

/* TBYB self-test + buy (core 0: rom_explicit_buy is flash_safe_execute'd, and core
 * 0 is the flash actor). Only acts on a TRIAL boot. The new image must prove it is
 * FULLY FUNCTIONAL -- not just alive -- before we commit it:
 *   - core-1 heartbeat advancing (the RT loop isn't wedged), AND
 *   - the box is publishing to dserv (state client connected), AND
 *   - dserv has connected back to our config server (== registration acked, so the
 *     box is reachable for config/cmd -- e.g. the NEXT OTA).
 * All three must hold CONTINUOUSLY for OTA_SELFTEST_MS; any dip restarts the window.
 * Pass -> explicit_buy commits. If the new image can't sustain this (wedge, dead
 * transport, can't reach dserv, reg fails) it never buys -> the Stage-3 watchdog
 * reboots it -> the bootrom reverts to the previous slot (rollback). */
#define OTA_SELFTEST_MS 8000u
static void ota_buy_service_core0(void)
{
    static uint32_t healthy_since;    /* 0 = not currently in a healthy window */
    static uint32_t last_beat, beat_ms;
    if (!g_boot_trial || g_ota_bought) return;
    uint32_t now  = to_ms_since_boot(get_absolute_time());
    uint32_t beat = g_rt_beat;
    if (beat != last_beat) { last_beat = beat; beat_ms = now; }
    int alive   = (now - beat_ms) < 1500;             /* heartbeat advanced recently */
    int healthy = alive && g_net_up && g_srv_up;      /* RT alive + dserv connected + registered */

    if (!healthy) {                                   /* the window only runs while fully healthy */
        if (healthy_since)
            printf("ota trial: self-test dipped (alive=%d net=%d srv=%d) -- window restart\n",
                   alive, g_net_up, g_srv_up);
        healthy_since = 0;
        return;
    }
    if (healthy_since == 0) {
        healthy_since = now;
        printf("ota trial: healthy (RT+dserv+reg) -- self-test window %ums\n", OTA_SELFTEST_MS);
        return;
    }
    if (now - healthy_since < OTA_SELFTEST_MS) return;
    int rc = pico_ota_buy();
    g_ota_buy_rc = (int16_t) rc;
    g_ota_bought = 1;
    printf("ota trial: self-test PASSED (RT+dserv+reg %ums) -> explicit_buy rc=%d (%s)\n",
           OTA_SELFTEST_MS, rc, rc == 0 ? "COMMITTED" : "buy FAILED");
}

/* Render the status snapshot to the OLED, 4 Hz (core 0; ~0.5ms per frame).
 *   row 0: name + transport (+ '*' while auto is still sensing)
 *   row 1: IP / DHCP state / USB host state
 *   row 2: dserv client, registration ack, obs, DI levels
 *   row 3: boot cause + uptime */
static void oled_service_core0(void)
{
    static uint32_t next_ms;
    if (!oled_on) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((int32_t)(now - next_ms) < 0) return;
    next_ms = now + 250;

    char l[26], di[6];
    oled_clear();
    snprintf(l, sizeof l, "%-13.13s %s%s", dserv_cfg_name(&g_cfg),
             g_stat.usb ? "usb" : "eth", g_stat.sensing ? "*" : "");
    oled_text(0, 0, l);
    if (g_stat.usb)
        snprintf(l, sizeof l, "cdc %s", g_stat.cli_up ? "host attached" : "no host");
    else if (g_stat.ip[0] | g_stat.ip[1] | g_stat.ip[2] | g_stat.ip[3])
        snprintf(l, sizeof l, "%u.%u.%u.%u", g_stat.ip[0], g_stat.ip[1], g_stat.ip[2], g_stat.ip[3]);
    else
        snprintf(l, sizeof l, "dhcp: waiting");
    oled_text(0, 1, l);
    int n = g_stat.di_n;
    for (int i = 0; i < n; i++) di[i] = (char)('0' + ((g_stat.di_lvls >> i) & 1));
    di[n] = '\0';
    snprintf(l, sizeof l, "ds:%c rg:%c ob:%u di:%s",
             g_stat.cli_up ? '+' : '-', g_stat.srv_up ? '+' : '-', g_stat.obs, di);
    oled_text(0, 2, l);
    snprintf(l, sizeof l, "boot:%s up:%lum", g_boot_reason,
             (unsigned long)(time_us_64() / 60000000u));
    oled_text(0, 3, l);
    oled_flush();
}

/* Arm the HW watchdog once core 1 is up, then pet it while core 1's heartbeat
 * advances (or unconditionally in `wdt 0` bench mode). See the block comment at
 * g_rt_beat. Not armed while core 1 never comes up: console stays usable. */
static void watchdog_service(void)
{
    static uint8_t  armed;
    static uint32_t last_beat;
    if (!armed) {
        if (!g_core1_ready) return;
        watchdog_enable(BOX_WDT_MS, true);            /* pause while a debugger halts us */
        armed = 1;
        printf("watchdog: armed (%dms, core-1 gated; boot was %s)\n", BOX_WDT_MS, g_boot_reason);
        return;
    }
    uint32_t b = g_rt_beat;
    if (!g_wdt_gate || b != last_beat) {
        watchdog_update();
        last_beat = b;
    }
}

int main(void)
{
    stdio_init_all();
    box_console_init();   /* printf -> per-core ring from here on; UART/SDK-CDC demoted to sinks */
    sleep_ms(200);        /* brief power/settle delay */

    /* Why did we boot? watchdog = a core wedged and Stage 3 recovered it;
     * soft = commanded reboot (CLI/cmd/watchdog_reboot); power = cold start.
     * Printed here, echoed when the watchdog arms, published as state/boot. */
    if      (watchdog_enable_caused_reboot()) g_boot_reason = "watchdog";
    else if (watchdog_caused_reboot())        g_boot_reason = "soft";
    if (g_boot_reason[0] == 'w')
        printf("boot: WATCHDOG RESET -- a core wedged last run (state/boot=watchdog)\n");
#ifdef BOX_BLE_BREADCRUMBS
    /* Radio-path breadcrumb from the PREVIOUS run (armed at bring-up; scratch
     * survives a watchdog reset). Names the last stage the cyw43 bus code
     * reached -- the whole point is diagnosing a bring-up wedge that kills the
     * console before anything can print. See BLE.md "slot-boot radio wedge". */
    if (watchdog_hw->scratch[1] == 0xB007CB07u) {   /* [0]/[1] only -- [2]/[3] are
                                                     * rom_reboot's param parking! */
        g_ble_crumb = watchdog_hw->scratch[0];
        watchdog_hw->scratch[1] = 0;
        printf("bledbg: last-run radio crumb = 0x%08lx (0x01 pre-init, 0x30 bus-init, "
               "0x33 chip-up, 0x3E fw-check, 0x40xxxxxx fw-dl@offset, 0x5x spi)\n",
               (unsigned long) g_ble_crumb);
    }
#endif

    /* TBYB: capture the bootrom boot-info (lightweight, boot-safe -- just
     * rom_get_boot_info). If this is a FLASH_UPDATE buy-pending boot we're on a
     * TRIAL: ota_buy_service_core0() self-tests then explicit_buy's, else the
     * watchdog reboots us and the bootrom reverts. */
    { boot_info_t bi;
      if (rom_get_boot_info(&bi)) {
          g_boot_type      = bi.boot_type;
          g_boot_partition = (int8_t) bi.partition;
          g_boot_trial     = pico_ota_boot_is_trial(&bi);
          /* Reflect an OTA reboot in state/boot (was stuck at "power" -- neither
           * watchdog nor soft -- so an OTA-committed box looked like a cold boot).
           * "trial" = FLASH_UPDATE buy-pending; "update" = a committed FLASH_UPDATE. */
          if (g_boot_type == BOOT_TYPE_FLASH_UPDATE)
              g_boot_reason = g_boot_trial ? "trial" : "update";
      }
      printf("boot: rom_type=%s %s%s\n", pico_ota_boot_type_str(g_boot_type),
             pico_ota_boot_part_str(g_boot_partition), g_boot_trial ? " TRIAL(buy-pending)" : ""); }

    memset(&g_cfg, 0, sizeof g_cfg);
    if (flash_store_load(&g_cfg) == 0) printf("config: loaded from flash (name=%s)\n", dserv_cfg_name(&g_cfg));
    else                               printf("config: none/invalid -> defaults\n");

#ifdef BOX_NET_DUAL
    /* Boot transport policy. The GP28 strap to GND hard-forces Ethernet (hardware
     * can't go stale; unchanged for deployed boxes). Strap OPEN now defers to the
     * persisted `mode` policy, default AUTO: boot USB -- the safe transport that
     * always comes up -- and let core 1 sense the PHY link and upgrade to eth
     * (auto_sense_service). Safe to trust software again: the core-0 console is
     * alive no matter what the transport does, and auto never blocks on a cable. */
    gpio_init(BOX_MODE_STRAP_PIN);
    gpio_set_dir(BOX_MODE_STRAP_PIN, GPIO_IN);
    gpio_pull_up(BOX_MODE_STRAP_PIN);
    sleep_us(50);                             /* let the pull-up settle */
    int strap_gnd = !gpio_get(BOX_MODE_STRAP_PIN);
    if      (strap_gnd)                            g_xport = XPORT_ETH;   /* hardware wins */
    else if (g_cfg.transport_mode == XMODE_ETH)    g_xport = XPORT_ETH;
    else if (g_cfg.transport_mode == XMODE_USB)    g_xport = XPORT_USB;
    else                                         { g_xport = XPORT_USB; g_auto_sense = 1; }
    printf("transport: %s (%s, strap GP%d %s)\n", dserv_xport_str((uint8_t) g_xport),
           strap_gnd    ? "strap-forced"
         : g_auto_sense ? "auto: usb now, eth when link seen"
                        : "mode override",
           BOX_MODE_STRAP_PIN, strap_gnd ? "GND" : "open");
    box_net_select(g_xport, &g_cfg);          /* vtable set before core 1 launches */
#endif

    queue_init(&g_cmd_q,  CLI_LINE_MAX,         4);
    queue_init(&g_ain_q,  sizeof(ain_sample_t), 16);
    queue_init(&g_save_q, sizeof(save_req_t),   2);
#ifdef BOX_BLE
    box_ble_pipe_queues_init();     /* relay frame queues, before core 1 launches */
#endif
#ifdef BOX_NET_BLE
    box_net_ble_queues_init();      /* transport frame queues, before core 1 launches */
#endif
#ifdef BOX_FUEL_MAX17048
    fuel_init();
#endif
#ifdef BOX_STATUS_LED
    status_led_init();              /* onboard WS2812 (GP14): the handheld's only status UI */
#endif
    if (g_cfg.ain_en) ain_init();   /* I2C lives on THIS core (else the pins stay free) */
    if (g_cfg.oled_en) {            /* SPI display likewise; claim pins BEFORE core 1's  */
        pico_gpio_oled_claim = 1;   /* apply_config so user pin modes can't collide      */
        oled_init();
    }

    multicore_launch_core1_with_stack(rt_main, g_core1_stack, sizeof g_core1_stack);

    int announced = 0;
    absolute_time_t warn_at = make_timeout_time_ms(5000);
    while (1) {
        box_console_drain();        /* log rings -> UART (+ CDC0 via core-1 ferry) */
        console_service();
        ain_service_core0();
#ifdef BOX_FUEL_MAX17048
        fuel_service();
#endif
        save_service();
        ota_flash_service_core0();  /* service core-1 OTA flash requests */
        ota_buy_service_core0();    /* TBYB: self-test + explicit_buy on a trial boot */
        /* (The old boot-time flash-save DIAG lived here. REMOVED 2026-07-17: it
         * re-saved g_cfg every boot, so any FAILED load overwrote the good
         * persisted blob with defaults -- it destroyed the Thing Plus's saved
         * config while we hunted the load bug. Never re-add a boot-time WRITE
         * probe; `show`'s persist line reads the load diagnostics instead.) */
        oled_service_core0();       /* status panel, 4 Hz, ~0.5ms/frame on this core */
        watchdog_service();         /* arm once core 1 is up; pet while its heartbeat advances */
#ifdef BOX_BLE
        box_ble_service(&g_cfg, g_core1_ready,       /* radio on THIS core. core-1-ready gate =
                                                      * watchdog armed, so bring-up may re-arm it
                                                      * wider; wdt-boot flag skips auto bring-up
                                                      * once (no boot loop on a saved ble_en) */
                        g_boot_reason[0] == 'w');
#endif
#ifdef BOX_NET_BLE
        box_ble_periph_service(&g_cfg, g_core1_ready,   /* handheld: same contract; the radio IS
                                                         * the transport, so bring-up ignores ble_en */
                               g_boot_reason[0] == 'w');
#endif
#ifdef BOX_STATUS_LED
        /* Handheld status LED: derive one logical state from the signals this
         * core owns (radio state/link/encrypt + battery %), priority high->low;
         * the module renders color + blink. FAIL first (hardware), then low
         * battery (always worth seeing), then link quality. */
        { uint8_t st;
          if      (box_ble_state == BOX_BLE_FAIL)              st = STATUS_LED_FAIL;
          else if (box_ble_state != BOX_BLE_UP)               st = STATUS_LED_BRINGUP;
#ifdef BOX_FUEL_MAX17048
          else if (g_fuel_soc >= 0 && g_fuel_soc < 15)        st = STATUS_LED_LOWBATT;
#endif
          else if (box_ble_link && hh_encrypted)              st = STATUS_LED_GOOD;
          else if (hh_connected())                            st = STATUS_LED_CONNECTING;
          else                                                st = STATUS_LED_SEARCHING;
          status_led_service(st);
        }
#endif
        if (!announced) {
            if (g_core1_ready) {
                announced = 1;
                printf("box ready [%s] fw %s: config TCP :%d  |  console CLI (type 'help')\n",
                       box_net_backend_name(), BOX_FW_VERSION, CFG_PORT);
            } else if (absolute_time_diff_us(get_absolute_time(), warn_at) <= 0) {
                announced = 1;      /* keep the console alive regardless; core 1 may still come up */
                printf("core1 not ready after 5s (transport init?) -- console still live\n");
            }
        }
    }
}
