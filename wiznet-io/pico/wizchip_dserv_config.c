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
 * reads just ain_en/ain_rate/ain_gain (benign staleness).
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

alarm_pool_t *box_alarm_pool;       /* core-1 alarm pool (pulse/sched/DHCP-tick IRQs on the RT core);
                                     * extern'd by pico_gpio.h + box_net_w6300.h */

static uint8_t g_rxbuf[RXBUF_SIZE];
static pico_config_t  g_cfg;
static dserv_framer_t g_framer;
static box_clock_t    g_clock;      /* box->dserv time offset, snapped at obs edges */
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
    /* box edge time mapped into dserv time (0 => dserv arrival-stamps pre-sync) */
    dserv_msg_int(f, nm, box_clock_stamp(&g_clock, e->t_us), lvl);
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
    dserv_msg_int(f, nm, t_us ? box_clock_stamp(&g_clock, t_us) : 0, bits);
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
    dserv_msg_int(f, nm, box_clock_stamp(&g_clock, t_us), v);
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
    dserv_msg_int(f, nm, box_clock_stamp(&g_clock, fire_us), 1);
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
    if (usb_target) snprintf(s, sizeof s, "usb-host");
    else snprintf(s, sizeof s, "%u.%u.%u.%u:%u", g_cfg.dserv_ip[0], g_cfg.dserv_ip[1],
                  g_cfg.dserv_ip[2], g_cfg.dserv_ip[3], dserv_cfg_port(&g_cfg));
    dserv_state_name(&g_cfg, nm, sizeof nm, "dserv");
    dserv_msg_string(f, nm, 0, s);
    box_net_client_send(f, DSERV_MSG_LEN);
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
#if defined(BOX_NET_USB) || defined(BOX_NET_DUAL)
    if (!force && !box_net_client_reading()) return 0;   /* host hasn't opened the data CDC yet */
#else
    (void) force;
#endif
    publish_ident(); publish_manifest();
    publish_di_levels(); publish_group_levels();
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
#if !defined(BOX_NET_USB)               /* pure-USB build: host module owns forwarding */
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
#endif /* !BOX_NET_USB */

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

/* ---- config/cmd in: dispatch one 128B datapoint frame ---- */
static void on_frame(const uint8_t *frame, void *ud)
{
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

    gpio_cmd_t cmd;
    cfg_result_t r = dserv_dispatch(cfg, &m, &cmd);
    DBG("dp: %.*s -> %s\n", (int) m.namelen, m.name, dserv_cfg_result_str(r));

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
            publish_do(cmd.pin, (uint8_t) cmd.value, box_clock_stamp(&g_clock, t_act));  /* actual readback */
    }
    else if (r == CFG_PIN_MODE || r == CFG_OBS_PIN || r == CFG_SYNC_PIN) {
        pico_gpio_apply_config(cfg); groups_reset_all();
        publish_manifest();   /* active-pin set OR obs/sync pin changed -> re-announce */
    }
    else if (r == CFG_GROUP)     { groups_reset_all(); publish_manifest(); }
    else if (r == CFG_LABEL || r == CFG_DESC)          publish_manifest();
    else if (r == CFG_SAVE)      save_request(0);       /* core 0 writes; prints flash ok/FAIL */
    else if (r == CFG_REBOOT)    box_reboot(0);
    else if (r == CFG_BOOTSEL)   { printf("entering BOOTSEL\n"); box_reboot(1); }
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
    if (!strcmp(line, "show"))         /* live status trailer: transport / boot / uptime / fw */
        printf("  transport=%s%s boot=%s up=%lus fw=%s\n",
#ifdef BOX_NET_DUAL
               box_net_is_usb() ? "usb" : "eth",
               g_auto_sense ? " (auto: sensing for eth link)" : "",
#else
               box_net_backend_name(), "",
#endif
               g_boot_reason, (unsigned long)(time_us_64() / 1000000u), BOX_FW_VERSION);
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
#if defined(BOX_NET_USB)
    return 1;   /* USB: the host dserv module is always the target -- no IP to configure */
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
#ifndef PICO_FLASH_ASSUME_CORE1_SAFE
    flash_safe_execute_core_init();     /* lockout victim: core 0 flash ops park us safely.
                                         * copy_to_ram builds define the flag instead: this core
                                         * never touches flash/XIP, so saves don't park it at all. */
#endif
    box_alarm_pool = alarm_pool_create_with_unused_hardware_alarm(24);  /* pulse/sched/DHCP IRQs -> this core;
                                                                         * 24 = 8 sched + many pulses + DHCP tick */
#ifdef BOX_NET_DUAL
    box_net_dual_usb_init();            /* tusb_init HERE: the USB IRQ must land on core 1 */
#endif
    pico_gpio_apply_config(&g_cfg);     /* HERE: DI edge IRQs must land on core 1 */
    groups_reset_all();                 /* chord groups start from the seeded levels */
    if (box_net_init(&g_cfg) != 0) printf("net init FAILED\n");
    dserv_framer_reset(&g_framer);
    box_clock_reset(&g_clock);
#ifdef BOX_NET_DUAL
    g_auto_window_end_ms = to_ms_since_boot(get_absolute_time()) + AUTO_WINDOW_MS;
#endif
    g_core1_ready = 1;                  /* core 0 announces the ready banner */

    uint32_t last_hb = 0;
    uint8_t  announce_pending = 0;      /* connect burst deferred until the host reads (USB tty race) */
    uint8_t  announce_hb = 0;           /* heartbeats since defer -> forces a DTR-bypass retry every ~5s */
#if defined(BOX_NET_USB) || defined(BOX_NET_DUAL)
    uint8_t  prev_reading = 0;          /* DTR edge on the data CDC: host (re)opened -> re-announce */
    uint16_t announce_backstop = 0;     /* ~60s re-announce backstop if a DTR reconnect edge is missed */
#endif
#if defined(BOX_NET_DUAL) || defined(BOX_USB_FORWARD_REGISTER)
    uint32_t reg_tick = 0;              /* delayed USB autoregistration cadence */
#endif
    while (1) {
        g_rt_beat++;               /* heartbeat: core 0 pets the HW watchdog only while this advances */
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

        char cline[CLI_LINE_MAX];  /* console lines assembled by core 0 */
        while (queue_try_remove(&g_cmd_q, cline)) cmd_exec(cline);

        reg_service();             /* advance any in-flight registration, one step per pass */

        /* state out (box -> dserv), once a target is configured */
        if (!have_dserv_target()) {
            status_update(0);                  /* OLED snapshot still refreshes unconfigured */
        } else {
            uint16_t port = dserv_cfg_port(&g_cfg);
            int up = box_net_client_service(g_cfg.dserv_ip, port);
            status_update(up);
#if defined(BOX_NET_USB) || defined(BOX_NET_DUAL)
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
            }
#if defined(BOX_NET_DUAL)
            if (up == 2 && !box_net_is_usb()) reg_request(1, 0);  /* Eth: reg on connect; USB: delayed autoreg below */
            if (up >= 1 && !box_net_is_usb()) reg_watchdog_service();
#elif !defined(BOX_USB_FORWARD_REGISTER)
            if (up == 2) reg_request(1, 0);    /* wired/wifi: register on connect. (USB autoreg emits later,
                                                * NOT on connect -- writing CDC1 before macOS finishes creating
                                                * its tty drops the data port; the delayed periodic below is safe.) */
#endif
#if !defined(BOX_NET_USB) && !defined(BOX_NET_DUAL)
            if (up >= 1) reg_watchdog_service();   /* wired/wifi: heal lost registrations */
#endif
            if (up >= 1) {
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
                sched_publish_fired();                 /* post state/timer/<n> for fired schedules */
                uint32_t now = to_ms_since_boot(get_absolute_time());
                if (now - last_hb >= HEARTBEAT_MS) {
                    last_hb = now; publish_heartbeat();
#if defined(BOX_NET_USB) || defined(BOX_NET_DUAL)
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
    else printf("flash %s\n", flash_store_save(&g_save_req.cfg) == 0 ? "ok" : "FAIL");
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
#ifdef BOX_FUEL_MAX17048
    fuel_init();
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
        oled_service_core0();       /* status panel, 4 Hz, ~0.5ms/frame on this core */
        watchdog_service();         /* arm once core 1 is up; pet while its heartbeat advances */
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
