/*
 * box_announce.c -- see box_announce.h.
 */
#include "box_announce.h"
#include "box_boot.h"
#include "box_gpio.h"
#include "box_uplink.h"                 /* box_uplink_active_name for "transport" */
#include "box_pub.h"
#include "platform/box_console.h"       /* box_console_bound (v34) */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>          /* dac_en capability check below */
#include <stdio.h>
#include <string.h>

#if defined(CONFIG_NETWORKING)
#include "box_net_eth.h"
#endif

/* Identity strings. The Pico's build.sh bakes `git describe` / $TARGET / $BOARD
 * in here; until this tree has an equivalent release step, take what Zephyr
 * already knows and mark the version honestly as a dev build. */
#ifndef BOX_FW_VERSION
#define BOX_FW_VERSION   "dev"
#endif
#define BOX_BUILD_TARGET CONFIG_BOARD_TARGET   /* e.g. "teensy40/mimxrt1062" */
#define BOX_BOARD_ID     CONFIG_BOARD          /* e.g. "teensy40" -- OTA compat filter */

/* state/build is the SHELF IMAGE MATCH KEY: extio_ota_push_shelf picks the shelf
 * image whose `build` equals this string. But the shelf validates that field with
 *
 *     fwBuildRe = ^[a-z0-9][a-z0-9_-]*$        (dserv-agent/firmware.go)
 *
 * which has NO SLASH -- and every Zephyr board target here contains one
 * ("frdm_mcxn947/mcxn947/cpu0", "teensy40/mimxrt1062"). Publishing verbatim is
 * rejected as "invalid or missing build", so until now NO Zephyr board could be
 * put on the shelf at all; the RP2350 targets (usb/dual/w6300/...) are flat words
 * and never hit it.
 *
 * So flatten the separators. `frdm_mcxn947/mcxn947/cpu0` becomes
 * `frdm_mcxn947_mcxn947_cpu0` -- which is EXACTLY the string Zephyr itself uses
 * for this board's conf/overlay filenames, so the shelf key and the board files
 * agree and neither is a new convention to remember.
 *
 * Flattening the whole qualified target rather than publishing CONFIG_BOARD alone
 * is deliberate: on a multi-core part cpu0 and cpu1 are DIFFERENT images, and
 * `build` is the unique key the on-box updater matches. Collapsing them would let
 * a box fetch an image built for the other core.
 */
const char *box_build_key(void)
{
	static char key[48];

	if (key[0] == '\0') {
		size_t i = 0;
		for (const char *p = BOX_BUILD_TARGET; *p && i < sizeof key - 1; p++) {
			key[i++] = (*p == '/') ? '_' : *p;
		}
		key[i] = '\0';
	}
	return key;
}

/* ---- small helpers: one datapoint per item ----
 *
 * BULK class, deliberately: a manifest burst is dozens of frames of
 * re-emittable state (the announce can always be asked for again --
 * cmd/announce), and it must never occupy the event queue ahead of a DI edge.
 * Dropped-oldest under pressure is visible in dbg/pub_bulk_drop. */

/* A frame carries name + payload in ~109 bytes, and dserv_msg_build REFUSES
 * (returns -1) rather than truncating when they do not fit. Ignoring that -- as
 * this did -- sends whatever was left in the buffer, so an over-long value
 * becomes a garbage datapoint or a silently missing one. Neither is visible
 * from the host, which is the worst property a manifest can have: a UI renders
 * a confident, incomplete pin map and nothing says so.
 *
 * Publish a marker instead. "!toolong" is short enough to always fit, and a
 * consumer showing it is being told the truth about what happened. */
/* v34: pace the burst. Announces fire dozens of frames flat-out into a
 * finite net_buf pool, and whichever frames lose that race are silently
 * dropped -- a random subset of manifest keys missing per boot (officepi
 * 2026-08-05: state/obs_pin absent after a reconnect burst, so auto-bind
 * had nothing to resolve; .50 logged 118 boot-burst wire drops the same
 * way). Announces are rare, run in main-loop context, and are worthless
 * when lossy: yield briefly every few frames so TX drains. A ~40-frame
 * manifest gains ~15 ms, once per (re)connect. */
static unsigned pace_n;

static void pace(void)
{
	if (++pace_n >= 6) {
		pace_n = 0;
		k_sleep(K_MSEC(2));
	}
}

static void pub_str(const box_config_t *c, const char *leaf, const char *val)
{
	uint8_t f[DSERV_MSG_LEN];
	char nm[80];

	dserv_state_name(c, nm, sizeof nm, leaf);
	if (dserv_msg_string(f, nm, 0, val) <= 0) {
		dserv_msg_string(f, nm, 0, "!toolong");
	}
	(void) box_pub_bulk(f);
	pace();
}

static void pub_int(const box_config_t *c, const char *leaf, int64_t val)
{
	uint8_t f[DSERV_MSG_LEN];
	char nm[80];

	dserv_state_name(c, nm, sizeof nm, leaf);
	dserv_msg_int(f, nm, 0, (int) val);
	(void) box_pub_bulk(f);
	pace();
}

/* ---- identity ---- */

static void announce_ident(const box_config_t *c)
{
	char s[24];

	pub_str(c, "transport", box_uplink_active_name());   /* ACTIVE now */
	/* LATCHED at boot by box_boot_init(), not read here: the hardware cause
	 * register is cleared after that read, and it also carries the OTA verdicts
	 * (trial/revert/rejected) that no register knows about. */
	pub_str(c, "boot",      box_boot_reason());
	pub_str(c, "fw",        BOX_FW_VERSION);

	/* OTA/bootloader facts, on every connect because dserv RETAINS datapoints:
	 * a box that reverts while dserv is down must still say so when it comes
	 * back, and a stale "everything is fine" is exactly the failure this whole
	 * step exists to prevent. NULL = no bootloader on this board, in which case
	 * none of these leaves would mean anything. */
	if (box_boot_img_ver() != NULL) {
		/* The version MCUboot actually booted. state/fw above is a build-time
		 * string identical in every image we produce, so it cannot show an OTA
		 * taking effect -- this can. */
		pub_str(c, "fw_ver",          box_boot_img_ver());
		pub_int(c, "ota/trial",       box_boot_on_trial());
		pub_int(c, "ota/reverts",     box_boot_reverts());
		pub_int(c, "ota/rejects",     box_boot_rejects());
		pub_int(c, "ota/updates",     box_boot_updates());
		pub_str(c, "ota/last_arm_ver", box_boot_last_arm_ver());
		/* +56: can this box PULL an image itself (cmd/ota/fetch, the '<'
		 * binary-get)? Per-connect because it rides the ACTIVE transport:
		 * eth yes, usb no. The host picks its transfer front-end off this
		 * leaf, so absence (older firmware) safely means the chunk path. */
#if defined(CONFIG_NETWORKING)
		pub_int(c, "ota/fetch_ok",
			strcmp(box_uplink_active_name(), "eth") == 0 ? 1 : 0);
#else
		pub_int(c, "ota/fetch_ok", 0);
#endif
	}
	pub_str(c, "build",     box_build_key());     /* shelf image match key   */
	pub_str(c, "board",     BOX_BOARD_ID);       /* OTA compat filter       */
	/* +31: the obs pin's declared role, and the discoverable capability.
	 * obs/mode is what the pin DOES (mirror follows the host; leader owns
	 * the line and the epoch); obs_leader=1 is the flag a host scans for
	 * when resolving `obs_schedule_bind auto`. Announced from persisted
	 * config -- capability is static, per-obs fitness (sync trust, live
	 * watchdog) is the binder's judgement at bind time. */
	box_announce_obs_role(c);
	pub_str(c, "channel",   dserv_cfg_channel(c));
	/* which device the console bound to -- a timing-relevant choice, so a host
	 * can see it without guessing (see config/console). */
	pub_str(c, "console",   dserv_console_str((uint8_t) dserv_cfg_console_mode(c)));
	pub_int(c, "console_bound", box_console_bound);   /* 0 dead / 1 ok / 2 fellback (v34) */
	/* transport boot POLICY (config/xport/mode twin), in the MANIFEST not
	 * announce_ident: CFG_XPORT re-announces via box_announce_manifest, so
	 * the leaf must live here or a datapoint set reads back stale -- the
	 * exact "looks like it didn't stick" bug this parity work exists to
	 * kill. state/transport (ACTIVE) stays in ident; this is the policy. */
	pub_str(c, "xport/mode", dserv_xmode_str(c->transport_mode));

	/* 0.0.0.0 over USB -- `transport` says why, so this is informative rather
	 * than an error. */
	uint8_t ip[4] = {0, 0, 0, 0};
#if defined(CONFIG_NETWORKING)
	(void) box_net_eth_get_ip(ip);
#endif
	snprintf(s, sizeof s, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
	pub_str(c, "ip", s);
}

/* ---- manifest ---- */


/* ---- SILK: the board's own printed pin names, announced once per connect ----
 *
 * Box pin numbers are an internal index (GPIO2 bit indices on the Teensy, the
 * Arduino identity on the MCXN947) and match no marking on some boards -- box
 * pin 2 is Teensy pin 11. Publishing what is PRINTED lets the extio page show
 * it beside the number, so wiring does not require PINMAP.md.
 *
 * ONE datapoint, not one per pin: a full map is ~126 bytes on the MCXN947,
 * past the 109-byte fixed-frame payload, and per-pin frames would add ~18 to an
 * announce burst that already drops some. Sent with the '}' length-prefixed
 * form (dserv_msg_var_build) straight down box_uplink_send, which takes a
 * length -- box_pub's queue is fixed-frame only.
 *
 * Index-qualified ("0:T10,1:T12") so gaps are safe: entries the board leaves ""
 * are skipped entirely rather than published as blanks, which is how a 4.0
 * expresses the pins that only exist on a 4.1.
 *
 * A board that declares neither property publishes nothing and the page falls
 * back to bare numbers -- that is the intended default for a new card, not a
 * failure. */
#define SILK_NODE DT_PATH(zephyr_user)

static void pub_silk_map(const box_config_t *c, const char *leaf,
                         const char *const *names, int n)
{
        static uint8_t vf[512];
        char body[320];
        char nm[80];
        int k = 0;

        for (int i = 0; i < n; i++) {
                if (!names[i] || !names[i][0]) {
                        continue;               /* not bonded out on this variant */
                }
                int w = snprintf(body + k, sizeof body - k, "%s%d:%s",
                                 k ? "," : "", i, names[i]);
                if (w < 0 || k + w >= (int) sizeof body) {
                        break;                  /* full: publish what fits */
                }
                k += w;
        }
        if (!k) {
                return;
        }
        body[k] = '\0';
        dserv_state_name(c, nm, sizeof nm, leaf);
        int vlen = dserv_msg_var_string(vf, sizeof vf, nm, 0, body);

        if (vlen > 0) {
                (void) box_uplink_send(vf, vlen);
        }
}

void box_announce_silk(const box_config_t *c)
{
#if DT_NODE_HAS_PROP(SILK_NODE, box_pin_silk)
        static const char *const pin_silk[] = DT_PROP(SILK_NODE, box_pin_silk);
        pub_silk_map(c, "pins/silk", pin_silk, (int) ARRAY_SIZE(pin_silk));
#endif
#if DT_NODE_HAS_PROP(SILK_NODE, box_ain_silk)
        static const char *const ain_silk[] = DT_PROP(SILK_NODE, box_ain_silk);
        pub_silk_map(c, "ain/silk", ain_silk, (int) ARRAY_SIZE(ain_silk));
#endif
}

void box_announce_manifest(const box_config_t *c)
{
	box_announce_silk(c);   /* board-printed pin names; see above */

	char leaf[48], csv[96];

	pub_str(c, "desc", c->desc);

	/* pins/in + pins/out: exactly the ACTIVE DIO, so a UI renders the real pin
	 * map. A pin turned off drops out of these lists even though its last
	 * di/do datapoint lingers retained in dserv -- which would otherwise show a
	 * ghost pin that no longer exists. */
	int ki = 0, ko = 0;
	char in_csv[96], out_csv[96];

	in_csv[0] = out_csv[0] = '\0';
	for (int i = 0; i < BOX_NPINS; i++) {
		if (c->pin_mode[i] == 2 || c->pin_mode[i] == 3) {
			ki += snprintf(in_csv + ki, sizeof in_csv - ki, "%s%d", ki ? "," : "", i);
		} else if (c->pin_mode[i] == 1) {
			ko += snprintf(out_csv + ko, sizeof out_csv - ko, "%s%d", ko ? "," : "", i);
		}
	}
	pub_str(c, "pins/in",  in_csv);
	pub_str(c, "pins/out", out_csv);

	/* ---- what the board HAS, not just what is currently switched on ----
	 *
	 * pins/in and pins/out above describe the ACTIVE map, which is what a
	 * monitor wants. A CONFIGURATION UI needs the other half: a pin set to
	 * `off` appears in neither list, so it cannot be shown, let alone
	 * re-enabled, and there is no way to learn that pin 5 is the PHY's MDIO
	 * rather than merely unused. Without this a page must hardcode a table per
	 * board -- and MCXN947 / RW612 / Teensy differ in count AND in which pins
	 * are reserved, so that table renders confidently wrong pins for whichever
	 * board is added next.
	 *
	 * SPARSE `n:v` FOR THE PER-PIN SCALARS, not one datapoint per pin. Dense
	 * would be 4 x BOX_NPINS extra frames per announce -- ~72 here -- into a
	 * 40-deep publish queue, which is precisely the burst that drops the
	 * single-shot frames at the end of an operation (see main.c's watchdog
	 * catch-up note; that is how an OTA result reads as "never ran"). Six
	 * frames carry the same information, and only pins that differ from the
	 * default appear at all. */
	{
		int ka = 0, kl = 0, kr = 0, kd = 0, kp = 0, kA = 0, kP = 0;
		char all_csv[96], ain_csv[64], rsv_csv[64];
		char deb_csv[96], pul_csv[96], alo_csv[64], pup_csv[64];
		uint32_t rmask = box_gpio_reserved_mask();

		all_csv[0] = ain_csv[0] = rsv_csv[0] = '\0';
		deb_csv[0] = pul_csv[0] = alo_csv[0] = pup_csv[0] = '\0';

		for (int i = 0; i < BOX_NPINS; i++) {
			ka += snprintf(all_csv + ka, sizeof all_csv - ka, "%s%d", ka ? "," : "", i);
			if ((rmask >> i) & 1u) {
				kr += snprintf(rsv_csv + kr, sizeof rsv_csv - kr, "%s%d", kr ? "," : "", i);
			}
			/* pins/in deliberately merges modes 2 and 3, because a monitor
			 * only cares that a pin is an input. A CONFIG ui does care --
			 * otherwise its mode dropdown cannot show which of the two a pin
			 * actually is, and rewrites in_pullup to in the first time anyone
			 * touches an unrelated field on that row. Announced as a SUBSET of
			 * pins/in rather than a separate mode list, so nothing that
			 * consumes pins/in has to change. */
			if (c->pin_mode[i] == 3) {
				kP += snprintf(pup_csv + kP, sizeof pup_csv - kP, "%s%d", kP ? "," : "", i);
			}
			if (c->pin_mode[i] == PIN_MODE_AIN) {
				kA += snprintf(ain_csv + kA, sizeof ain_csv - kA, "%s%d", kA ? "," : "", i);
			}
			if (c->debounce_ms[i]) {
				kd += snprintf(deb_csv + kd, sizeof deb_csv - kd, "%s%d:%u",
					       kd ? "," : "", i, (unsigned) c->debounce_ms[i]);
			}
			if (c->do_pulse_us[i]) {
				kp += snprintf(pul_csv + kp, sizeof pul_csv - kp, "%s%d:%u",
					       kp ? "," : "", i, (unsigned) c->do_pulse_us[i]);
			}
			if ((c->di_active_low >> i) & 1u) {
				kl += snprintf(alo_csv + kl, sizeof alo_csv - kl, "%s%d", kl ? "," : "", i);
			}
		}
		pub_str(c, "pins/all",         all_csv);
		pub_str(c, "pins/reserved",    rsv_csv);
		pub_str(c, "pins/ain",         ain_csv);
		pub_str(c, "pins/pullup",      pup_csv);   /* subset of pins/in */
		pub_str(c, "pins/debounce_ms", deb_csv);   /* sparse n:ms  */
		pub_str(c, "pins/pulse_us",    pul_csv);   /* sparse n:us  */
		pub_str(c, "pins/active_low",  alo_csv);
	}

	/* Settings the box accepted but never reported back, so a UI could only
	 * offer a blank box that is indistinguishable from "the value is empty".
	 * dserv/ip is the one that matters most: it is how you discover which rig a
	 * box is pointed at without opening its console. */
	{
		char ip[20];

		snprintf(ip, sizeof ip, "%u.%u.%u.%u", c->dserv_ip[0], c->dserv_ip[1],
			 c->dserv_ip[2], c->dserv_ip[3]);
		pub_str(c, "dserv/ip",   ip);
		pub_int(c, "dserv/port", c->dserv_port);
		pub_int(c, "ain/rate",   dserv_cfg_ain_rate(c));
		pub_int(c, "ain/oversample", dserv_cfg_ain_ovs_count(c));
		pub_str(c, "net/mode",   c->net_mode ? "static" : "dhcp");
	}

	/* Special-function pins; -1 = off, so DISABLING one updates the retained
	 * value instead of leaving a ghost pointing at a pin that is now ordinary. */
	pub_int(c, "obs_pin",  obs_mirror_enabled(c) ? obs_mirror_pin(c)  : -1);
	pub_int(c, "sync_pin", sync_input_enabled(c) ? sync_input_pin(c) : -1);

	/* Feature flags, so a UI can shade pins claimed by a peripheral without
	 * hardcoding the pin budget. Neither is implemented on this port yet -- they
	 * are announced honestly as configured, not as running. */
	pub_int(c, "ain_en",  c->ain_en  ? 1 : 0);
	pub_int(c, "oled_en", c->oled_en ? 1 : 0);

	/* dac_en is COMPILED capability, not config: it says whether this build
	 * answers cmd/dac/<ch> at all (MCXN947's dac0; the RP2350 fleet and the
	 * teensy targets have no DT node and announce 0). Consumers capability-
	 * check this instead of probing a command that would silently no-op. */
#if defined(CONFIG_DAC) && DT_NODE_HAS_STATUS(DT_NODELABEL(dac0), okay)
	pub_int(c, "dac_en", 1);
#else
	pub_int(c, "dac_en", 0);
#endif

	/* Per-pin labels. A pin publishes its label if it HAS one or is configured.
	 * The mask tracks pins we published a non-empty label for, so clearing one
	 * re-publishes it EMPTY exactly once -- without that, "remove label" never
	 * reaches consumers and the old value stays retained forever. */
	static uint32_t label_pub_mask;

	for (int i = 0; i < BOX_NPINS; i++) {
		int has = c->pin_label[i][0] != 0;

		if (!has && !c->pin_mode[i] && !((label_pub_mask >> i) & 1u)) {
			continue;
		}
		snprintf(leaf, sizeof leaf, "label/%d", i);
		pub_str(c, leaf, c->pin_label[i]);       /* "" clears the ghost */
		if (has) {
			label_pub_mask |= (1u << i);
		} else {
			label_pub_mask &= ~(1u << i);
		}
	}

	/* Slot capacity, so a config UI renders exactly the CREATABLE slots
	 * instead of hardcoding the compiled-in constants -- the same reasoning
	 * as pins/all: a per-board table in the page is confidently wrong for
	 * the next board. */
	pub_int(c, "groups/max",     BOX_NGROUPS);
	pub_int(c, "ain/groups/max", BOX_NAGROUPS);

	/* DI chord groups. `pins` ascending IS the published bit order, so a host
	 * decodes state/group/<label> from this string alone; `idx` maps a labeled
	 * group back to its `group <n>` slot for editing.
	 *
	 * TOMBSTONES, same reason labels track label_pub_mask above: dserv
	 * retains every key forever, so a freed or renamed group otherwise
	 * ghosts in every UI with its last pins/value looking current. When a
	 * slot's published NAME disappears or changes, its old pins leaf is
	 * re-published EMPTY exactly once -- consumers treat pins=="" as "this
	 * name is dead", which no live group can collide with (a live group
	 * always has at least one member pin). */
	static char group_pub_name[BOX_NGROUPS][BOX_LABEL_MAX + 4];

	for (int g = 0; g < BOX_NGROUPS; g++) {
		char gn[BOX_LABEL_MAX + 4];
		int active = c->group_pins[g] != 0;

		gn[0] = '\0';
		if (active) {
			dserv_group_name(c, g, gn, sizeof gn);
		}
		if (group_pub_name[g][0] &&
		    (!active || strcmp(group_pub_name[g], gn) != 0)) {
			snprintf(leaf, sizeof leaf, "group/%s/pins", group_pub_name[g]);
			pub_str(c, leaf, "");
		}
		if (!active) {
			group_pub_name[g][0] = '\0';
			continue;
		}
		snprintf(group_pub_name[g], sizeof group_pub_name[g], "%s", gn);
		dserv_pins_str(c->group_pins[g], csv, sizeof csv);

		snprintf(leaf, sizeof leaf, "group/%s/pins", gn);
		pub_str(c, leaf, csv);
		snprintf(leaf, sizeof leaf, "group/%s/settle_ms", gn);
		pub_int(c, leaf, c->group_settle_ms[g]);
		snprintf(leaf, sizeof leaf, "group/%s/quiet", gn);
		pub_int(c, leaf, c->group_quiet[g]);
		snprintf(leaf, sizeof leaf, "group/%s/idx", gn);
		pub_int(c, leaf, g);
	}

	/* ANALOG groups -- the analog twin of the DI block, and NEW: until this,
	 * the manifest said nothing about them (only ain_en + ain/rate), so a
	 * config UI could neither show nor create one and the fleet page had no
	 * metadata for the ain/<label> data blocks. Announced under
	 * ain/group/<name>/* to mirror the config write path
	 * (config/ain/group/<i>/...); `idx` maps name -> slot for those writes.
	 * chans ascending is the blocks' column order (mask bit c = ch c), same
	 * decode-from-the-announced-string contract as the DI groups -- though
	 * the blocks also carry their mask, so they stand alone regardless.
	 * Same tombstone rule as above, on the chans leaf. */
	static char aingrp_pub_name[BOX_NAGROUPS][BOX_LABEL_MAX + 4];

	for (int g = 0; g < BOX_NAGROUPS; g++) {
		char gn[BOX_LABEL_MAX + 4];
		int active = c->ain_group_chans[g] != 0;

		gn[0] = '\0';
		if (active) {
			dserv_ain_group_name(c, g, gn, sizeof gn);
		}
		if (aingrp_pub_name[g][0] &&
		    (!active || strcmp(aingrp_pub_name[g], gn) != 0)) {
			snprintf(leaf, sizeof leaf, "ain/group/%s/chans", aingrp_pub_name[g]);
			pub_str(c, leaf, "");
		}
		if (!active) {
			aingrp_pub_name[g][0] = '\0';
			continue;
		}
		snprintf(aingrp_pub_name[g], sizeof aingrp_pub_name[g], "%s", gn);
		dserv_pins_str(c->ain_group_chans[g], csv, sizeof csv);

		snprintf(leaf, sizeof leaf, "ain/group/%s/chans", gn);
		pub_str(c, leaf, csv);
		snprintf(leaf, sizeof leaf, "ain/group/%s/mode", gn);
		pub_str(c, leaf, c->ain_group_mode[g] ? "continuous" : "onchange");
		snprintf(leaf, sizeof leaf, "ain/group/%s/deadband", gn);
		pub_int(c, leaf, c->ain_group_deadband[g]);
		snprintf(leaf, sizeof leaf, "ain/group/%s/decimate", gn);
		pub_int(c, leaf, c->ain_group_decimate[g] ? c->ain_group_decimate[g] : 1);
		snprintf(leaf, sizeof leaf, "ain/group/%s/batch", gn);
		pub_int(c, leaf, c->ain_group_batch[g] ? c->ain_group_batch[g] : 1);
		snprintf(leaf, sizeof leaf, "ain/group/%s/avg", gn);
		pub_int(c, leaf, (c->ain_group_flags[g] & AIN_GROUP_FLAG_AVG) ? 1 : 0);
		snprintf(leaf, sizeof leaf, "ain/group/%s/idx", gn);
		pub_int(c, leaf, g);
	}
}

/* ---- live levels, so a fresh host sees state without waiting for an edge ---- */

static void announce_levels(const box_config_t *c, const group_rt_t *groups)
{
	uint8_t levels[BOX_NPINS];
	char leaf[48];        /* "group/" + a max-length label must fit */

	box_gpio_read_di_levels(c, levels);
	for (int i = 0; i < BOX_NPINS; i++) {
		if (c->pin_mode[i] == 2 || c->pin_mode[i] == 3) {
			snprintf(leaf, sizeof leaf, "di/%u", i);
			pub_int(c, leaf, levels[i]);
		}
	}

	for (int g = 0; g < BOX_NGROUPS; g++) {
		char gn[BOX_LABEL_MAX + 4];

		if (!c->group_pins[g]) {
			continue;
		}
		dserv_group_name(c, g, gn, sizeof gn);
		snprintf(leaf, sizeof leaf, "group/%s", gn);
		pub_int(c, leaf, groups[g].cur);
	}
}

void box_announce_obs_role(const box_config_t *c)
{
	pub_str(c, "obs/mode",  obs_is_leader(c) ? "leader" :
				(obs_mirror_enabled(c) ? "mirror" : "off"));
	pub_int(c, "obs_leader", obs_is_leader(c) ? 1 : 0);
	/* v34: the pin rides every role announce -- auto-bind resolution
	 * reads it, and a role without its pin is exactly the half-config
	 * that stranded officepi (leader persisted, pin not). -1 is honest
	 * "leader with no pin configured"; the resolver refuses it loudly. */
	pub_int(c, "obs_pin", obs_mirror_enabled(c) ? obs_mirror_pin(c) : -1);
}

void box_announce_burst(const box_config_t *c, const group_rt_t *groups)
{
	announce_ident(c);
	box_announce_manifest(c);
	announce_levels(c, groups);
}
