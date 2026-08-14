/*
 * box_cli.h -- portable line-oriented CLI for the box, used over USB-CDC on the
 * device (bootstrap before the network is up + recovery) and over stdin in the
 * simulator. Pure C. One command per line; writes a human response into `out`
 * and returns an action for the caller to perform platform-specific storage.
 *
 * Commands:
 *   help
 *   show
 *   mode auto|usb|eth    (dual image: boot transport policy when the GP28 strap
 *                         is open; strap to GND still hard-forces Ethernet)
 *   net mode dhcp|static
 *   net ip A.B.C.D       (also sets mode=static)
 *   wifi ssid SSID       (pico2w; runtime creds, else compile-time fallback)
 *   wifi pass PASS
 *   dserv ip A.B.C.D
 *   dserv port N
 *   pin N mode out|in|in_pullup|ain|off   (ain = hand the pad to the ADC)
 *   pin N pulse US
 *   desc TEXT...         (free-form box description; `desc off` clears)
 *   label N TEXT|off     (per-pin role label -> announced manifest)
 *   group G pins 2,3,4,5 (DI chord group; `group G off` clears)
 *   group G label NAME | group G settle MS | group G quiet 0|1
 *   save          -> CLI_SAVE   (caller persists to flash/file)
 *   factory       -> CLI_FACTORY(caller erases storage + resets cfg)
 *   reboot        -> CLI_REBOOT (caller resets the MCU)
 */
#ifndef BOX_CLI_H
#define BOX_CLI_H

#include "dserv_config.h"
#include <stdarg.h>          /* box_cli_dump_line's sink takes varargs */
#include <stdio.h>
#include <string.h>

/* ---- fitted analog channel count ----
 *
 * This used to be a hardcoded `mask & ~0x0Fu` -- four channels, the MCP3204's
 * width baked into the grammar. On a board with a different converter that is
 * simply a lie: the MCXN947's on-chip LPADC can declare six or eight, announces
 * the true number in `state/ain/dbg/chans`, and the CLI still answered "ch 0-3".
 *
 * The limit is now WHAT THE PLATFORM ACTUALLY FITTED, reported after
 * box_adc_init(). Two properties worth keeping:
 *   * it defaults to 4, so a build that never calls the setter behaves exactly
 *     as before (this file is a fork of the RP2350's pico_cli.h, not a shared
 *     compilation unit, but keeping the default identical keeps the GRAMMAR
 *     compatible, which is the part that is actually shared);
 *   * it is a CEILING FROM REALITY, not a widened constant. Simply allowing
 *     0-7 everywhere would let someone configure channels a fitted MCP3204 does
 *     not have, and the group would then publish nothing at all -- box_adc_sweep
 *     rejects the sweep, silently, every time. Refusing at the CLI is the only
 *     place the user finds out.
 *
 * NOTE this is a static in a header, which is safe only because box_cli.h is
 * included by exactly ONE translation unit per binary (box_console.c on the
 * device, tools/box_sim.c on the host). If that ever stops being true each TU
 * gets its own copy and only the one whose setter ran would be right -- move it
 * into a .c file at that point. The platform sets it via box_console.h rather
 * than including this header twice.
 */
#ifndef BOX_CLI_AIN_NCH_DEFAULT
#define BOX_CLI_AIN_NCH_DEFAULT 4      /* MCP3204 */
#endif
#define BOX_CLI_AIN_NCH_MAX     8      /* must not exceed AIN_MAX_CH */

static uint8_t box_cli_ain_nch = BOX_CLI_AIN_NCH_DEFAULT;

static inline void box_cli_set_ain_channels(int n)
{
	if (n < 1)                       n = BOX_CLI_AIN_NCH_DEFAULT;
	if (n > BOX_CLI_AIN_NCH_MAX)     n = BOX_CLI_AIN_NCH_MAX;
	box_cli_ain_nch = (uint8_t) n;
}

static inline uint32_t box_cli_ain_chmask(void)
{
	return (1u << box_cli_ain_nch) - 1u;
}

/* ---- pins the platform refuses ----
 *
 * Set from box_gpio_reserved() at boot, for the same reason as the channel
 * ceiling above: without it the CLI cheerfully accepts `pin 14 mode out` on a
 * pad that is wired to the ADC (or to the PHY), stores the mode, and prints it
 * back in `show` -- while box_gpio_apply_config() skips the pin and NOTHING
 * happens electrically. A config that reports a mode the hardware does not have
 * is the failure this project keeps having to re-learn; refuse it where the
 * user is looking. Default 0 = nothing reserved, so an unset platform behaves
 * exactly as before. */
static uint32_t box_cli_pin_rsv;

static inline void box_cli_set_reserved_pins(uint32_t mask) { box_cli_pin_rsv = mask; }
static inline int  box_cli_pin_reserved(int n)
{
	return (n >= 0 && n < 32) ? (int) ((box_cli_pin_rsv >> n) & 1u) : 0;
}

/* Refuse a change that would push the box past its total block-rate ceiling,
 * and SAY WHAT THE BUDGET IS. The number that matters to an operator is not
 * "2000" in the abstract but how much of it their current groups already
 * spend, so print both. */
static inline int box_cli_bps_ok(const box_config_t *c, char *out, int outsz)
{
    int bps = ain_total_bps(c);

    if (bps > AIN_MAX_TOTAL_BPS) {
        snprintf(out, outsz,
                 "ERR that would publish %d blocks/s; the box sustains %d "
                 "(sum over groups of rate/decimate/batch -- raise batch or decimate)\r\n",
                 bps, AIN_MAX_TOTAL_BPS);
        return 0;
    }
    return 1;
}

/* Refuse a `do` on a pin that is not an output, and SAY WHY.
 *
 * The firmware refuses it too (box_gpio_exec), but a refusal the operator only
 * discovers as a wire that never moves is barely better than the silent
 * override this replaces -- so the CLI names the actual mode. Same predicate as
 * the firmware, from dserv_config.h, so the two cannot drift: it honours the
 * obs-mirror override (that pin is an output at any mode -- the scheduled-onset
 * path drives it) and the sync input (an input even at `mode out`).
 *
 * Returns 1 to proceed, or 0 having written the error into `out`. */
static inline int box_cli_do_ok(const box_config_t *c, int n, char *out, int outsz)
{
    if (box_cli_pin_reserved(n)) {
        snprintf(out, outsz, "ERR pin %d is reserved by the board\r\n", n);
        return 0;
    }
    if (!pin_is_output(c, n)) {
        snprintf(out, outsz,
                 "ERR pin %d is '%s', not an output -- `pin %d mode out` first\r\n",
                 n, dserv_mode_str(c->pin_mode[n]), n);
        return 0;
    }
    return 1;
}

/* CLI_GROUP = labels/groups/desc changed: caller refreshes the group runtime
 * and re-announces the manifest (no GPIO re-apply needed).
 * CLI_OBS = obs mode changed: caller re-announces the obs role
 * (box_announce_obs_role), exactly as the datapoint CFG_OBS_MODE path does.
 * The platform-agnostic core cannot reach into box_announce.c, so it signals
 * and the platform caller acts -- same split as CLI_PIN. */
typedef enum { CLI_OK, CLI_ERR, CLI_PIN, CLI_GROUP, CLI_AIN, CLI_GPIO, CLI_OBS, CLI_SAVE, CLI_FACTORY, CLI_REBOOT, CLI_BOOTSEL } cli_action_t;

/* mode word<->value shared with dserv_config.h: dserv_mode_val / dserv_mode_str */

/* `mode auto|usb|eth` is meaningful on any image with more than one uplink to
 * arbitrate. On the Pico that is only the BOX_NET_DUAL build; on Zephyr it is any
 * board with networking compiled in, since the arbiter there always has both
 * candidates. Without this the Zephyr build had NO way to set transport_mode at
 * all -- it was permanently XMODE_AUTO, which made the declared-Ethernet
 * behaviours (including suppressing the USB data pipe) unreachable. */
#if defined(BOX_NET_DUAL) || defined(CONFIG_NETWORKING)
#define BOX_HAVE_XPORT_POLICY 1
#endif

static inline void box_cli_show(const box_config_t *c, char *out, int outsz)
{
    char obs[8], syn[8];
    if (obs_mirror_enabled(c))  snprintf(obs, sizeof obs, "%d", obs_mirror_pin(c));
    else                        snprintf(obs, sizeof obs, "off");
    if (sync_input_enabled(c))  snprintf(syn, sizeof syn, "%d", sync_input_pin(c));
    else                        snprintf(syn, sizeof syn, "off");
    int k = 0;
#ifdef BOX_HAVE_XPORT_POLICY
    k += snprintf(out + k, outsz - k, "mode=%s ", dserv_xmode_str(c->transport_mode));
#endif
    k += snprintf(out + k, outsz - k,
        "name=%s net.mode=%s net.ip=%u.%u.%u.%u net.gw=%u.%u.%u.%u net.mask=%u.%u.%u.%u dserv=%u.%u.%u.%u:%u obs.pin=%s sync.pin=%s wifi.ssid=%s wifi.pass=%s wifi.pm=%u ain.en=%u oled.en=%u ble.en=%u pipe.en=%u applied=%u\r\n",
        dserv_cfg_name(c), dserv_netmode_str(c->net_mode),
        c->net_ip[0], c->net_ip[1], c->net_ip[2], c->net_ip[3],
        c->net_gw[0], c->net_gw[1], c->net_gw[2], c->net_gw[3],
        c->net_sn[0], c->net_sn[1], c->net_sn[2], c->net_sn[3],
        c->dserv_ip[0], c->dserv_ip[1], c->dserv_ip[2], c->dserv_ip[3],
        dserv_cfg_port(c), obs, syn,   /* effective port (default when unset), not raw 0 */
        c->wifi_ssid[0] ? c->wifi_ssid : "(build)", c->wifi_pass[0] ? "set" : "(build)",
        c->wifi_pm, c->ain_en, c->oled_en, c->ble_en, c->pipe_en, c->applied_count);
    if (k < outsz - 24)
        k += snprintf(out + k, outsz - k, "  console=%s\r\n",
                      dserv_console_str((uint8_t) dserv_cfg_console_mode(c)));
    /* CONFIGURED role, not obs_is_leader() -- report the setting even when no
     * pin is set yet, so `obs mode leader` then `show` reflects the choice. */
    if (k < outsz - 24)
        k += snprintf(out + k, outsz - k, "  obs.mode=%s\r\n",
                      c->obs_mode == OBS_MODE_LEADER ? "leader" : "mirror");
    if (c->desc[0] && k < outsz - 8)
        k += snprintf(out + k, outsz - k, "  desc=%s\r\n", c->desc);
    for (int i = 0; i < BOX_NPINS && k < outsz - 64; i++)
        if (c->pin_mode[i])
            k += snprintf(out + k, outsz - k, "  pin%d=%s pulse=%uus debounce=%ums%s%s%s\r\n",
                          i, dserv_mode_str(c->pin_mode[i]), c->do_pulse_us[i], c->debounce_ms[i],
                          di_active_low(c, i) ? " active_low" : "",
                          c->pin_label[i][0] ? " label=" : "", c->pin_label[i]);
    for (int g = 0; g < BOX_NGROUPS && k < outsz - 96; g++)
        if (c->group_pins[g]) {
            char gn[BOX_LABEL_MAX + 4], ps[96];
            dserv_group_name(c, g, gn, sizeof gn);
            dserv_pins_str(c->group_pins[g], ps, sizeof ps);
            k += snprintf(out + k, outsz - k, "  group%d=%s pins=%s settle=%ums%s\r\n",
                          g, gn, ps, c->group_settle_ms[g], c->group_quiet[g] ? " quiet" : "");
        }
}

/* ---- where a dump's lines go ----
 *
 * `dump` was console-only, which made it useless for the case it is best at:
 * a host wanting to snapshot a box before something overwrites its config, and
 * put it back afterwards. (extio_test does exactly that -- it renames the ain
 * group, repoints its channels and leaves ain disabled, which silently ate a
 * joystick bench setup on 2026-08-06.) Reaching it needed a serial cable.
 *
 * So the body emits through DUMPF, which is either printf (the console verb,
 * unchanged) or a caller's sink (cmd/dump -> datapoints). One line per call,
 * already newline-terminated -- the sink strips the CRLF. */
typedef void (*box_cli_emit_fn)(void *ud, const char *line);

static box_cli_emit_fn box_cli_emit;      /* NULL = write to the console */
static void           *box_cli_emit_ud;

static inline void box_cli_dump_line(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (!box_cli_emit) {
		vprintf(fmt, ap);
		va_end(ap);
		return;
	}
	char line[128];
	int n = vsnprintf(line, sizeof line, fmt, ap);

	va_end(ap);
	if (n < 0) {
		return;
	}
	/* strip the trailing CRLF the console format carries */
	while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
		line[--n] = '\0';
	}
	if (n > 0) {
		box_cli_emit(box_cli_emit_ud, line);
	}
}
#define DUMPF(...) box_cli_dump_line(__VA_ARGS__)

/* TWO SYNTAXES, ONE WALK.
 *
 * CLI form pastes into a console. DP form is "<leaf> <value>" pairs naming the
 * config datapoint namespace exactly, so a host replays a saved config with
 * extio_cfg_set and needs no translation table of its own -- the mapping is
 * NOT mechanical (pulse -> pulse_us, settle -> settle_ms, `label N` ->
 * pin/N/label), and a host-side table would be a second copy of firmware
 * knowledge that drifts. The extio_cfg_writable allowlist already demonstrates
 * that failure mode: it listed `channel` for months while the codec had no
 * case for it.
 *
 * DUMP2 takes BOTH forms on the same line with the SAME arguments, so a new
 * dumped setting cannot compile without stating its leaf name. That is the
 * property worth having: the two forms cannot drift apart, because they are
 * written together or not at all. tools/box_sim.c --selftest then proves every
 * emitted leaf is one the codec actually accepts. */
static int box_cli_dump_form;                 /* set by box_cli_dump_to_form() */
#define DUMP2(cli, dp, ...) box_cli_dump_line(box_cli_dump_form ? (dp) : (cli), ##__VA_ARGS__)
#define DUMPCLI(...) do { if (!box_cli_dump_form) DUMPF(__VA_ARGS__); } while (0)

/* Emit the CLI commands that reproduce this config (only the non-default settings), so
 * pasting the output into a fresh box's console clones this setup. Ends with `save`.
 * Uses printf (not `out`) so a big config isn't bounded by the response buffer. Comment
 * (#) lines are ignored by box_cli_exec, so the whole capture pastes back cleanly. */
static inline void box_cli_dump(const box_config_t *c)
{
    DUMPCLI("# extio box config dump -- paste into a new box's console to clone this setup\r\n");
    if (box_cli_dump_form) DUMPF("# extio config (datapoint form): <leaf> <value> for extio_cfg_set\r\n");
    DUMPCLI("# (uncomment the next line to wipe the target's existing config first)\r\n");
    DUMPCLI("#factory\r\n");
    /* NAME IS IDENTITY, NOT CONFIGURATION, and in DP form it is actively
     * destructive: applying config/name mid-replay changes the box's own
     * datapoint prefix, so every following extio/<old>/config/* write in the
     * same batch is addressed to a name the box no longer answers to and is
     * dropped -- silently, since a prefix miss is CFG_NONE, not an error.
     * Caught by tools/box_sim.c --roundtrip, which saw exactly one setting
     * survive an import.
     *
     * Renaming is already a committed operation with its own re-registration
     * (extio_rename); a config import must not do it as a side effect. CLI form
     * keeps it -- a console paste is sequential, interactive, and the operator
     * chose the target. */
    if (c->name[0]) {
        DUMPCLI("name %s\r\n", c->name);
        if (box_cli_dump_form) DUMPF("# name %s   (identity: use extio_rename, not import)\r\n", c->name);
    }
#ifdef BOX_HAVE_XPORT_POLICY
    if (c->transport_mode)                DUMP2("mode %s\r\n", "xport/mode %s\r\n", dserv_xmode_str(c->transport_mode));
#endif
    if (c->net_mode == NET_MODE_STATIC) {
        DUMP2("net mode static\r\n", "net/mode static\r\n");
        DUMP2("net ip %u.%u.%u.%u\r\n", "net/ip %u.%u.%u.%u\r\n", c->net_ip[0], c->net_ip[1], c->net_ip[2], c->net_ip[3]);
        if (c->net_gw[0] || c->net_gw[1] || c->net_gw[2] || c->net_gw[3])
            DUMP2("net gateway %u.%u.%u.%u\r\n", "net/gateway %u.%u.%u.%u\r\n", c->net_gw[0], c->net_gw[1], c->net_gw[2], c->net_gw[3]);
        if (c->net_sn[0] || c->net_sn[1] || c->net_sn[2] || c->net_sn[3])
            DUMP2("net mask %u.%u.%u.%u\r\n", "net/mask %u.%u.%u.%u\r\n", c->net_sn[0], c->net_sn[1], c->net_sn[2], c->net_sn[3]);
    }
    if (c->dserv_ip[0] || c->dserv_ip[1] || c->dserv_ip[2] || c->dserv_ip[3])
        DUMP2("dserv ip %u.%u.%u.%u\r\n", "dserv/ip %u.%u.%u.%u\r\n", c->dserv_ip[0], c->dserv_ip[1], c->dserv_ip[2], c->dserv_ip[3]);
    if (c->dserv_port)                    DUMP2("dserv port %u\r\n", "dserv/port %u\r\n", c->dserv_port);
    for (int i = 0; i < BOX_NPINS; i++) {
        if (c->pin_mode[i])      DUMP2("pin %d mode %s\r\n", "pin/%d/mode %s\r\n", i, dserv_mode_str(c->pin_mode[i]));
        if (c->do_pulse_us[i])   DUMP2("pin %d pulse %u\r\n", "pin/%d/pulse_us %u\r\n", i, (unsigned) c->do_pulse_us[i]);
        if (c->debounce_ms[i])   DUMP2("pin %d debounce %u\r\n", "pin/%d/debounce_ms %u\r\n", i, c->debounce_ms[i]);
        if (di_active_low(c, i)) DUMP2("pin %d active_low 1\r\n", "pin/%d/active_low 1\r\n", i);
        if (c->pin_label[i][0])  DUMP2("label %d %s\r\n", "pin/%d/label %s\r\n", i, c->pin_label[i]);
    }
    if (c->desc[0])                       DUMP2("desc %s\r\n", "desc %s\r\n", c->desc);
    if (c->channel[0])                    DUMP2("channel %s\r\n", "channel %s\r\n", c->channel);
    for (int g = 0; g < BOX_NGROUPS; g++)
        if (c->group_pins[g]) {
            char ps[96]; dserv_pins_str(c->group_pins[g], ps, sizeof ps);
            DUMP2("group %d pins %s\r\n", "group/%d/pins %s\r\n", g, ps);
            if (c->group_label[g][0])     DUMP2("group %d label %s\r\n", "group/%d/label %s\r\n", g, c->group_label[g]);
            if (c->group_settle_ms[g])    DUMP2("group %d settle %u\r\n", "group/%d/settle_ms %u\r\n", g, c->group_settle_ms[g]);
            if (c->group_quiet[g])        DUMP2("group %d quiet 1\r\n", "group/%d/quiet 1\r\n", g);
        }
    if (obs_mirror_enabled(c))            DUMP2("obs pin %d\r\n", "obs/pin %d\r\n", obs_mirror_pin(c));
    if (c->obs_mode == OBS_MODE_LEADER)   DUMP2("obs mode leader\r\n", "obs/mode leader\r\n");
    if (sync_input_enabled(c))            DUMP2("sync pin %d\r\n", "sync/pin %d\r\n", sync_input_pin(c));
    if (c->wifi_ssid[0])                  DUMP2("wifi ssid %s\r\n", "wifi/ssid %s\r\n", c->wifi_ssid);
    if (c->wifi_pass[0])                  DUMPF("# wifi pass <re-enter manually; not dumped>\r\n");
    if (c->wifi_pm)                       DUMP2("wifi pm 1\r\n", "wifi/pm 1\r\n");
    if (c->ain_en)                        DUMP2("ain enable 1\r\n", "ain/enable 1\r\n");
    if (c->ain_rate)                      DUMP2("ain rate %u\r\n", "ain/rate %u\r\n", c->ain_rate);
    if (c->ain_ovs)                       DUMP2("ain oversample %u\r\n", "ain/oversample %u\r\n", 1u << c->ain_ovs);
    for (int i = 0; i < AIN_MAX_CH; i++)
        if (c->ain_label[i][0])           DUMP2("ain label %d %s\r\n", "ain/label/%d %s\r\n", i, c->ain_label[i]);
    if (c->ain_clk_ppm)                   DUMP2("ain clkppm %d\r\n", "ain/clk_ppm %d\r\n", c->ain_clk_ppm);
    if (c->dbg_level == DBG_LEVEL_FULL)   DUMP2("dbg level full\r\n", "dbg/level full\r\n");
    if (c->dbg_level == DBG_LEVEL_OFF)    DUMP2("dbg level off\r\n", "dbg/level off\r\n");
    if (c->ain_pace == AIN_PACE_POLLED)   DUMP2("ain pace polled\r\n", "ain/pace polled\r\n");
    if (c->ain_pace == AIN_PACE_STREAM)   DUMP2("ain pace stream\r\n", "ain/pace stream\r\n");
    for (int ag = 0; ag < BOX_NAGROUPS; ag++)
        if (c->ain_group_chans[ag]) {
            char cs[16]; dserv_pins_str(c->ain_group_chans[ag], cs, sizeof cs);
            DUMP2("ain group %d channels %s\r\n", "ain/group/%d/channels %s\r\n", ag, cs);
            if (c->ain_group_label[ag][0]) DUMP2("ain group %d label %s\r\n", "ain/group/%d/label %s\r\n", ag, c->ain_group_label[ag]);
            if (c->ain_group_mode[ag])     DUMP2("ain group %d mode continuous\r\n", "ain/group/%d/mode continuous\r\n", ag);
            if (c->ain_group_deadband[ag]) DUMP2("ain group %d deadband %u\r\n", "ain/group/%d/deadband %u\r\n", ag, c->ain_group_deadband[ag]);
            if (c->ain_group_decimate[ag]) DUMP2("ain group %d decimate %u\r\n", "ain/group/%d/decimate %u\r\n", ag, c->ain_group_decimate[ag]);
            if (c->ain_group_batch[ag])    DUMP2("ain group %d batch %u\r\n", "ain/group/%d/batch %u\r\n", ag, c->ain_group_batch[ag]);
            if (c->ain_group_flags[ag] & AIN_GROUP_FLAG_AVG) DUMP2("ain group %d average 1\r\n", "ain/group/%d/average 1\r\n", ag);
        }
    if (dserv_cfg_console_mode(c) == CONSOLE_MODE_UART) DUMP2("console uart\r\n", "console uart\r\n");
    if (c->oled_en)                       DUMP2("oled enable 1\r\n", "oled/enable 1\r\n");
    if (c->ble_en)                        DUMP2("ble enable 1\r\n", "ble/enable 1\r\n");
    if (c->pipe_en)                       DUMP2("ble pipe 1\r\n", "ble/pipe 1\r\n");
    if (c->ble_latency)                   DUMP2("ble latency %u\r\n", "ble/latency %u\r\n", c->ble_latency);
    DUMPCLI("save\r\n");
    DUMPCLI("# reboot   (uncomment / run to apply mode/net changes)\r\n");
}

/* Same dump, delivered to a caller's sink instead of the console. Restores the
 * console target afterwards so the `dump` verb is unaffected. */
static inline void box_cli_dump_to_form(const box_config_t *c, box_cli_emit_fn fn,
					void *ud, int form)
{
	box_cli_emit = fn;
	box_cli_emit_ud = ud;
	box_cli_dump_form = form;
	box_cli_dump(c);
	box_cli_dump_form = BOX_DUMP_CLI;   /* never leave DP form armed */
	box_cli_emit = NULL;
	box_cli_emit_ud = NULL;
}

static inline void box_cli_dump_to(const box_config_t *c, box_cli_emit_fn fn, void *ud)
{
	box_cli_dump_to_form(c, fn, ud, BOX_DUMP_CLI);
}

#if defined(BOX_NET_DUAL)
#define BOX_CLI_HELP_XTRA "mode auto|usb|eth | phylink [1|0] | "   /* phylink: Pico bench tool */
#elif defined(BOX_HAVE_XPORT_POLICY)
#define BOX_CLI_HELP_XTRA "mode auto|usb|eth | "
#else
#define BOX_CLI_HELP_XTRA ""
#endif
#if defined(BOX_BLE)
#define BOX_CLI_HELP_BLE "ble | ble scan 1|0 | ble pipe 1|0 | ble latency <n> | ble pair <s> | ble forget | ble bonds | "   /* runtime radio cmds live in cmd_exec */
#elif defined(BOX_NET_BLE)
#define BOX_CLI_HELP_BLE "ble | "
#else
#define BOX_CLI_HELP_BLE ""
#endif

/* Execute one line. Returns an action; fills `out` with a response line.
 * `cmd` (may be NULL) receives a GPIO command for the `do` verbs (CLI_GPIO). */
static inline cli_action_t box_cli_exec(box_config_t *c, const char *line,
                                         char *out, int outsz, gpio_cmd_t *cmd)
{
    int n, v; char w[24];
    if (cmd) cmd->op = GPIO_OP_NONE;

    /* skip leading spaces; ignore blank lines and # comments (so a pasted `dump` -- which
     * includes header/# lines -- applies cleanly) */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '#') { out[0] = '\0'; return CLI_OK; }

    if (sscanf(line, "do %d pulse %d", &n, &v) == 2) {
        if (n < 0 || n >= BOX_NPINS || v < 0) { snprintf(out, outsz, "ERR bad do/pulse\r\n"); return CLI_ERR; }
        if (!box_cli_do_ok(c, n, out, outsz)) return CLI_ERR;
        if (cmd) { cmd->op = GPIO_OP_PULSE; cmd->pin = (uint8_t) n; cmd->value = (uint32_t) v; }
        snprintf(out, outsz, "OK do%d pulse=%dus\r\n", n, v); return CLI_GPIO;
    }
    if (sscanf(line, "do %d %d", &n, &v) == 2) {
        if (n < 0 || n >= BOX_NPINS) { snprintf(out, outsz, "ERR bad do pin\r\n"); return CLI_ERR; }
        if (!box_cli_do_ok(c, n, out, outsz)) return CLI_ERR;
        if (cmd) { cmd->op = GPIO_OP_SET; cmd->pin = (uint8_t) n; cmd->value = v ? 1 : 0; }
        snprintf(out, outsz, "OK do%d=%d\r\n", n, v ? 1 : 0); return CLI_GPIO;
    }

    if (sscanf(line, "name %15s", w) == 1) {
        if (!dserv_name_valid(w)) { snprintf(out, outsz, "ERR name: printable, no '/'\r\n"); return CLI_ERR; }
        strncpy(c->name, w, sizeof c->name - 1); c->name[sizeof c->name - 1] = '\0';
        c->applied_count++; snprintf(out, outsz, "OK name=%s\r\n", c->name); return CLI_OK;
    }
    /* channel: the firmware update track the host tool compares against. Bare
     * `channel` queries; `channel dev`/`off` resets to the default (stored empty
     * -> reads as dev). Not part of the pin/group manifest, so CLI_OK -- it
     * re-announces as state/channel at the next (re)connect. */
    if (!strcmp(line, "channel")) {
        snprintf(out, outsz, "OK channel=%s\r\n", dserv_cfg_channel(c)); return CLI_OK;
    }
    if (sscanf(line, "channel %15s", w) == 1) {
        if (!strcmp(w, "off") || !strcmp(w, "dev")) c->channel[0] = '\0';
        else if (!dserv_name_valid(w)) { snprintf(out, outsz, "ERR channel: printable, no '/'\r\n"); return CLI_ERR; }
        else { strncpy(c->channel, w, sizeof c->channel - 1); c->channel[sizeof c->channel - 1] = '\0'; }
        c->applied_count++;
        snprintf(out, outsz, "OK channel=%s\r\n", dserv_cfg_channel(c)); return CLI_OK;
    }
    if (sscanf(line, "pin %d mode %23s", &n, w) == 2) {
        int m = dserv_mode_val(w);
        if (n < 0 || n >= BOX_NPINS || m < 0) { snprintf(out, outsz, "ERR bad pin/mode\r\n"); return CLI_ERR; }
        if (box_cli_pin_reserved(n)) {
            snprintf(out, outsz, "ERR pin %d is reserved on this board\r\n", n); return CLI_ERR;
        }
        c->pin_mode[n] = (uint8_t) m; c->applied_count++;
        snprintf(out, outsz, "OK pin%d=%s\r\n", n, dserv_mode_str((uint8_t)m)); return CLI_PIN;
    }
    if (sscanf(line, "pin %d pulse %d", &n, &v) == 2) {
        if (n < 0 || n >= BOX_NPINS || v < 0) { snprintf(out, outsz, "ERR bad pin/pulse\r\n"); return CLI_ERR; }
        c->do_pulse_us[n] = (uint32_t) v; c->applied_count++;
        snprintf(out, outsz, "OK pin%d pulse=%dus\r\n", n, v); return CLI_OK;
    }
    if (sscanf(line, "pin %d debounce %d", &n, &v) == 2) {
        if (n < 0 || n >= BOX_NPINS || v < 0 || v > 255) { snprintf(out, outsz, "ERR bad pin/debounce (0-255ms)\r\n"); return CLI_ERR; }
        c->debounce_ms[n] = (uint8_t) v; c->applied_count++;
        snprintf(out, outsz, "OK pin%d debounce=%dms\r\n", n, v); return CLI_OK;
    }
    if (sscanf(line, "pin %d active_low %d", &n, &v) == 2) {
        if (n < 0 || n >= BOX_NPINS) { snprintf(out, outsz, "ERR bad pin\r\n"); return CLI_ERR; }
        di_active_low_set(c, n, v ? 1 : 0); c->applied_count++;
        snprintf(out, outsz, "OK pin%d active_low=%d\r\n", n, v ? 1 : 0); return CLI_OK;
    }
    if (sscanf(line, "label %d %15s", &n, w) == 2) {
        if (n < 0 || n >= BOX_NPINS) { snprintf(out, outsz, "ERR bad pin\r\n"); return CLI_ERR; }
        if (!strcmp(w, "off")) w[0] = '\0';
        if (!dserv_label_valid(w)) { snprintf(out, outsz, "ERR label: printable, no '/' or spaces\r\n"); return CLI_ERR; }
        /* %.*s, not %s: the sscanf above already caps at 15 = BOX_LABEL_MAX-1,
         * but `w` is the shared 24-byte scratch (other verbs read %23s into it),
         * so the compiler can only see a possible 23-byte write into 16 and
         * -Wformat-truncation fails the build. Stating the bound is the same
         * behaviour, provably. */
        snprintf(c->pin_label[n], BOX_LABEL_MAX, "%.*s", BOX_LABEL_MAX - 1, w);
        c->applied_count++;
        snprintf(out, outsz, "OK pin%d label=%s\r\n", n, c->pin_label[n][0] ? c->pin_label[n] : "(none)");
        return CLI_GROUP;
    }
    if (sscanf(line, "group %d pins %23s", &n, w) == 2) {
        uint32_t mask;
        if (n < 0 || n >= BOX_NGROUPS || dserv_parse_pins(w, &mask) < 0) {
            snprintf(out, outsz, "ERR group pins: 'group G pins 2,3,4,5' (G 0-%d, pins 0-%d)\r\n",
                     BOX_NGROUPS - 1, BOX_NPINS - 1);
            return CLI_ERR;
        }
        c->group_pins[n] = mask; c->applied_count++;
        snprintf(out, outsz, "OK group%d pins=%s\r\n", n, w); return CLI_GROUP;
    }
    if (sscanf(line, "group %d label %15s", &n, w) == 2) {
        if (n < 0 || n >= BOX_NGROUPS) { snprintf(out, outsz, "ERR bad group\r\n"); return CLI_ERR; }
        if (!strcmp(w, "off")) w[0] = '\0';
        if (!dserv_label_valid(w)) { snprintf(out, outsz, "ERR label: printable, no '/' or spaces\r\n"); return CLI_ERR; }
        snprintf(c->group_label[n], BOX_LABEL_MAX, "%.*s", BOX_LABEL_MAX - 1, w);  /* see pin label above */
        c->applied_count++;
        snprintf(out, outsz, "OK group%d label=%s\r\n", n, c->group_label[n][0] ? c->group_label[n] : "(none)");
        return CLI_GROUP;
    }
    if (sscanf(line, "group %d settle %d", &n, &v) == 2) {
        if (n < 0 || n >= BOX_NGROUPS || v < 0 || v > 65535) {
            snprintf(out, outsz, "ERR group settle 0-65535 ms\r\n"); return CLI_ERR; }
        c->group_settle_ms[n] = (uint16_t) v; c->applied_count++;
        snprintf(out, outsz, "OK group%d settle=%dms\r\n", n, v); return CLI_GROUP;
    }
    if (sscanf(line, "group %d quiet %d", &n, &v) == 2) {
        if (n < 0 || n >= BOX_NGROUPS) { snprintf(out, outsz, "ERR bad group\r\n"); return CLI_ERR; }
        c->group_quiet[n] = v ? 1 : 0; c->applied_count++;
        snprintf(out, outsz, "OK group%d quiet=%d\r\n", n, c->group_quiet[n]); return CLI_GROUP;
    }
    if (sscanf(line, "group %d %23s", &n, w) == 2 && !strcmp(w, "off")) {
        if (n < 0 || n >= BOX_NGROUPS) { snprintf(out, outsz, "ERR bad group\r\n"); return CLI_ERR; }
        c->group_pins[n] = 0; c->applied_count++;
        snprintf(out, outsz, "OK group%d off\r\n", n); return CLI_GROUP;
    }
    /* ---- analog (MCP3204) groups: base scan rate + per-group channel-set policy ---- */
    if (sscanf(line, "ain oversample %d", &v) == 1) {
        uint8_t e = 0;
        while ((1 << e) < v && e < 7) e++;
        if (v < 1 || (1 << e) != v) {
            snprintf(out, outsz, "ERR ain oversample 1|2|4|8|16|32|64|128\r\n"); return CLI_ERR;
        }
        c->ain_ovs = e;
        snprintf(out, outsz, "OK ain oversample=%dx (hw average per trigger)\r\n", v);
        return CLI_AIN;
    }
    if (sscanf(line, "dbg level %15s", w) == 1) {
        if (!strcmp(w, "health"))    c->dbg_level = DBG_LEVEL_HEALTH;
        else if (!strcmp(w, "full")) c->dbg_level = DBG_LEVEL_FULL;
        else if (!strcmp(w, "off"))  c->dbg_level = DBG_LEVEL_OFF;
        else { snprintf(out, outsz, "ERR dbg level health|full|off\r\n"); return CLI_ERR; }
        c->applied_count++;
        snprintf(out, outsz, "OK dbg level=%s (watchdog always publishes)\r\n", w);
        return CLI_AIN;
    }
    if (sscanf(line, "ain label %d %15s", &n, w) == 2) {
        if (n < 0 || n >= AIN_MAX_CH) {
            snprintf(out, outsz, "ERR ain label: channel 0-%d\r\n", AIN_MAX_CH - 1); return CLI_ERR; }
        if (!strcmp(w, "off")) w[0] = '\0';
        if (!dserv_label_valid(w)) {
            snprintf(out, outsz, "ERR label: printable, no '/', <= %d chars\r\n", BOX_LABEL_MAX - 1);
            return CLI_ERR; }
        snprintf(c->ain_label[n], BOX_LABEL_MAX, "%s", w); c->applied_count++;
        snprintf(out, outsz, "OK ain label%d=%s\r\n", n, w[0] ? w : "(none)");
        return CLI_GROUP;   /* labels change the manifest -> re-announce */
    }
    if (sscanf(line, "ain clkppm %d", &v) == 1) {
        if (v < -32768 || v > 32767) { snprintf(out, outsz, "ERR ain clkppm -32768..32767\r\n"); return CLI_ERR; }
        c->ain_clk_ppm = (int16_t) v; c->applied_count++;
        snprintf(out, outsz, "OK ain clkppm=%d (CTIMER source vs nominal; 0 = uncalibrated)\r\n", v);
        return CLI_AIN;
    }
    if (sscanf(line, "ain pace %15s", w) == 1) {
        if (!strcmp(w, "auto"))        c->ain_pace = AIN_PACE_AUTO;
        else if (!strcmp(w, "polled")) c->ain_pace = AIN_PACE_POLLED;
        else if (!strcmp(w, "stream")) c->ain_pace = AIN_PACE_STREAM;
        else { snprintf(out, outsz, "ERR ain pace auto|polled|stream\r\n"); return CLI_ERR; }
        c->applied_count++;
        snprintf(out, outsz, "OK ain pace=%s (auto follows the build; ain/dbg/pace says what is RUNNING)\r\n", w);
        return CLI_AIN;
    }
    if (sscanf(line, "ain rate %d", &v) == 1) {
        if (v < 1 || v > 65535) { snprintf(out, outsz, "ERR ain rate 1-65535 Hz\r\n"); return CLI_ERR; }
        uint16_t _oldrate = c->ain_rate;
        c->ain_rate = (uint16_t) v; c->applied_count++;
        if (!box_cli_bps_ok(c, out, outsz)) { c->ain_rate = (uint16_t) _oldrate; return CLI_ERR; }
        snprintf(out, outsz, "OK ain rate=%dHz (base scan; save+reboot)\r\n", v); return CLI_AIN;
    }
    if (sscanf(line, "ain group %d channels %23s", &n, w) == 2) {
        uint32_t mask;
        if (n < 0 || n >= BOX_NAGROUPS || dserv_parse_pins(w, &mask) < 0 ||
            (mask & ~box_cli_ain_chmask())) {
            snprintf(out, outsz, "ERR ain channels: 'ain group G channels 0,1' (G 0-%d, ch 0-%d)\r\n",
                     BOX_NAGROUPS - 1, box_cli_ain_nch - 1);
            return CLI_ERR;
        }
        uint8_t _oldm = c->ain_group_chans[n], _oldb = c->ain_group_batch[n];
        c->ain_group_chans[n] = (uint8_t) mask; c->applied_count++;
        /* Widening a group is the one edit that can invalidate an already
         * accepted batch. Refusing the CHANNEL change over a secondary field
         * would reject the wrong thing, so batch yields -- but say so, or it
         * is the same silent clamp from a different direction. */
        if (!box_cli_bps_ok(c, out, outsz)) {
            c->ain_group_chans[n] = _oldm; c->ain_group_batch[n] = _oldb; return CLI_ERR;
        }
        if (ain_batch_reclamp(c, n)) {
            snprintf(out, outsz, "OK ain group%d channels=%s (batch reduced to %d to fit)\r\n",
                     n, w, c->ain_group_batch[n]);
        } else {
            snprintf(out, outsz, "OK ain group%d channels=%s\r\n", n, w);
        }
        return CLI_AIN;
    }
    if (sscanf(line, "ain group %d label %15s", &n, w) == 2) {
        if (n < 0 || n >= BOX_NAGROUPS) { snprintf(out, outsz, "ERR bad group\r\n"); return CLI_ERR; }
        if (!strcmp(w, "off")) w[0] = '\0';
        if (!dserv_label_valid(w)) { snprintf(out, outsz, "ERR label: printable, no '/' or spaces\r\n"); return CLI_ERR; }
        snprintf(c->ain_group_label[n], BOX_LABEL_MAX, "%.*s", BOX_LABEL_MAX - 1, w); c->applied_count++;
        snprintf(out, outsz, "OK ain group%d label=%s\r\n", n,
                 c->ain_group_label[n][0] ? c->ain_group_label[n] : "(none)");
        return CLI_AIN;
    }
    if (sscanf(line, "ain group %d mode %15s", &n, w) == 2) {
        if (n < 0 || n >= BOX_NAGROUPS) { snprintf(out, outsz, "ERR bad group\r\n"); return CLI_ERR; }
        uint8_t _oldmo = c->ain_group_mode[n];
        if (!strcmp(w, "continuous")) c->ain_group_mode[n] = 1;
        else if (!strcmp(w, "onchange")) c->ain_group_mode[n] = 0;
        else { snprintf(out, outsz, "ERR ain mode: onchange|continuous\r\n"); return CLI_ERR; }
        c->applied_count++; snprintf(out, outsz, "OK ain group%d mode=%s\r\n", n, w); return CLI_AIN;
    }
    if (sscanf(line, "ain group %d deadband %d", &n, &v) == 2) {
        if (n < 0 || n >= BOX_NAGROUPS || v < 0 || v > 4095) {
            snprintf(out, outsz, "ERR ain deadband 0-4095\r\n"); return CLI_ERR; }
        c->ain_group_deadband[n] = (uint16_t) v; c->applied_count++;
        snprintf(out, outsz, "OK ain group%d deadband=%d\r\n", n, v); return CLI_AIN;
    }
    if (sscanf(line, "ain group %d decimate %d", &n, &v) == 2) {
        if (n < 0 || n >= BOX_NAGROUPS || v < 1 || v > 255) {
            snprintf(out, outsz, "ERR ain decimate 1-255\r\n"); return CLI_ERR; }
        uint8_t _oldd = c->ain_group_decimate[n];
        c->ain_group_decimate[n] = (uint8_t) v; c->applied_count++;
        if (!box_cli_bps_ok(c, out, outsz)) { c->ain_group_decimate[n] = _oldd; return CLI_ERR; }
        snprintf(out, outsz, "OK ain group%d decimate=%d\r\n", n, v); return CLI_AIN;
    }
    if (sscanf(line, "ain group %d batch %d", &n, &v) == 2) {
        if (n < 0 || n >= BOX_NAGROUPS) { snprintf(out, outsz, "ERR bad group\r\n"); return CLI_ERR; }
        /* REFUSE rather than store-and-clamp. The sampler has always clamped
         * (ain_group_batch_eff), which meant a group set to batch 20 on 2
         * channels read back 20 and sampled 12 -- the operator believing a
         * setting that never took. Name the cap so the next attempt is right. */
        int cap = ain_batch_cap(c, n);
        if (v < 0 || v > cap) {
            snprintf(out, outsz,
                     "ERR ain group %d batch 0-%d (batch x %d channels must fit %d samples/block)\r\n",
                     n, cap, (cap > 0 ? AIN_BLOCK_MAX / cap : 0), AIN_BLOCK_MAX);
            return CLI_ERR;
        }
        uint8_t _oldbt = c->ain_group_batch[n];
        c->ain_group_batch[n] = (uint8_t) v; c->applied_count++;
        if (!box_cli_bps_ok(c, out, outsz)) { c->ain_group_batch[n] = _oldbt; return CLI_ERR; }
        snprintf(out, outsz, "OK ain group%d batch=%d\r\n", n, v); return CLI_AIN;
    }
    if (sscanf(line, "ain group %d average %d", &n, &v) == 2) {
        if (n < 0 || n >= BOX_NAGROUPS) { snprintf(out, outsz, "ERR bad group\r\n"); return CLI_ERR; }
        if (v) c->ain_group_flags[n] |= AIN_GROUP_FLAG_AVG;
        else   c->ain_group_flags[n] &= (uint8_t) ~AIN_GROUP_FLAG_AVG;
        c->applied_count++; snprintf(out, outsz, "OK ain group%d average=%d\r\n", n, v ? 1 : 0); return CLI_AIN;
    }
    if (sscanf(line, "ain group %d %15s", &n, w) == 2 && !strcmp(w, "off")) {
        if (n < 0 || n >= BOX_NAGROUPS) { snprintf(out, outsz, "ERR bad group\r\n"); return CLI_ERR; }
        c->ain_group_chans[n] = 0; c->ain_group_label[n][0] = '\0'; c->applied_count++;
        snprintf(out, outsz, "OK ain group%d off\r\n", n); return CLI_AIN;
    }
    /* desc: value is the rest of the line verbatim, so spaces survive. */
    if (!strncmp(line, "desc ", 5)) {
        if (!strcmp(line + 5, "off")) c->desc[0] = '\0';
        else { strncpy(c->desc, line + 5, sizeof c->desc - 1); c->desc[sizeof c->desc - 1] = '\0'; }
        c->applied_count++;
        snprintf(out, outsz, "OK desc=%s\r\n", c->desc[0] ? c->desc : "(none)"); return CLI_GROUP;
    }
    if (sscanf(line, "dserv ip %23s", w) == 1) {
        if (dserv_cfg__parse_ip(w, c->dserv_ip)) { snprintf(out, outsz, "ERR bad ip\r\n"); return CLI_ERR; }
        c->applied_count++; snprintf(out, outsz, "OK dserv ip=%s\r\n", w); return CLI_OK;
    }
    if (sscanf(line, "dserv port %d", &v) == 1) {
        if (v < 1 || v > 65535) { snprintf(out, outsz, "ERR bad port\r\n"); return CLI_ERR; }
        c->dserv_port = (uint16_t) v; c->applied_count++;
        snprintf(out, outsz, "OK dserv port=%d\r\n", v); return CLI_OK;
    }
    if (sscanf(line, "net ip %23s", w) == 1) {
        if (dserv_cfg__parse_ip(w, c->net_ip)) { snprintf(out, outsz, "ERR bad ip\r\n"); return CLI_ERR; }
        c->net_mode = NET_MODE_STATIC;      /* setting a static IP implies static mode */
        c->applied_count++; snprintf(out, outsz, "OK net ip=%s mode=static (save+reboot to apply)\r\n", w); return CLI_OK;
    }
    if (sscanf(line, "net gateway %23s", w) == 1) {   /* static gateway; 0.0.0.0 => auto (subnet .1) */
        if (dserv_cfg__parse_ip(w, c->net_gw)) { snprintf(out, outsz, "ERR bad ip\r\n"); return CLI_ERR; }
        c->applied_count++; snprintf(out, outsz, "OK net gateway=%s (save+reboot to apply)\r\n", w); return CLI_OK;
    }
    if (sscanf(line, "net mask %23s", w) == 1) {      /* static subnet mask; 0.0.0.0 => /24 */
        if (dserv_cfg__parse_ip(w, c->net_sn)) { snprintf(out, outsz, "ERR bad mask\r\n"); return CLI_ERR; }
        c->applied_count++; snprintf(out, outsz, "OK net mask=%s (save+reboot to apply)\r\n", w); return CLI_OK;
    }
    if (sscanf(line, "obs pin %d", &n) == 1) {
        if (n < 0 || n >= BOX_NPINS) { snprintf(out, outsz, "ERR obs pin 0-%d (or 'obs off')\r\n", BOX_NPINS - 1); return CLI_ERR; }
        obs_mirror_set(c, n); c->applied_count++;
        snprintf(out, outsz, "OK obs pin=%d\r\n", n); return CLI_PIN;
    }
    if (!strcmp(line, "obs off")) {
        obs_mirror_off(c); c->applied_count++;
        snprintf(out, outsz, "OK obs off\r\n"); return CLI_PIN;
    }
    /* obs mode mirror|leader -- the pin's ROLE, orthogonal to `obs pin N`
     * (which sets WHICH pin and implies mirror). Mirror follows the host's
     * ess/in_obs; leader OWNS the line + the epoch (fires it at an at_abs
     * instant, stamps the actual onset). Applies live -- no save+reboot --
     * and re-announces so a host's leader scan sees it now. Was CLI-only-
     * MISSING through v34 (datapoint config/obs/mode had it, the CLI did
     * not), so the serial config path could not reach leader at all. */
    {
        char w[8];
        if (sscanf(line, "obs mode %7s", w) == 1) {
            if (!strcmp(w, "mirror")) {
                c->obs_mode = OBS_MODE_MIRROR; c->applied_count++;
                snprintf(out, outsz, "OK obs mode=mirror\r\n"); return CLI_OBS;
            }
            if (!strcmp(w, "leader")) {
#ifdef BOX_HAVE_OBS_LEADER
                c->obs_mode = OBS_MODE_LEADER; c->applied_count++;
                snprintf(out, outsz, "OK obs mode=leader%s\r\n",
                         obs_mirror_enabled(c) ? ""
                         : " (WARN no obs pin set -- `obs pin N` first)");
                return CLI_OBS;
#else
                snprintf(out, outsz,
                    "ERR this build has no obs-leader machinery (mirror only)\r\n");
                return CLI_ERR;
#endif
            }
            snprintf(out, outsz, "ERR obs mode mirror|leader\r\n"); return CLI_ERR;
        }
    }
    if (sscanf(line, "sync pin %d", &n) == 1) {   /* TTL obs-sync INPUT (hardware clock anchor) */
        if (n < 0 || n >= BOX_NPINS) { snprintf(out, outsz, "ERR sync pin 0-%d (or 'sync off')\r\n", BOX_NPINS - 1); return CLI_ERR; }
        sync_input_set(c, n); c->applied_count++;
        snprintf(out, outsz, "OK sync pin=%d (input; wire the rig host's obs TTL)\r\n", n); return CLI_PIN;
    }
    if (!strcmp(line, "sync off")) {
        sync_input_off(c); c->applied_count++;
        snprintf(out, outsz, "OK sync off\r\n"); return CLI_PIN;
    }
    /* wifi creds: value is the rest of the line verbatim (one space separator),
     * so SSIDs/passwords with spaces or special chars survive intact. */
    if (!strncmp(line, "wifi ssid ", 10)) {
        strncpy(c->wifi_ssid, line + 10, sizeof c->wifi_ssid - 1); c->wifi_ssid[sizeof c->wifi_ssid - 1] = '\0';
        c->applied_count++; snprintf(out, outsz, "OK wifi ssid=%s (save+reboot to apply)\r\n", c->wifi_ssid); return CLI_OK;
    }
    if (!strncmp(line, "wifi pass ", 10)) {
        strncpy(c->wifi_pass, line + 10, sizeof c->wifi_pass - 1); c->wifi_pass[sizeof c->wifi_pass - 1] = '\0';
        c->applied_count++; snprintf(out, outsz, "OK wifi pass set (save+reboot to apply)\r\n"); return CLI_OK;
    }
    if (sscanf(line, "wifi pm %d", &v) == 1) {
        c->wifi_pm = v ? 1 : 0; c->applied_count++;
        snprintf(out, outsz, "OK wifi pm=%d (%s; save+reboot to apply)\r\n",
                 c->wifi_pm, c->wifi_pm ? "power-save/battery" : "off/low-latency"); return CLI_OK;
    }
    if (sscanf(line, "ain enable %d", &v) == 1) { /* master switch for analog input, whatever converter is fitted */
        c->ain_en = v ? 1 : 0; c->applied_count++;
        /* RETURNS CLI_AIN, AND THAT IS THE WHOLE FIX. It used to return CLI_OK,
         * which is the one return value the console dispatch does nothing with --
         * so `ain enable 0/1` set the config byte and NEVER told the sampler.
         * Every other `ain ...` command here already returned CLI_AIN.
         *
         * So the old "(save+reboot to apply)" was RIGHT IN EFFECT and wrong in
         * its reason. It was written as an RP2350 statement (there ain_en claimed
         * the SPI0 pins at init, so a reboot genuinely was required); on Zephyr
         * the pins come from pinctrl regardless, and I removed the message on the
         * strength of that -- while the reboot requirement was in fact still real,
         * created by this missing return one line down.
         *
         * CAUGHT ONLY ON HARDWARE: `ain enable 0` printed OK, and the box went on
         * sampling with ain/dbg/running=1 and sweeps still climbing. Reading the
         * code found the intent; running it found the return value. */
        snprintf(out, outsz, "OK ain enable=%d (live; `save` to persist)\r\n", c->ain_en); return CLI_AIN;
    }
    if (sscanf(line, "oled enable %d", &v) == 1) { /* SSD1306 SPI status display (see PINMAP.md) */
        c->oled_en = v ? 1 : 0; c->applied_count++;
        snprintf(out, outsz, "OK oled enable=%d (claims SPI0 pins; save+reboot to apply)\r\n", c->oled_en);
        return CLI_OK;
    }
    if (sscanf(line, "ble enable %d", &v) == 1) { /* BLE central (BOX_BLE radio builds; see BLE.md) */
        c->ble_en = v ? 1 : 0; c->applied_count++;
        snprintf(out, outsz, "OK ble enable=%d (live on radio builds; `save` to persist)\r\n", c->ble_en);
        return CLI_OK;
    }
    if (sscanf(line, "ble pipe %d", &v) == 1) {   /* receiver relay latch (v16); cmd_exec fires the
                                                   * live request first, then falls through here */
        c->pipe_en = v ? 1 : 0; c->applied_count++;
        snprintf(out, outsz, "OK ble pipe=%d (live on the receiver; `save` to auto-arm at boot)\r\n", c->pipe_en);
        return CLI_OK;
    }
    if (sscanf(line, "ble latency %d", &v) == 1) { /* idle peripheral-latency target (v17); read live by
                                                    * box_ble_latency_service. 0 = always-listen default */
        if (v < 0) v = 0;
        if (v > 30) v = 30;
        c->ble_latency = (uint8_t) v; c->applied_count++;
        snprintf(out, outsz, "OK ble latency=%d (0=always listen; raises when synced+idle; `save` to persist)\r\n",
                 c->ble_latency);
        return CLI_OK;
    }
    if (sscanf(line, "net mode %11s", w) == 1) {
        if      (!strcmp(w, "dhcp"))   c->net_mode = NET_MODE_DHCP;
        else if (!strcmp(w, "static")) c->net_mode = NET_MODE_STATIC;
        else { snprintf(out, outsz, "ERR net mode dhcp|static\r\n"); return CLI_ERR; }
        c->applied_count++; snprintf(out, outsz, "OK net mode=%s (save+reboot to apply)\r\n", dserv_netmode_str(c->net_mode)); return CLI_OK;
    }
#ifdef BOX_HAVE_XPORT_POLICY
    /* Boot transport policy for the open-strap case (GND strap always forces eth).
     * Safe to persist again: with the core split, a bad choice can't kill the
     * console -- and auto never commits to a transport it can't bring up. */
    if (sscanf(line, "mode %7s", w) == 1) {
        int m = !strcmp(w, "auto") ? XMODE_AUTO :
                !strcmp(w, "eth")  ? XMODE_ETH  :
                !strcmp(w, "usb")  ? XMODE_USB  : -1;
        if (m < 0) { snprintf(out, outsz, "ERR mode auto|usb|eth\r\n"); return CLI_ERR; }
        c->transport_mode = (uint8_t) m; c->applied_count++;
        /* APPLIES NOW -- the arbiter re-reads transport_mode on every service
         * pass (box_uplink.c desired()), so the transport switches within a
         * pass of this line. The message used to say "save+reboot to apply",
         * which was false in a way that cost real time: a person reads it,
         * reboots WITHOUT saving, and both the mode and anything adopted since
         * revert to defaults -- so the box comes back on USB looking like the
         * change never took. Say what saving is actually for. */
        snprintf(out, outsz, "OK mode=%s -- active now (GND strap overrides; `save` to keep it)\r\n",
                 dserv_xmode_str(c->transport_mode));
        return CLI_OK;
    }
#endif
    if (sscanf(line, "console %7s", w) == 1) {   /* cdc|uart -- a TIMING choice */
        int v = dserv_console_val(w);
        if (v < 0) { snprintf(out, outsz, "ERR console cdc|uart\r\n"); return CLI_ERR; }
        c->console_mode = (uint8_t) v; c->applied_count++;
        /* Deliberately does NOT claim a USB-serial adapter is needed: that is
           true of the Teensy (lpuart6 on pins 0/1) and FALSE of a board whose
           console UART is its on-board debug VCOM, where it is the same cable
           that already powers the box. This grammar is shared across both, so
           it states WHERE the console goes and lets the board speak for
           itself. */
        snprintf(out, outsz, "OK console=%s (save+reboot to apply%s)\r\n",
                 dserv_console_str(c->console_mode),
                 v == CONSOLE_MODE_UART ? "; moves to the board console UART" : "");
        return CLI_OK;
    }
    if (!strcmp(line, "show"))    { box_cli_show(c, out, outsz); return CLI_OK; }
    if (!strcmp(line, "dump"))    { box_cli_dump(c); out[0] = '\0'; return CLI_OK; }  /* config as replayable cmds */
    if (!strcmp(line, "save"))    { snprintf(out, outsz, "saving...\r\n"); return CLI_SAVE; }
    if (!strcmp(line, "factory")) { snprintf(out, outsz, "factory reset...\r\n"); return CLI_FACTORY; }
    if (!strcmp(line, "reboot"))  { snprintf(out, outsz, "rebooting...\r\n"); return CLI_REBOOT; }
    if (!strcmp(line, "bootsel")) { snprintf(out, outsz, "entering USB BOOTSEL (then: picotool load <uf2>)...\r\n"); return CLI_BOOTSEL; }
    if (!strcmp(line, "help")) {
        snprintf(out, outsz,
            "cmds: show | dump | name NAME | desc TEXT | channel NAME | console cdc|uart | " BOX_CLI_HELP_XTRA
            "net mode dhcp|static | net ip A.B.C.D | net gateway A.B.C.D | net mask A.B.C.D |\r\n"
            "      wifi ssid SSID | wifi pass PASS | wifi pm 0|1 | dserv ip A.B.C.D | dserv port N |\r\n"
            "      pin N mode out|in|in_pullup|ain|off | pin N pulse US | pin N debounce MS |\r\n"
            "      pin N active_low 0|1 | label N TEXT|off | obs pin N | obs off |\r\n"
            "      sync pin N | sync off |\r\n"
            "      group G pins 2,3,4,5 | group G label NAME | group G settle MS |\r\n"
            "      group G quiet 0|1 | group G off |\r\n"
            "      ain enable 0|1 | ain rate HZ | ain oversample N | oled enable 0|1 |\r\n"
            "      ain group G channels 0,1 | ain group G label NAME | ain group G mode onchange|continuous |\r\n"
            "      ain group G deadband N | ain group G decimate N | ain group G batch N | ain group G average 0|1 | ain group G off |\r\n"
            "      ble enable 0|1 | " BOX_CLI_HELP_BLE "\r\n"
            "      do N 0|1 | do N pulse US | wdt 0|1|test | save | factory | reboot | bootsel\r\n");
        return CLI_OK;
    }
    snprintf(out, outsz, "ERR unknown (try 'help')\r\n");
    return CLI_ERR;
}

#endif /* BOX_CLI_H */
