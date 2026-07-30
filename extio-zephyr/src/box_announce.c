/*
 * box_announce.c -- see box_announce.h.
 */
#include "box_announce.h"
#include "box_boot.h"
#include "box_gpio.h"
#include "box_uplink.h"

#include <zephyr/kernel.h>
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
static const char *box_build_key(void)
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

/* ---- small helpers: one datapoint per item ---- */

static void pub_str(const box_config_t *c, const char *leaf, const char *val)
{
	uint8_t f[DSERV_MSG_LEN];
	char nm[80];

	dserv_state_name(c, nm, sizeof nm, leaf);
	dserv_msg_string(f, nm, 0, val);
	box_uplink_send(f, DSERV_MSG_LEN);
}

static void pub_int(const box_config_t *c, const char *leaf, int64_t val)
{
	uint8_t f[DSERV_MSG_LEN];
	char nm[80];

	dserv_state_name(c, nm, sizeof nm, leaf);
	dserv_msg_int(f, nm, 0, (int) val);
	box_uplink_send(f, DSERV_MSG_LEN);
}

/* ---- identity ---- */

static void announce_ident(const box_config_t *c)
{
	char s[24];

	pub_str(c, "transport", box_uplink_active_name());
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
	}
	pub_str(c, "build",     box_build_key());     /* shelf image match key   */
	pub_str(c, "board",     BOX_BOARD_ID);       /* OTA compat filter       */
	pub_str(c, "channel",   dserv_cfg_channel(c));
	/* which device the console bound to -- a timing-relevant choice, so a host
	 * can see it without guessing (see config/console). */
	pub_str(c, "console",   dserv_console_str((uint8_t) dserv_cfg_console_mode(c)));

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

void box_announce_manifest(const box_config_t *c)
{
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

	/* Special-function pins; -1 = off, so DISABLING one updates the retained
	 * value instead of leaving a ghost pointing at a pin that is now ordinary. */
	pub_int(c, "obs_pin",  obs_mirror_enabled(c) ? obs_mirror_pin(c)  : -1);
	pub_int(c, "sync_pin", sync_input_enabled(c) ? sync_input_pin(c) : -1);

	/* Feature flags, so a UI can shade pins claimed by a peripheral without
	 * hardcoding the pin budget. Neither is implemented on this port yet -- they
	 * are announced honestly as configured, not as running. */
	pub_int(c, "ain_en",  c->ain_en  ? 1 : 0);
	pub_int(c, "oled_en", c->oled_en ? 1 : 0);

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

	/* DI chord groups. `pins` ascending IS the published bit order, so a host
	 * decodes state/group/<label> from this string alone; `idx` maps a labeled
	 * group back to its `group <n>` slot for editing. */
	for (int g = 0; g < BOX_NGROUPS; g++) {
		char gn[BOX_LABEL_MAX + 4];

		if (!c->group_pins[g]) {
			continue;
		}
		dserv_group_name(c, g, gn, sizeof gn);
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

void box_announce_burst(const box_config_t *c, const group_rt_t *groups)
{
	announce_ident(c);
	box_announce_manifest(c);
	announce_levels(c, groups);
}
