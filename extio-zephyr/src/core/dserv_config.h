/*
 * dserv_config.h -- hardware-independent dispatch of the box's datapoints.
 *
 * Every datapoint the box cares about lives under a CONFIGURABLE device name
 * (default "box"), so multiple boxes coexist on one dserv (e.g. "io1", "rig2").
 * The name is persisted config; set it over the USB CLI (`name io1`) or via the
 * datapoint <name>/config/name. dserv relays <name>/config/(keys) and <name>/gpio/(keys)
 * to the box (%reg ... 1 + matches).
 *
 *   <name>/config/name            (str)  rename the device
 *   <name>/config/desc            (str)  free-form box description (announced)
 *   <name>/config/pin/<n>/mode    (int)  0=unset 1=out 2=in 3=in_pullup
 *   <name>/config/pin/<n>/pulse_us(int)  default DO pulse width for pin n
 *   <name>/config/pin/<n>/label   (str)  role label ("up", "button_left"); "off" clears
 *   <name>/config/group/<g>/pins  (str)  "2,3,4,5" DI chord group; "off" clears
 *   <name>/config/group/<g>/label (str)  names state/group/<label>; "off" clears
 *   <name>/config/group/<g>/settle_ms (int) chord-settle window (0 = per transition)
 *   <name>/config/group/<g>/quiet (int)  1 = suppress members' state/di/<n>
 *   <name>/config/sync/pin        (int)  TTL obs-sync INPUT from the rig host
 *                                        ("off" disables) -- hardware clock anchor
 *   <name>/config/net/ip          (str)  "a.b.c.d" (applied on save/boot)
 *   <name>/config/dserv/ip        (str)  "a.b.c.d"
 *   <name>/config/dserv/port      (int)
 *   <name>/gpio/<n>               (int)  drive pin level 0/1        [transient]
 *   <name>/gpio/<n>/pulse_us      (int)  box-timed high pulse (us)  [transient]
 *
 * ACTIONS ARE cmd/, NOT config/ -- the config/ keys above only mutate the struct
 * in RAM, and nothing is written to flash until:
 *
 *   <name>/cmd/save               (any)  persist the whole config blob to flash
 *   <name>/cmd/reboot             (any)  warm reset
 *   <name>/cmd/factory            (any)  wipe persisted config
 *
 * This block previously documented `<name>/config/save`, which HAS NEVER
 * EXISTED: the matcher below tests strcmp(k, "save") on the cmd/ leaf, and
 * main.c's handler prints "cmd/save -> ...". Setting config/save is silently
 * inert -- it returns CFG_UNKNOWN, prints nothing, and the config you just set
 * looks perfectly applied in state/* right up until the next reboot loses it.
 * Verified 2026-07-27: config/save -> no console output, config lost on reboot;
 * cmd/save -> "cmd/save -> ok (1080 bytes)", config survives.
 *
 * config/(keys) are persistent settings ONCE cmd/save is sent; gpio/(keys) are
 * transient commands (never saved).
 * config_apply mutates the struct (portable); a gpio command is returned as a
 * gpio_cmd_t for the platform layer (real GPIO on device, print in sim).
 *
 * Pure C, zero-alloc, no hardware. Shared by firmware and simulator.
 */
#ifndef DSERV_CONFIG_H
#define DSERV_CONFIG_H

#include "dserv_msg.h"
#include <stdio.h>       /* sscanf */
#include <stdlib.h>      /* strtol (group pin lists) */

#define BOX_NPINS     30
#define BOX_NAME_MAX  16
#define BOX_NGROUPS   4     /* DI chord groups (atomic bitmask publishing)   */
#define BOX_NAGROUPS  4     /* analog (MCP3204) groups: named channel sets   */
#define BOX_LABEL_MAX 16    /* per-pin / per-group role labels               */
#define BOX_DESC_MAX  40    /* free-form box description                     */
#define BOX_CHANNEL_MAX 16  /* firmware update channel this box tracks       */

/* dserv's client port when cfg->dserv_port is unset (0). One place, so the
 * dispatch code and the CLI's `show` agree on the effective value; see
 * dserv_cfg_port() below. */
#define DSERV_DEFAULT_PORT 4620

/* Namespace class: every box publishes/subscribes under <BOX_CLASS>/<name>/... so
 * all external I/O boxes group under one parent (extio/) in the datapoint tree,
 * clearly separated from local ess/gpio/mtouch/etc. One-line change to rename. */
#define BOX_CLASS "extio"

typedef struct {
    char     name[BOX_NAME_MAX];       /* device name (within BOX_CLASS/); "" => "box" */
    uint8_t  pin_mode[BOX_NPINS];      /* 0 unset, 1 out, 2 in, 3 in_pullup  */
    uint32_t do_pulse_us[BOX_NPINS];
    uint8_t  net_ip[4];
    uint8_t  dserv_ip[4];
    uint16_t dserv_port;
    uint32_t applied_count;             /* persistent settings applied        */
    uint8_t  debounce_ms[BOX_NPINS];   /* per-input debounce window, 0 = off */
    uint8_t  net_mode;                  /* 0=DHCP (default), 1=static; NET_MODE_* */
    uint8_t  obs_pin;                   /* GPIO driven to the live ess/in_obs copy (valid iff obs_en) */
    uint8_t  obs_en;                    /* 1 = obs-mirror on; 0 = off (default). Separate flag so any
                                         * GPIO incl GP0 is usable and factory-zero == off. */
    uint32_t di_active_low;             /* bitmask: bit n set => publish state/di/n inverted (pressed=1,
                                         * matching the local gpio ACTIVE_LOW convention) */
    char     wifi_ssid[32];             /* pico2w WiFi creds set at RUNTIME (USB CLI / datapoint);
                                         * empty => fall back to compile-time WIFI_SSID/PASSWORD */
    char     wifi_pass[64];
    uint8_t  wifi_pm;                   /* pico2w: 1 = WiFi power-save (battery), 0 = off (low latency) */
    /* v10 was the ADS1115 I2C analog-in (ain_rate/ain_gain/ain_en). REMOVED
     * 2026-07-19 in favor of the MCP3204 SPI ADC (ain_en); these 3 bytes are
     * RETAINED AS RESERVED so the persist layout is byte-identical -- removing
     * them would shift every later field and corrupt already-saved configs. */
    uint8_t  _rsvd_ain_rate;
    uint8_t  _rsvd_ain_gain;
    uint8_t  _rsvd_ain_en;
    uint8_t  transport_mode;            /* DUAL boot policy when the GP28 strap is OPEN (strap to GND still
                                         * hard-forces Ethernet): XMODE_AUTO (0, default: boot USB, sense the
                                         * PHY link, upgrade to eth), XMODE_ETH, XMODE_USB. Safe to persist
                                         * now: the core-0 console stays alive whatever the transport does. */
    uint8_t  oled_en;                   /* 1 = SSD1306 128x32 SPI status display on the OLED_PIN_* set
                                         * (box_gpio.h; those pins become reserved). Applied at boot. */
    /* v13: identity + DI chord groups. desc/labels are announced METADATA (the
     * manifest published at every (re)connect); groups add one generic BEHAVIOR:
     * member DI edges publish as a single atomic bitmask datapoint, chord-settled
     * and stamped at the FIRST edge of the episode (box_group.h). The box knows
     * pins/groups/labels -- never device types; semantics stay host-side. */
    char     desc[BOX_DESC_MAX];       /* free-form description, for humans/fleet page */
    char     pin_label[BOX_NPINS][BOX_LABEL_MAX];     /* role label; "" = none */
    uint32_t group_pins[BOX_NGROUPS];  /* member DI pins (bit n = GPn); 0 = group unused.
                                         * Published bit i = i-th LOWEST member pin. */
    char     group_label[BOX_NGROUPS][BOX_LABEL_MAX]; /* names state/group/<label>; "" -> "g<idx>" */
    uint16_t group_settle_ms[BOX_NGROUPS];  /* chord-settle window; 0 = every settled transition */
    uint8_t  group_quiet[BOX_NGROUPS]; /* 1 = suppress members' state/di/<n> publishes */
    /* v14: hardware obs-sync INPUT -- the rig host's TTL obs line wired to a
     * box pin. The IRQ-stamped edge replaces frame-arrival time as the clock
     * anchor (transport drops out of the sync error budget); the ess/in_obs
     * frame just delivers the dserv-side timestamp. See box_gpio.h latch +
     * the pairing in wizchip_dserv_config.c on_frame. High = in obs (matches
     * ess-2.0.tm begin_obs's rpioPinOn). Separate from obs_pin (the OUTPUT
     * mirror/LED): both together give a scope-able end-to-end self-test. */
    uint8_t  sync_pin;                  /* valid iff sync_en */
    uint8_t  sync_en;                   /* 1 = TTL sync input on; 0 = off (default) */
    /* v15: BLE central (BOX_BLE radio builds -- pico2w-family boards; no-op
     * elsewhere). Default OFF: a lab of boxes scanning/advertising unasked is
     * RF noise + a pairing surprise surface; a rig opts in when a handheld is
     * bonded. Enable is live (core 0 lazily brings the radio up); disable
     * quiets it, full power-off at reboot. See wiznet-io/BLE.md. */
    uint8_t  ble_en;

    /* v16: BLE central relay auto-arm (`ble pipe 1` persisted). When set, the
     * receiver arms the handheld pipe once per radio-up -- a saved pipe_en
     * survives reboots, so the relay comes back with zero console touches.
     * Runtime `ble pipe 0|1` stays live either way; only `save` persists. */
    uint8_t  pipe_en;

    /* v17: idle peripheral-latency target (BOX_BLE receiver, `ble latency <n>`).
     * 0 (default) = handheld listens every connection event -- the echo-sync-
     * validated behavior. N > 0 = once the clock is synced, let the handheld
     * SKIP up to N events when idle (RX-power win; event RTs unaffected since the
     * peripheral still wakes to TRANSMIT). The receiver drops back to 0 for a
     * periodic sync burst. See wiznet-io/BLE.md "Power" + box_ble_central.h. */
    uint8_t  ble_latency;

    /* v18: analog input -- originally the MCP3204 SPI ADC, now any converter
     * behind box_adc.h (on-chip LPADC on the MCXN947). A channel scan (2-axis
     * joystick on CH0/CH1) published as one packed state/ain/scan snapshot. Off
     * by default.
     *
     * APPLIED LIVE on Zephyr boards: clearing it stops the sampler and (where
     * the driver has a PM hook) powers the converter down; setting it brings
     * both back. Nothing here needs a reboot. The RP2350 build is the one where
     * this claimed the SPI0 pins at init and therefore did. */
    uint8_t  ain_en;

    /* v19: firmware update channel this box TRACKS (extio-setup / OTA compare
     * against <channel>/latest for this build line). A deployment policy, not a
     * compile stamp -- the same image bytes can live in dev and stable, so it's
     * persisted, set at provision time (provision.sh --channel) and settable live
     * (`channel <name>`). Empty => "dev" (see dserv_cfg_channel); a box predating
     * this field loads it zeroed and thus reads as dev. Announced as state/channel
     * and in the `show` trailer so the host tool can key updates to it. */
    char     channel[BOX_CHANNEL_MAX];

    /* v20: MCP3204 analog groups -- a named set of ADC channels published
     * together with one sampling policy. The analog twin of DI chord groups
     * (box_group.h / box_ain_group.h): the box knows channels/labels/policy,
     * never device semantics (that stays host-side). A group is ACTIVE iff
     * ain_group_chans[g] != 0. ain_en stays the master switch; with it set and
     * NO group defined, dserv_cfg_ain_
     * default() synthesizes group 0 = {0,1} on-change "joystick" so the stick
     * just works. ain_rate is the box-wide base SCAN rate; a group publishes at
     * base/decimate (0/1 = every scan), optionally boxcar-averaged (flags bit0).
     * All fields additive at the struct tail => older saved configs stay valid
     * (they load these zeroed => no groups => the default kicks in). */
    uint16_t ain_rate;                                 /* base scan rate Hz (0 -> 50) */
    uint8_t  ain_group_chans[BOX_NAGROUPS];           /* channel mask, bit c = MCP ch c (0..3); 0 = unused */
    char     ain_group_label[BOX_NAGROUPS][BOX_LABEL_MAX]; /* names state/ain/<label>; "" -> "a<idx>" */
    uint8_t  ain_group_mode[BOX_NAGROUPS];            /* 0 = on-change (deadband), 1 = continuous */
    uint16_t ain_group_deadband[BOX_NAGROUPS];        /* on-change: publish when a member moves > this (counts) */
    uint8_t  ain_group_decimate[BOX_NAGROUPS];        /* publish every Nth base scan (0/1 = every) */
    uint8_t  ain_group_batch[BOX_NAGROUPS];           /* continuous: scans per block (0/1 = per-scan) */
    uint8_t  ain_group_flags[BOX_NAGROUPS];           /* bit0 = average (boxcar mean) instead of drop */

    /* v21: STATIC net gateway + subnet mask. net_mode=static previously set only
     * net_ip and left the compiled 192.168.11.x default gateway -- off-subnet for
     * any real static IP, which WEDGES the W6300 outbound path (it can't ARP a
     * gateway that isn't on the wire, so even same-subnet connects fail). Zeroed
     * => bn_apply_static derives the subnet's .1 gateway and a /24 mask; set them
     * to override (a real gateway, or a direct link's own-subnet address).
     * Additive tail => older saved blobs load these zeroed (auto-derive). */
    uint8_t  net_gw[4];
    uint8_t  net_sn[4];

    /* v22: which device the management console binds to. This is a TIMING
     * choice, not a cosmetic one: a second actively-claimed CDC-ACM instance
     * costs ~0.21 ms of median round-trip latency (measured -- see
     * PORTING.md), while the same console on a hardware UART is essentially
     * free. 0 = cdc (default: works with no extra hardware), 1 = uart (needs a
     * USB-serial adapter on the board's console UART pins).
     * Additive tail => older blobs load 0 = cdc, i.e. today's behaviour.
     * Safe to persist wrongly: the console is NOT the control channel, so
     * config/console is still reachable over the frame pipe from dserv. */
    uint8_t  console_mode;
    /* v23: the obs pin's ROLE (valid iff obs_en; pin stays obs_pin).
     * 0 = mirror (default: the output follows the host's ess/in_obs --
     * today's behaviour, and what an older blob loads). 1 = LEADER: the
     * box owns the obs line and the epoch -- an at_abs on the pin is a
     * scheduled onset (provisional epoch at arm, actual-stamped
     * state/in_obs at fire), and mirror-following is SUPPRESSED so the
     * physical line moves only at scheduled instants plus the end clear:
     * once other instruments trigger on that line it is data, and a
     * twitch at frame-arrival time would be a lie on the wire. A
     * persisted leader is announced (manifest obs_leader) so hosts can
     * DISCOVER the box that may lead: ::ess::obs_schedule_bind auto. */
    uint8_t  obs_mode;
    /* v24: LPADC hardware averaging, stored as the EXPONENT (0..7 -> 1x..128x
     * conversions averaged in silicon per trigger). This is the noise tool
     * that does NOT cost sweep rate: base 10 kHz + software boxcar to get a
     * quiet 1 kHz saturated the cooperative sampler (the +59 wedge); base
     * 1 kHz + 16x here delivers MORE averaging at ~1/5 the CPU and a tenth
     * of the wakeups. 0 = off = exactly what an older blob loads as. */
    uint8_t  ain_ovs;
    /* v25: which PACING the analog sampler uses. 0 = auto (the build's default,
     * CONFIG_BOX_ADC_STREAM_DEFAULT -- so flipping the fleet is a firmware
     * decision, not a re-save of every box's config), 1 = polled, 2 = stream.
     *
     * AN OVERRIDE, NOT A SETTING, and the encoding is what makes that true. A
     * plain 0/1 boolean would have meant that the day stream becomes the
     * default, every box carrying a saved blob would still load 0 = polled and
     * "the default" would be a thing only factory-fresh boxes ever saw. Auto
     * defers to the build; the two explicit values exist so a box can be pinned
     * either way from the console while a fault is being chased -- which is the
     * only reason anyone should be setting this at all.
     *
     * Boards without CONFIG_BOX_ADC_STREAM ignore it entirely: there is no
     * hardware-paced path to select, and `auto` resolves to polled. */
    uint8_t  ain_pace;
    /* v26: how much periodic telemetry this box publishes.
     *
     * 0 = HEALTH (default), 1 = FULL, 2 = OFF -- and health MUST be the zero,
     * because an older blob loads absent fields as zero and the default has to
     * be what those boxes get.
     *
     * WHY IT EXISTS. Telemetry was unconditional and it grew by accretion: by
     * stage 2 a box was sending ~48 diagnostic frames/s against 250 frames/s of
     * actual eye data, and at batch 10 the diagnostics OUTWEIGHED the science
     * (47 against 25). Each frame costs 275-864 us of CPU to send, so the cost
     * is per-send overhead rather than bandwidth -- not sending is worth far
     * more than sending efficiently. Health keeps what answers "is this box
     * alive and is it losing data"; full is what you turn on for an afternoon
     * while chasing something.
     *
     * The watchdog is NOT gated by this at any level. Its whole purpose is to
     * advance every second so a frozen one is diagnostic, and a debug setting
     * that could silence the liveness beacon would be a setting that can make a
     * dead box look like a quiet one. */
    uint8_t  dbg_level;
    /* v27: measured deviation of the CTIMER's source clock from the frequency
     * the SDK reports, in signed ppm. 0 = uncalibrated (use nominal).
     *
     * WHY A CALIBRATION CONSTANT AND NOT A CONTROL LOOP. The trigger clock is
     * FRO_HF, an on-chip RC oscillator, so the rate actually delivered differs
     * from the rate asked for by that oscillator's manufacturing error --
     * measured -2897 ppm on one box and about +500 ppm on another. Sample TIMES
     * are unaffected (they are measured, not assumed), so this is not a
     * correctness fix; it is what makes `ain rate 100` mean 100 Hz, and makes
     * two boxes agree with each other.
     *
     * Applied ONCE, when a stream starts, so the rate is right from the first
     * sample and constant for the whole run. Re-tuning mid-run would trade a
     * stable-but-offset rate for one that steps around as the box warms, which
     * is worse for analysis: the offset is mostly fixed manufacturing spread,
     * not drift.
     *
     * ain/dbg/clk_ppm_meas reports what the box currently MEASURES; this field
     * is what it APPLIES. cmd/ain/calibrate copies one to the other. They
     * differ until someone calibrates, and after a `save` they agree across
     * reboots. */
    int16_t  ain_clk_ppm;
} box_config_t;

/* Max int16 samples in one analog block: 12B header + 48B of samples, and a
 * long "extio/<name>/ain/<label>" varname must still fit the 128B frame
 * (varlen+datalen <= 109). At 8 channels this caps batch at 3 (24/8).
 *
 * DEFINED HERE, not beside the block struct, because the CONFIG layer is where
 * an over-large batch has to be refused -- and box_ain_group.h cannot be the
 * home for that constant when it is this header's dependent, not its provider. */
#define AIN_BLOCK_MAX  24

/* Ceiling on how often ONE on-change group may publish. Its rate is set by
 * INPUT NOISE rather than by config, so it needs a cap of its own. */
#define AIN_MAX_BLOCKS_PER_S  200

/* Ceiling on the TOTAL block rate across every group, and the number that
 * decides whether this box stays up.
 *
 * MEASURED on a bench MCXN947 (2 ch, 64x oversample, continuous, batch 1),
 * counting delivered blocks and watching the service loop:
 *
 *     2000 blocks/s   exact delivery, 0 drops, watchdog 15/15  -- clean
 *                     (identical whether from 1 group at 2000 Hz or 4 at 500)
 *     3000 blocks/s   6.6% short, bulk drops climbing, watchdog 14/15
 *     8000 blocks/s   box lost (4 groups x 2000 Hz, no batching)
 *
 * The governing variable is TOTAL blocks/s, not sample rate: the per-frame send
 * cost (275-864 us) is what saturates the service loop, and four groups at
 * 500 Hz load it exactly like one at 2000. The old per-group cap could not see
 * that, and exempted continuous mode entirely on the grounds that its rate is
 * operator-declared -- which is precisely how 8000 was reached. */
#define AIN_MAX_TOTAL_BPS  2000

/* Largest batch group `g` may use, given the channels it currently has.
 *
 * A block carries count*nchan samples, so the two settings are coupled and the
 * sampler has always clamped silently (ain_group_batch_eff). Silently is the
 * problem: a group configured batch 20 on 2 channels read back 20 and sampled
 * 12, which is the config-field-that-lies shape from the inside. With no
 * channels set yet there is nothing to bound, so the write is allowed and
 * re-checked when channels arrive. */
static inline int ain_batch_cap(const box_config_t *c, int g)
{
    int n = 0;

    if (g < 0 || g >= BOX_NAGROUPS) return 1;
    for (uint8_t m = c->ain_group_chans[g]; m; m >>= 1) n += (m & 1u);
    if (n <= 0) return AIN_BLOCK_MAX;
    int cap = AIN_BLOCK_MAX / n;
    return cap < 1 ? 1 : cap;
}

/* Re-clamp a group's batch after its CHANNEL SET changed. Widening a group is
 * the one edit that can invalidate an already-accepted batch, and refusing the
 * channel change over a secondary field would be the wrong thing to reject --
 * so batch yields, and the announce republishes the value that will actually be
 * used. Returns 1 if it had to change something. */
static inline int ain_batch_reclamp(box_config_t *c, int g)
{
    int cap = ain_batch_cap(c, g);

    if (c->ain_group_batch[g] > cap) {
        c->ain_group_batch[g] = (uint8_t) cap;
        return 1;
    }
    return 0;
}

/* box_config_t.ain_pace */
enum { AIN_PACE_AUTO = 0, AIN_PACE_POLLED = 1, AIN_PACE_STREAM = 2 };
/* box_config_t.dbg_level */
enum { DBG_LEVEL_HEALTH = 0, DBG_LEVEL_FULL = 1, DBG_LEVEL_OFF = 2 };

#define OBS_MODE_MIRROR 0
#define OBS_MODE_LEADER 1

#define AIN_GROUP_FLAG_AVG  0x01u   /* ain_group_flags: decimate window -> boxcar mean, not drop */

/* box_config_t.net_mode. Zeroed default (factory/blank config) => DHCP, so a
 * fresh box works out of the box on a router; a static net/ip is honored only in
 * NET_MODE_STATIC. DHCP falls back to the static/default IP if no lease. */
enum { NET_MODE_DHCP = 0, NET_MODE_STATIC = 1 };

typedef enum { GPIO_OP_NONE = 0, GPIO_OP_SET, GPIO_OP_PULSE,
               GPIO_OP_SCHED_PULSE,   /* fire a pulse at beginobs + value(us)  */
               GPIO_OP_SCHED_TIMER    /* post state/timer/<pin> at beginobs + value(us) */
} gpio_op_t;
typedef struct { gpio_op_t op; uint8_t pin; uint32_t value; } gpio_cmd_t;

typedef enum {
    CFG_NONE = 0,   /* not under this box's name          */
    CFG_NAME,
    CFG_PIN_MODE,
    CFG_DO_PULSE,
    CFG_DEBOUNCE,
    CFG_NET_IP,
    CFG_NET_MODE,
    CFG_OBS_PIN,
    CFG_OBS_MODE,
    CFG_SYNC_PIN,
    CFG_CONSOLE,
    CFG_XPORT,      /* xport/mode auto|usb|eth -- transport boot policy, the
                     * datapoint twin of the CLI `mode` verb (save+reboot) */
    CFG_ACTIVE_LOW,
    CFG_WIFI_SSID,
    CFG_WIFI_PASS,
    CFG_WIFI_PM,
    CFG_AIN_EN,
    CFG_OLED_EN,
    CFG_BLE_EN,
    CFG_PIPE_EN,
    CFG_BLE_LATENCY,
    CFG_DESC,
    CFG_LABEL,
    CFG_GROUP,
    CFG_AIN,        /* mcp/rate or ain/group/<g>/... analog-group config */
    CFG_DSERV_IP,
    CFG_DSERV_PORT,
    CFG_GPIO,       /* a cmd/do output command parsed into *cmd */
    CFG_SAVE,
    CFG_REBOOT,
    CFG_FACTORY,
    CFG_BOOTSEL,    /* cmd/bootsel -> reboot into USB BOOTSEL for reflashing */
    CFG_BLE_PAIR,   /* cmd/ble/pair <secs> -> open the receiver's pairing window (remote `ble pair`) */
    CFG_BLE_FORGET, /* cmd/ble/forget      -> clear the bond allowlist  (remote `ble forget`)      */
    CFG_ANNOUNCE,   /* cmd/announce -> re-publish the FULL manifest right now.
                     * The burst otherwise fires only when a host opens the pipe,
                     * and only SOME config edits re-publish the manifest as a
                     * side effect (pin mode, obs/sync, debounce, active_low,
                     * group, label, desc). Change ain/rate or dserv/ip and
                     * nothing is published at all, so a UI shows the old value
                     * until the next reconnect -- indistinguishable from the
                     * write having failed. This makes "tell me what you are
                     * now" an explicit, cheap request instead of a wait. */
    CFG_DUMP,       /* cmd/dump -> publish this box's whole config to state/cfg/dump
                     * as ONE datapoint: the same replayable CLI text the `dump`
                     * console verb prints, newline-joined.
                     *
                     * The `dump` verb needed a serial cable, which is the wrong
                     * place for it -- the caller who most wants a snapshot is a
                     * HOST about to overwrite the config (extio_test renames the
                     * ain group, repoints its channels and leaves ain disabled;
                     * it silently ate a joystick bench setup on 2026-08-06).
                     *
                     * Sent with the '}' LENGTH-PREFIXED form, not the fixed 128-byte
                     * '>' frame, so the whole document arrives in one read with no
                     * reassembly and no "is more coming" question. That bypasses
                     * box_pub's queue, which is hardcoded to fixed frames -- see
                     * dserv_msg_var_build(). */
    CFG_STATS_RESET,/* cmd/stats/reset -> zero the SINCE-BOOT diagnostic counters
                     * (ain sweeps/blocks/dropped/late + late gaps, adc
                     * sweep_max_us, eth send_max_us). Rates can be had by
                     * diffing counters over a window, but the high-water marks
                     * only ever climb, so one bad millisecond during an OTA
                     * hours ago is indistinguishable from a problem happening
                     * now. Zeroing at the start of a session makes what you
                     * read afterwards describe THAT session. loop_max_us and
                     * disp_max_us are already per-publish-window and are left
                     * alone. Rebooting also clears these, but costs PTP lock,
                     * re-registration, and the accumulated `late` history that
                     * is the most useful thing here. */
    CFG_UNKNOWN     /* under this box's name but unrecognized */
} cfg_result_t;

/* Build "<BOX_CLASS>/<name>/state/<leaf>" for datapoints the box PUBLISHES. */
static inline void dserv_state_name(const box_config_t *c, char *buf, int sz,
                                    const char *leaf);

static inline const char *dserv_cfg_name(const box_config_t *c)
{ return c->name[0] ? c->name : "box"; }

static inline uint16_t dserv_cfg_port(const box_config_t *c)
{ return c->dserv_port ? c->dserv_port : DSERV_DEFAULT_PORT; }

/* The update channel this box tracks; "" (factory/pre-v19) => "dev". */
static inline const char *dserv_cfg_channel(const box_config_t *c)
{ return c->channel[0] ? c->channel : "dev"; }

/* The box's datapoint prefix "<BOX_CLASS>/<name>" (e.g. extio/office). Returns len. */
static inline int dserv_cfg_prefix(const box_config_t *c, char *buf, int sz)
{ return snprintf(buf, sz, "%s/%s", BOX_CLASS, dserv_cfg_name(c)); }

/* ---- analog (MCP3204) groups ---- */
/* base scan rate: 0 => 50 Hz default. */
static inline int dserv_cfg_ain_rate(const box_config_t *c)
{ return c->ain_rate ? c->ain_rate : 50; }

/* The hardware-average COUNT (1..128) the exponent encodes. */
/* Blocks per second group `g` will publish, from config alone.
 *
 * Continuous is arithmetic: rate / decimate / batch. On-change is driven by
 * input noise and cannot be predicted, so it counts as its own cap -- the
 * honest worst case, which is what a ceiling has to be sized against. */
static inline int ain_group_bps(const box_config_t *c, int g)
{
    if (g < 0 || g >= BOX_NAGROUPS || !c->ain_group_chans[g]) return 0;

    int rate = dserv_cfg_ain_rate(c);

    if (!c->ain_group_mode[g]) {
        return rate < AIN_MAX_BLOCKS_PER_S ? rate : AIN_MAX_BLOCKS_PER_S;
    }
    int dec = c->ain_group_decimate[g] ? c->ain_group_decimate[g] : 1;
    int bat = c->ain_group_batch[g] ? c->ain_group_batch[g] : 1;
    int cap = ain_batch_cap(c, g);

    if (bat > cap) bat = cap;                    /* what the sampler will use */
    int bps = rate / (dec * bat);
    return bps < 1 ? 1 : bps;
}

static inline int ain_total_bps(const box_config_t *c)
{
    int t = 0;

    for (int g = 0; g < BOX_NAGROUPS; g++) t += ain_group_bps(c, g);
    return t;
}

/* Apply-check-revert. Every setting that moves the block rate goes through
 * this, so the refusal lands where the operator made the change rather than as
 * missing data an hour later. Reverting is how one helper can guard fields of
 * different types without a scratch copy of a 1 KB struct. */
#define AIN_TRY_BPS(c, field, val, saveT)                                  \
    do {                                                                   \
        saveT _old = (field);                                              \
        (field) = (val);                                                   \
        if (ain_total_bps(c) > AIN_MAX_TOTAL_BPS) {                        \
            (field) = _old;                                                \
            return CFG_UNKNOWN;                                            \
        }                                                                  \
    } while (0)

static inline int dserv_cfg_ain_ovs_count(const box_config_t *c)
{ return 1 << (c->ain_ovs & 7); }

/* number of active analog groups (chans mask non-zero). */
static inline int dserv_ain_active_count(const box_config_t *c)
{ int n = 0; for (int g = 0; g < BOX_NAGROUPS; g++) if (c->ain_group_chans[g]) n++; return n; }

/* leaf name for group g: "ain/<label>", or "ain/a<g>" when unlabeled. */
static inline void dserv_ain_group_leaf(const box_config_t *c, int g, char *buf, int sz)
{ if (c->ain_group_label[g][0]) snprintf(buf, sz, "ain/%s", c->ain_group_label[g]);
  else                          snprintf(buf, sz, "ain/a%d", g); }

/* bare name segment of analog group g: label, or "a<g>" while unlabeled --
 * the <label> in BOTH ain/<label> (data blocks, above) and the manifest's
 * ain/group/<label>/* leaves (box_announce.c). One helper so the two can
 * never disagree about what a group is called. */
static inline void dserv_ain_group_name(const box_config_t *c, int g, char *buf, int sz)
{ if (c->ain_group_label[g][0]) snprintf(buf, sz, "%s", c->ain_group_label[g]);
  else                          snprintf(buf, sz, "a%d", g); }

/* If the ADC is enabled but no group is configured, synthesize group 0 as the
 * 2-axis joystick ({0,1}, on-change, deadband 8) so the stick works out of the
 * box. RAM-only default (a later `save` persists it, which is fine); a box that
 * has ANY group defined is left untouched. Call once after config load. */
static inline void dserv_cfg_ain_default(box_config_t *c)
{
    if (!c->ain_en || dserv_ain_active_count(c)) return;
    c->ain_group_chans[0]    = 0x03;   /* ch0 = X, ch1 = Y */
    c->ain_group_mode[0]     = 0;      /* on-change */
    c->ain_group_deadband[0] = 8;
    if (!c->ain_group_label[0][0]) snprintf(c->ain_group_label[0], BOX_LABEL_MAX, "joystick");
}

/* per-pin active-low (invert published DI level). */
static inline int  di_active_low(const box_config_t *c, int pin)
{ return (pin >= 0 && pin < BOX_NPINS) ? (int) ((c->di_active_low >> pin) & 1u) : 0; }
static inline void di_active_low_set(box_config_t *c, int pin, int on)
{ if (pin < 0 || pin >= BOX_NPINS) return;
  if (on) c->di_active_low |= (1u << pin); else c->di_active_low &= ~(1u << pin); }

/* logical DI level for publishing/grouping: active_low pins read pressed=1. */
static inline int di_logical(const box_config_t *c, int pin, int raw)
{ return di_active_low(c, pin) ? !raw : (raw != 0); }

/* Labels ride inside datapoint names and host-side name algebra: printable,
 * no '/' and no spaces. Empty is valid (= cleared/none). */
static inline int dserv_label_valid(const char *s)
{
    for (; *s; s++)
        if ((unsigned char) *s <= 0x20 || (unsigned char) *s > 0x7e || *s == '/') return 0;
    return 1;
}

/* "2,3,4,5" -> pin bitmask. Returns the member count, or -1 on a bad token or
 * out-of-range pin. "off" (or empty) -> mask 0, count 0. */
static inline int dserv_parse_pins(const char *s, uint32_t *mask)
{
    *mask = 0;
    if (!*s || !strcmp(s, "off")) return 0;
    int n = 0;
    while (*s) {
        char *end; long p = strtol(s, &end, 10);
        if (end == s || p < 0 || p >= BOX_NPINS) return -1;
        *mask |= (1u << p); n++;
        s = end;
        if (*s == ',') s++;
        else if (*s) return -1;
    }
    return n;
}

/* pin bitmask -> "2,3,4,5", ascending -- the published bit order (bit i =
 * i-th lowest member pin), so this string IS the wire-format contract. */
static inline void dserv_pins_str(uint32_t mask, char *buf, int sz)
{
    int k = 0;
    if (sz <= 0) return;
    buf[0] = '\0';
    for (int i = 0; i < BOX_NPINS && k < sz - 1; i++)
        if ((mask >> i) & 1u)
            k += snprintf(buf + k, sz - k, "%s%d", k ? "," : "", i);
}

/* group's datapoint segment: its label, or "g<idx>" while unlabeled. */
static inline void dserv_group_name(const box_config_t *c, int g, char *buf, int sz)
{
    if (c->group_label[g][0]) snprintf(buf, sz, "%s", c->group_label[g]);
    else                      snprintf(buf, sz, "g%d", g);
}

/* pin mode word -> value; -1 if not a recognized word (caller falls back to int) */
#define PIN_MODE_AIN 4     /* pin is an ADC input, not digital I/O */

static inline int dserv_mode_val(const char *w)
{
    if (!strcmp(w, "out"))       return 1;
    if (!strcmp(w, "in"))        return 2;
    if (!strcmp(w, "in_pullup")) return 3;
    if (!strcmp(w, "ain"))       return PIN_MODE_AIN;
    if (!strcmp(w, "off") || !strcmp(w, "unset")) return 0;
    return -1;
}
static inline const char *dserv_mode_str(uint8_t m)
{ return m == 1 ? "out" : m == 2 ? "in" : m == 3 ? "in_pullup"
       : m == PIN_MODE_AIN ? "ain" : "off"; }

/* A pin handed to the ADC. The pad is muxed away from GPIO (on MCX that is
 * PCR[MUX]=0, kPORT_PinDisabledOrAnalog, which Zephyr reaches via
 * GPIO_DISCONNECTED), and the ain channel bound to it becomes samplable.
 * Which pins CAN do this is a board property -- see zephyr,user/box-ain-pins --
 * but WHETHER they do is runtime config like every other setting on the box. */
static inline int pin_is_ain(const box_config_t *c, int n)
{ return (n >= 0 && n < BOX_NPINS && c->pin_mode[n] == PIN_MODE_AIN); }

static inline const char *dserv_netmode_str(uint8_t mode)
{ return mode == NET_MODE_STATIC ? "static" : "dhcp"; }

/* Transport backends for the DUAL build. XPORT_* is the ACTIVE transport; the
 * boot policy is: GP28 strap to GND -> Ethernet, hardware-forced (can't go
 * stale). Strap open -> cfg->transport_mode decides: XMODE_ETH / XMODE_USB
 * force one, XMODE_AUTO (factory default) boots USB immediately -- the safe,
 * always-works transport -- then senses the W6300 PHY link (debounced, after
 * autonegotiation has had time to finish; the old cold-boot instant read was
 * what made auto-detect unreliable) and upgrades to Ethernet when a live cable
 * is seen. dserv_xport_str logs the active transport. */
enum { XPORT_USB = 0, XPORT_ETH = 1, XPORT_SWITCH = 2 };
static inline const char *dserv_xport_str(uint8_t m)
{ return m == XPORT_ETH ? "eth" : m == XPORT_SWITCH ? "switch" : "usb"; }

/* box_config_t.transport_mode (persisted policy; 0 default == auto). */
enum { XMODE_AUTO = 0, XMODE_ETH = 1, XMODE_USB = 2 };
static inline const char *dserv_xmode_str(uint8_t m)
{ return m == XMODE_ETH ? "eth" : m == XMODE_USB ? "usb" : "auto"; }

/* console binding: 0 = the USB console CDC (convenient), 1 = the board's console
 * UART (better timing). See box_config_t.console_mode. */
#define CONSOLE_MODE_CDC   0
#define CONSOLE_MODE_UART  1

/* Which console a FRESH config binds to (no saved blob, or after `factory`).
 *
 * PER BOARD, because the ergonomics are not comparable. On the FRDM-MCXN947 the
 * console UART is the MCU-Link VCOM -- the same single cable that POWERS the
 * board -- so a box straight out of the box shows its config console with
 * nothing else attached, which is exactly what someone bringing one up at a new
 * location needs. On the Teensy the same setting means lpuart6 on pins 0/1 and
 * a SOLDERED-ON USB-serial adapter, so defaulting to uart there would make a
 * perfectly healthy box look dead.
 *
 * Boards opt in via -DBOX_DEFAULT_CONSOLE_MODE (see CMakeLists.txt); everything
 * else keeps the CDC default it has always had. */
#ifndef BOX_DEFAULT_CONSOLE_MODE
#define BOX_DEFAULT_CONSOLE_MODE CONSOLE_MODE_CDC
#endif
static inline int         dserv_cfg_console_mode(const box_config_t *c)
{ return c->console_mode == CONSOLE_MODE_UART ? CONSOLE_MODE_UART : CONSOLE_MODE_CDC; }
static inline const char *dserv_console_str(uint8_t m)
{ return m == CONSOLE_MODE_UART ? "uart" : "cdc"; }
static inline int         dserv_console_val(const char *w)
{ return !strcmp(w, "cdc") ? CONSOLE_MODE_CDC : (!strcmp(w, "uart") ? CONSOLE_MODE_UART : -1); }

/* obs-mirror accessors: enable is a flag distinct from the pin, so GP0 is a
 * valid mirror pin and the zeroed factory default is "off". */
static inline int  obs_mirror_enabled(const box_config_t *c) { return c->obs_en != 0; }
static inline int  obs_mirror_pin(const box_config_t *c)     { return (int) c->obs_pin; }
static inline int  obs_is_leader(const box_config_t *c)
{ return c->obs_en != 0 && c->obs_mode == OBS_MODE_LEADER; }
static inline void obs_mirror_set(box_config_t *c, int gpio) { c->obs_pin = (uint8_t) gpio; c->obs_en = 1; }
static inline void obs_mirror_off(box_config_t *c)          { c->obs_en = 0; }

/* hardware obs-sync input accessors (TTL from the rig host), same shape. */
static inline int  sync_input_enabled(const box_config_t *c) { return c->sync_en != 0; }
static inline int  sync_input_pin(const box_config_t *c)     { return (int) c->sync_pin; }
static inline void sync_input_set(box_config_t *c, int gpio) { c->sync_pin = (uint8_t) gpio; c->sync_en = 1; }
static inline void sync_input_off(box_config_t *c)           { c->sync_en = 0; }

/* Is box pin `n` DRIVEN BY THE BOX as an output, i.e. is a `do` on it meaningful?
 *
 * THIS MIRRORS box_gpio_apply_config's ORDER, and the order is the whole content
 * of the answer -- pin_mode is applied first, then the obs mirror forces its pin
 * to an output regardless of mode, then the sync input forces ITS pin to an
 * input regardless of everything. So a pin that is both `mode out` and the sync
 * pin is an INPUT, and the obs pin is an output even at `mode off`. Anywhere
 * that re-derives this from pin_mode alone will be wrong about both.
 *
 * The obs pin answering yes is not a technicality: the scheduled-onset path
 * (`cmd/do/<pin>/at_abs`, from ess's obs_schedule_bind) drives exactly that pin,
 * and it is normal for its pin_mode to be unset because the obs config owns it.
 *
 * Reserved pins are a PLATFORM fact (box_gpio_reserved / box_cli_pin_reserved),
 * not a config one, so they are checked separately at each call site. */
static inline int pin_is_output(const box_config_t *c, int n)
{
    if (n < 0 || n >= BOX_NPINS)                              return 0;
    if (sync_input_enabled(c) && sync_input_pin(c) == n)      return 0;
    if (obs_mirror_enabled(c) && obs_mirror_pin(c) == n)      return 1;
    return c->pin_mode[n] == 1;
}

/* The device name is a datapoint-prefix: it must be non-empty, printable, and
 * carry no '/' (or a stray control byte from line-editing) -- a bad name breaks
 * the config/cmd/state namespace match silently. Hyphens are fine. */
static inline int dserv_name_valid(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) if ((unsigned char) *s < 0x20 || (unsigned char) *s > 0x7e || *s == '/') return 0;
    return 1;
}

static inline int dserv_cfg__parse_ip(const char *s, uint8_t out[4])
{
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -1;
    if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
    out[0]=(uint8_t)a; out[1]=(uint8_t)b; out[2]=(uint8_t)c; out[3]=(uint8_t)d;
    return 0;
}

static inline cfg_result_t dserv_cfg__config(box_config_t *c, const char *k,
                                             const dserv_msg_t *m)
{
    int n, pos = -1;
    if (sscanf(k, "pin/%d/mode%n", &n, &pos) == 1 && pos > 0 &&
        k[pos] == '\0' && n >= 0 && n < BOX_NPINS) {
        char w[16]; dserv_msg_copy_cstr(m, w, sizeof w);   /* accept word or int */
        int mv = dserv_mode_val(w);
        if (mv < 0) mv = (int) dserv_msg_as_long(m);
        c->pin_mode[n] = (uint8_t) mv; c->applied_count++; return CFG_PIN_MODE;
    }
    pos = -1;
    if (sscanf(k, "pin/%d/pulse_us%n", &n, &pos) == 1 && pos > 0 &&
        k[pos] == '\0' && n >= 0 && n < BOX_NPINS) {
        c->do_pulse_us[n] = (uint32_t) dserv_msg_as_long(m); c->applied_count++; return CFG_DO_PULSE;
    }
    pos = -1;
    if (sscanf(k, "pin/%d/debounce_ms%n", &n, &pos) == 1 && pos > 0 &&
        k[pos] == '\0' && n >= 0 && n < BOX_NPINS) {
        long v = dserv_msg_as_long(m); if (v < 0) v = 0; if (v > 255) v = 255;
        c->debounce_ms[n] = (uint8_t) v; c->applied_count++; return CFG_DEBOUNCE;
    }
    pos = -1;
    if (sscanf(k, "pin/%d/active_low%n", &n, &pos) == 1 && pos > 0 &&
        k[pos] == '\0' && n >= 0 && n < BOX_NPINS) {
        di_active_low_set(c, n, dserv_msg_as_long(m) ? 1 : 0); c->applied_count++; return CFG_ACTIVE_LOW;
    }
    pos = -1;
    if (sscanf(k, "pin/%d/label%n", &n, &pos) == 1 && pos > 0 &&
        k[pos] == '\0' && n >= 0 && n < BOX_NPINS) {
        char w[BOX_LABEL_MAX]; dserv_msg_copy_cstr(m, w, sizeof w);
        if (!strcmp(w, "off")) w[0] = '\0';
        if (!dserv_label_valid(w)) return CFG_UNKNOWN;
        snprintf(c->pin_label[n], BOX_LABEL_MAX, "%s", w);
        c->applied_count++; return CFG_LABEL;
    }
    pos = -1;
    if (sscanf(k, "group/%d/pins%n", &n, &pos) == 1 && pos > 0 &&
        k[pos] == '\0' && n >= 0 && n < BOX_NGROUPS) {
        char w[96]; dserv_msg_copy_cstr(m, w, sizeof w);
        uint32_t mask;
        if (dserv_parse_pins(w, &mask) < 0) return CFG_UNKNOWN;
        c->group_pins[n] = mask; c->applied_count++; return CFG_GROUP;
    }
    pos = -1;
    if (sscanf(k, "group/%d/label%n", &n, &pos) == 1 && pos > 0 &&
        k[pos] == '\0' && n >= 0 && n < BOX_NGROUPS) {
        char w[BOX_LABEL_MAX]; dserv_msg_copy_cstr(m, w, sizeof w);
        if (!strcmp(w, "off")) w[0] = '\0';
        if (!dserv_label_valid(w)) return CFG_UNKNOWN;
        snprintf(c->group_label[n], BOX_LABEL_MAX, "%s", w);
        c->applied_count++; return CFG_GROUP;
    }
    pos = -1;
    if (sscanf(k, "group/%d/settle_ms%n", &n, &pos) == 1 && pos > 0 &&
        k[pos] == '\0' && n >= 0 && n < BOX_NGROUPS) {
        long v = dserv_msg_as_long(m); if (v < 0) v = 0; if (v > 65535) v = 65535;
        c->group_settle_ms[n] = (uint16_t) v; c->applied_count++; return CFG_GROUP;
    }
    pos = -1;
    if (sscanf(k, "group/%d/quiet%n", &n, &pos) == 1 && pos > 0 &&
        k[pos] == '\0' && n >= 0 && n < BOX_NGROUPS) {
        c->group_quiet[n] = dserv_msg_as_long(m) ? 1 : 0; c->applied_count++; return CFG_GROUP;
    }
    if (strcmp(k, "desc") == 0) {
        dserv_msg_copy_cstr(m, c->desc, sizeof c->desc); c->applied_count++; return CFG_DESC;
    }
    if (strcmp(k, "name") == 0) {
        char w[BOX_NAME_MAX]; dserv_msg_copy_cstr(m, w, sizeof w);
        if (!dserv_name_valid(w)) return CFG_UNKNOWN;
        strncpy(c->name, w, sizeof c->name - 1); c->name[sizeof c->name - 1] = '\0';
        c->applied_count++; return CFG_NAME;
    }
    if (strcmp(k, "net/ip") == 0) {
        char ip[24]; dserv_msg_copy_cstr(m, ip, sizeof ip);
        if (dserv_cfg__parse_ip(ip, c->net_ip) == 0) { c->applied_count++; return CFG_NET_IP; }
        return CFG_UNKNOWN;
    }
    if (strcmp(k, "net/gateway") == 0) {   /* static gateway; 0.0.0.0 => auto-derive */
        char ip[24]; dserv_msg_copy_cstr(m, ip, sizeof ip);
        if (dserv_cfg__parse_ip(ip, c->net_gw) == 0) { c->applied_count++; return CFG_NET_IP; }
        return CFG_UNKNOWN;
    }
    if (strcmp(k, "net/mask") == 0) {      /* static subnet mask; 0.0.0.0 => /24 */
        char ip[24]; dserv_msg_copy_cstr(m, ip, sizeof ip);
        if (dserv_cfg__parse_ip(ip, c->net_sn) == 0) { c->applied_count++; return CFG_NET_IP; }
        return CFG_UNKNOWN;
    }
    if (strcmp(k, "net/mode") == 0) {
        char w[12]; dserv_msg_copy_cstr(m, w, sizeof w);
        if      (!strcmp(w, "dhcp"))   c->net_mode = NET_MODE_DHCP;
        else if (!strcmp(w, "static")) c->net_mode = NET_MODE_STATIC;
        else c->net_mode = dserv_msg_as_long(m) ? NET_MODE_STATIC : NET_MODE_DHCP;
        c->applied_count++; return CFG_NET_MODE;
    }
    if (strcmp(k, "dserv/ip") == 0) {
        char ip[24]; dserv_msg_copy_cstr(m, ip, sizeof ip);
        if (dserv_cfg__parse_ip(ip, c->dserv_ip) == 0) { c->applied_count++; return CFG_DSERV_IP; }
        return CFG_UNKNOWN;
    }
    if (strcmp(k, "dserv/port") == 0) {
        c->dserv_port = (uint16_t) dserv_msg_as_long(m); c->applied_count++; return CFG_DSERV_PORT;
    }
    if (strcmp(k, "wifi/ssid") == 0) {
        dserv_msg_copy_cstr(m, c->wifi_ssid, sizeof c->wifi_ssid); c->applied_count++; return CFG_WIFI_SSID;
    }
    if (strcmp(k, "wifi/pass") == 0) {
        dserv_msg_copy_cstr(m, c->wifi_pass, sizeof c->wifi_pass); c->applied_count++; return CFG_WIFI_PASS;
    }
    if (strcmp(k, "wifi/pm") == 0) {
        c->wifi_pm = dserv_msg_as_long(m) ? 1 : 0; c->applied_count++; return CFG_WIFI_PM;
    }
    if (strcmp(k, "ain/enable") == 0) {    /* analog input master switch (applied at boot) */
        c->ain_en = dserv_msg_as_long(m) ? 1 : 0; c->applied_count++; return CFG_AIN_EN;
    }
    if (strcmp(k, "ain/rate") == 0) {      /* box-wide base SCAN rate Hz (groups decimate from it) */
        long v = dserv_msg_as_long(m); if (v < 1) v = 1; if (v > 65535) v = 65535;
        AIN_TRY_BPS(c, c->ain_rate, (uint16_t) v, uint16_t);
        c->applied_count++; return CFG_AIN;
    }
    if (strcmp(k, "ain/oversample") == 0) { /* hw conversions averaged per trigger: 1,2,4,...,128 */
        long v = dserv_msg_as_long(m);
        uint8_t e = 0;
        while ((1 << e) < v && e < 7) e++;
        if (v < 1 || (1 << e) != v) return CFG_UNKNOWN;   /* exact powers of two only */
        c->ain_ovs = e; c->applied_count++; return CFG_AIN;
    }
    if (strcmp(k, "ain/clk_ppm") == 0) {   /* CTIMER source deviation, signed ppm */
        long v = dserv_msg_as_long(m);
        if (v < -32768 || v > 32767) return CFG_UNKNOWN;
        c->ain_clk_ppm = (int16_t) v; c->applied_count++; return CFG_AIN;
    }
    if (strcmp(k, "dbg/level") == 0) {   /* health | full | off -- see dbg_level */
        char w[16]; dserv_msg_copy_cstr(m, w, sizeof w);
        if (!strcmp(w, "health"))      c->dbg_level = DBG_LEVEL_HEALTH;
        else if (!strcmp(w, "full"))   c->dbg_level = DBG_LEVEL_FULL;
        else if (!strcmp(w, "off"))    c->dbg_level = DBG_LEVEL_OFF;
        else return CFG_UNKNOWN;
        c->applied_count++; return CFG_AIN;
    }
    if (strcmp(k, "ain/pace") == 0) {      /* auto | polled | stream -- see ain_pace */
        char w[16]; dserv_msg_copy_cstr(m, w, sizeof w);
        if (!strcmp(w, "auto"))        c->ain_pace = AIN_PACE_AUTO;
        else if (!strcmp(w, "polled")) c->ain_pace = AIN_PACE_POLLED;
        else if (!strcmp(w, "stream")) c->ain_pace = AIN_PACE_STREAM;
        else return CFG_UNKNOWN;
        c->applied_count++; return CFG_AIN;
    }
    /* Analog groups: ain/group/<g>/{channels,label,mode,deadband,decimate,batch,average,off} */
    { int ng = -1; int npos = -1;
      if (sscanf(k, "ain/group/%d/%n", &ng, &npos) == 1 && npos > 0 &&
          ng >= 0 && ng < BOX_NAGROUPS) {
        const char *sub = k + npos;
        if (strcmp(sub, "channels") == 0) {
            char w[32]; dserv_msg_copy_cstr(m, w, sizeof w);
            uint32_t mask;
            /* 0xFF, not 0x0F: the ADC 20 Click (TLA2518) is 8-channel, and a
             * hardcoded 4-channel limit here would reject its upper half with a
             * bare CFG_UNKNOWN. box_ain masks the request down to what the part
             * actually reports, so asking for a channel that is not fitted is
             * ignored rather than refused -- the driver is the authority on
             * channel count, not this parser. */
            if (dserv_parse_pins(w, &mask) < 0 || (mask & ~0xFFu)) return CFG_UNKNOWN;
            {
                uint8_t oldm = c->ain_group_chans[ng];
                uint8_t oldb = c->ain_group_batch[ng];

                c->ain_group_chans[ng] = (uint8_t) mask;
                (void) ain_batch_reclamp(c, ng);
                if (ain_total_bps(c) > AIN_MAX_TOTAL_BPS) {
                    c->ain_group_chans[ng] = oldm;
                    c->ain_group_batch[ng] = oldb;
                    return CFG_UNKNOWN;
                }
            }
            c->applied_count++; return CFG_AIN;
        }
        if (strcmp(sub, "label") == 0) {
            char w[BOX_LABEL_MAX]; dserv_msg_copy_cstr(m, w, sizeof w);
            if (!strcmp(w, "off")) w[0] = '\0';
            if (!dserv_label_valid(w)) return CFG_UNKNOWN;
            snprintf(c->ain_group_label[ng], BOX_LABEL_MAX, "%s", w);
            c->applied_count++; return CFG_AIN;
        }
        if (strcmp(sub, "mode") == 0) {
            char w[16]; dserv_msg_copy_cstr(m, w, sizeof w);
            {
                uint8_t nm;

                if (!strcmp(w, "continuous")) nm = 1;
                else if (!strcmp(w, "onchange")) nm = 0;
                else nm = dserv_msg_as_long(m) ? 1 : 0;
                AIN_TRY_BPS(c, c->ain_group_mode[ng], nm, uint8_t);
            }
            c->applied_count++; return CFG_AIN;
        }
        if (strcmp(sub, "deadband") == 0) {
            long v = dserv_msg_as_long(m); if (v < 0) v = 0; if (v > 4095) v = 4095;
            c->ain_group_deadband[ng] = (uint16_t) v; c->applied_count++; return CFG_AIN;
        }
        if (strcmp(sub, "decimate") == 0) {
            long v = dserv_msg_as_long(m); if (v < 1) v = 1; if (v > 255) v = 255;
            AIN_TRY_BPS(c, c->ain_group_decimate[ng], (uint8_t) v, uint8_t);
            c->applied_count++; return CFG_AIN;
        }
        if (strcmp(sub, "batch") == 0) {
            long v = dserv_msg_as_long(m); if (v < 1) v = 1; if (v > 255) v = 255;
            if (v > ain_batch_cap(c, ng)) return CFG_UNKNOWN;   /* batch*nchan <= AIN_BLOCK_MAX */
            AIN_TRY_BPS(c, c->ain_group_batch[ng], (uint8_t) v, uint8_t);
            c->applied_count++; return CFG_AIN;
        }
        if (strcmp(sub, "average") == 0) {
            if (dserv_msg_as_long(m)) c->ain_group_flags[ng] |= AIN_GROUP_FLAG_AVG;
            else                      c->ain_group_flags[ng] &= (uint8_t) ~AIN_GROUP_FLAG_AVG;
            c->applied_count++; return CFG_AIN;
        }
        if (strcmp(sub, "off") == 0) {
            c->ain_group_chans[ng] = 0; c->ain_group_label[ng][0] = '\0';
            c->applied_count++; return CFG_AIN;
        }
      }
    }
    if (strcmp(k, "oled/enable") == 0) {
        c->oled_en = dserv_msg_as_long(m) ? 1 : 0; c->applied_count++; return CFG_OLED_EN;
    }
    if (strcmp(k, "ble/enable") == 0) {
        c->ble_en = dserv_msg_as_long(m) ? 1 : 0; c->applied_count++; return CFG_BLE_EN;
    }
    if (strcmp(k, "ble/pipe") == 0) {      /* receiver relay latch; on_frame fires the live request */
        c->pipe_en = dserv_msg_as_long(m) ? 1 : 0; c->applied_count++; return CFG_PIPE_EN;
    }
    if (strcmp(k, "ble/latency") == 0) {   /* idle peripheral latency; box_ble_latency_service reads it live */
        long v = dserv_msg_as_long(m); if (v < 0) v = 0; if (v > 30) v = 30;
        c->ble_latency = (uint8_t) v; c->applied_count++; return CFG_BLE_LATENCY;
    }
    if (strcmp(k, "console") == 0) {          /* cdc|uart; save+reboot to apply */
        char w[8]; dserv_msg_copy_cstr(m, w, sizeof w);
        int v = dserv_console_val(w);
        if (v < 0) return CFG_UNKNOWN;
        c->console_mode = (uint8_t) v; c->applied_count++; return CFG_CONSOLE;
    }
    if (strcmp(k, "xport/mode") == 0) {       /* auto|usb|eth transport boot
        * policy -- the datapoint twin of the CLI `mode` verb. Was CLI-only
        * through v34, so the web config page (datapoint-only) could not set
        * it: the same latent bug obs/mode had, one surface over. Persisted
        * policy, applied at the NEXT boot (strap-open decides then); GND
        * strap still hard-forces eth. */
        char w[8]; dserv_msg_copy_cstr(m, w, sizeof w);
        int v = !strcmp(w, "auto") ? XMODE_AUTO :
                !strcmp(w, "eth")  ? XMODE_ETH  :
                !strcmp(w, "usb")  ? XMODE_USB  : -1;
        if (v < 0) return CFG_UNKNOWN;
        c->transport_mode = (uint8_t) v; c->applied_count++; return CFG_XPORT;
    }
    if (strcmp(k, "obs/pin") == 0) {
        char w[8]; dserv_msg_copy_cstr(m, w, sizeof w);
        if (m->type == DSERV_STRING && !strcmp(w, "off")) obs_mirror_off(c);
        else {
            long v = dserv_msg_as_long(m);
            if (v >= 0 && v < BOX_NPINS) obs_mirror_set(c, (int) v);  /* 0..29, GP0 ok */
            else obs_mirror_off(c);                                    /* out-of-range = off */
        }
        c->applied_count++; return CFG_OBS_PIN;
    }
    if (strcmp(k, "obs/mode") == 0) {
        char w[8]; dserv_msg_copy_cstr(m, w, sizeof w);
        if (m->type == DSERV_STRING && !strcmp(w, "leader")) {
            c->obs_mode = OBS_MODE_LEADER;
        } else if (m->type == DSERV_STRING && !strcmp(w, "mirror")) {
            c->obs_mode = OBS_MODE_MIRROR;
        } else {
            long v = dserv_msg_as_long(m);
            c->obs_mode = (v == 1) ? OBS_MODE_LEADER : OBS_MODE_MIRROR;
        }
        c->applied_count++; return CFG_OBS_MODE;
    }
    if (strcmp(k, "sync/pin") == 0) {
        char w[8]; dserv_msg_copy_cstr(m, w, sizeof w);
        if (m->type == DSERV_STRING && !strcmp(w, "off")) sync_input_off(c);
        else {
            long v = dserv_msg_as_long(m);
            if (v >= 0 && v < BOX_NPINS) sync_input_set(c, (int) v);
            else sync_input_off(c);
        }
        c->applied_count++; return CFG_SYNC_PIN;
    }
    return CFG_UNKNOWN;
}

/* cmd/ actions: do/<n> level, do/<n>/pulse_us pulse, save|reboot|factory */
static inline cfg_result_t dserv_cfg__cmd(const char *k, const dserv_msg_t *m,
                                          gpio_cmd_t *cmd)
{
    int n, pos = -1;
    if (sscanf(k, "do/%d%n", &n, &pos) == 1 && pos > 0 && k[pos] == '\0' &&
        n >= 0 && n < BOX_NPINS) {
        cmd->op = GPIO_OP_SET; cmd->pin = (uint8_t) n;
        cmd->value = dserv_msg_as_long(m) ? 1 : 0; return CFG_GPIO;
    }
    pos = -1;
    if (sscanf(k, "do/%d/pulse_us%n", &n, &pos) == 1 && pos > 0 && k[pos] == '\0' &&
        n >= 0 && n < BOX_NPINS) {
        cmd->op = GPIO_OP_PULSE; cmd->pin = (uint8_t) n;
        cmd->value = (uint32_t) dserv_msg_as_long(m); return CFG_GPIO;
    }
    pos = -1;   /* do/<n>/at <us> : schedule a pulse at beginobs + <us> */
    if (sscanf(k, "do/%d/at%n", &n, &pos) == 1 && pos > 0 && k[pos] == '\0' &&
        n >= 0 && n < BOX_NPINS) {
        cmd->op = GPIO_OP_SCHED_PULSE; cmd->pin = (uint8_t) n;
        cmd->value = (uint32_t) dserv_msg_as_long(m); return CFG_GPIO;
    }
    pos = -1;   /* timer/<n>/at <us> : post state/timer/<n> at beginobs + <us> */
    if (sscanf(k, "timer/%d/at%n", &n, &pos) == 1 && pos > 0 && k[pos] == '\0' &&
        n >= 0 && n < 64) {
        cmd->op = GPIO_OP_SCHED_TIMER; cmd->pin = (uint8_t) n;
        cmd->value = (uint32_t) dserv_msg_as_long(m); return CFG_GPIO;
    }
    if (strcmp(k, "announce") == 0) return CFG_ANNOUNCE;
    if (strcmp(k, "dump")        == 0) return CFG_DUMP;
    if (strcmp(k, "stats/reset") == 0) return CFG_STATS_RESET;
    if (strcmp(k, "save")    == 0) return CFG_SAVE;
    if (strcmp(k, "reboot")  == 0) return CFG_REBOOT;
    if (strcmp(k, "factory") == 0) return CFG_FACTORY;
    if (strcmp(k, "bootsel") == 0) return CFG_BOOTSEL;
    /* BLE pairing control, remoteable (BOX_BLE receiver): the value of cmd/ble/pair
     * is the window length in seconds; cmd/ble/forget takes any value. The firmware
     * (on_frame) drives the window/allowlist -- these keys are inert elsewhere. */
    if (strcmp(k, "ble/pair")   == 0) return CFG_BLE_PAIR;
    if (strcmp(k, "ble/forget") == 0) return CFG_BLE_FORGET;
    return CFG_UNKNOWN;
}

/* Route one datapoint. Config keys mutate *c; a gpio command fills *cmd (set
 * cmd->op == GPIO_OP_NONE first). Pass cmd=NULL if you only handle config. */
static inline cfg_result_t dserv_dispatch(box_config_t *c, const dserv_msg_t *m,
                                          gpio_cmd_t *cmd)
{
    if (cmd) cmd->op = GPIO_OP_NONE;

    char pfx[DSERV_MSG_MAX_PAYLOAD + 1];
    int plen = dserv_cfg_prefix(c, pfx, sizeof pfx);   /* "extio/<name>" */
    if (plen <= 0 || m->namelen < (uint16_t)(plen + 1)) return CFG_NONE;
    if (memcmp(m->name, pfx, (size_t) plen) != 0 || m->name[plen] != '/') return CFG_NONE;

    char sub[DSERV_MSG_MAX_PAYLOAD + 1];
    uint16_t sl = (uint16_t)(m->namelen - (plen + 1));
    if (sl > DSERV_MSG_MAX_PAYLOAD) sl = DSERV_MSG_MAX_PAYLOAD;
    memcpy(sub, m->name + plen + 1, sl); sub[sl] = '\0';

    if (strncmp(sub, "config/", 7) == 0) return dserv_cfg__config(c, sub + 7, m);
    if (strncmp(sub, "cmd/", 4) == 0 && cmd) return dserv_cfg__cmd(sub + 4, m, cmd);
    return CFG_UNKNOWN;
}

/* Config-only convenience (ignores gpio commands). */
static inline cfg_result_t dserv_config_apply(box_config_t *c, const dserv_msg_t *m)
{ gpio_cmd_t d; return dserv_dispatch(c, m, &d); }

static inline const char *dserv_cfg_result_str(cfg_result_t r)
{
    switch (r) {
    case CFG_NONE:       return "none";
    case CFG_NAME:       return "name";
    case CFG_PIN_MODE:   return "pin_mode";
    case CFG_DO_PULSE:   return "do_pulse";
    case CFG_DEBOUNCE:   return "debounce";
    case CFG_NET_IP:     return "net_ip";
    case CFG_NET_MODE:   return "net_mode";
    case CFG_OBS_PIN:    return "obs_pin";
    case CFG_OBS_MODE:   return "obs_mode";
    case CFG_SYNC_PIN:   return "sync_pin";
    case CFG_CONSOLE:    return "console";
    case CFG_XPORT:      return "xport_mode";
    case CFG_ACTIVE_LOW: return "active_low";
    case CFG_WIFI_SSID:  return "wifi_ssid";
    case CFG_WIFI_PASS:  return "wifi_pass";
    case CFG_WIFI_PM:    return "wifi_pm";
    case CFG_AIN_EN:     return "ain_en";
    case CFG_OLED_EN:    return "oled_en";
    case CFG_BLE_EN:     return "ble_en";
    case CFG_PIPE_EN:    return "pipe_en";
    case CFG_BLE_LATENCY:return "ble_latency";
    case CFG_DESC:       return "desc";
    case CFG_LABEL:      return "label";
    case CFG_GROUP:      return "group";
    case CFG_AIN:        return "ain";
    case CFG_DSERV_IP:   return "dserv_ip";
    case CFG_DSERV_PORT: return "dserv_port";
    case CFG_GPIO:       return "cmd_do";
    case CFG_ANNOUNCE:   return "announce";
    case CFG_DUMP:       return "dump";
    case CFG_STATS_RESET: return "stats_reset";
    case CFG_SAVE:       return "save";
    case CFG_REBOOT:     return "reboot";
    case CFG_FACTORY:    return "factory";
    case CFG_BOOTSEL:    return "bootsel";
    case CFG_BLE_PAIR:   return "ble_pair";
    case CFG_BLE_FORGET: return "ble_forget";
    default:             return "unknown";
    }
}

static inline void dserv_state_name(const box_config_t *c, char *buf, int sz,
                                    const char *leaf)
{ snprintf(buf, sz, "%s/%s/state/%s", BOX_CLASS, dserv_cfg_name(c), leaf); }

#endif /* DSERV_CONFIG_H */
