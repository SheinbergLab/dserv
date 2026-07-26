/*
 * extio-zephyr -- Zephyr application entry: the converged extio box.
 *
 * Boot order matters: GPIO and the uplink come up BEFORE any printk, because on
 * boards whose console is the box's own USB CDC (the board overlays) anything
 * printed before enumeration is dropped. After a settle delay we run the core
 * smoke test (the on-target twin of tools/box_sim.c --selftest), then enter the
 * service loop.
 *
 * The loop is the whole box in one place: arbitrate the uplink (USB / Ethernet
 * by carrier), dispatch inbound frames to config + GPIO, and publish local DI,
 * BLE ingress, and a 1 Hz watchdog out whichever uplink is active. Subsystems
 * absent on a given board (Ethernet on teensy40, BLE on any Teensy) are
 * compiled out via CONFIG_NETWORKING / CONFIG_BT -- see boards/<board>.conf.
 */
#include <zephyr/kernel.h>
#include <zephyr/version.h>
#include <zephyr/sys/reboot.h>
#include <stdio.h>

#include "dserv_config.h"
#include "box_persist.h"
#include "box_gpio.h"
#include "box_sched.h"
#include "box_group.h"
#include "box_clock.h"
#include "box_announce.h"
#include "box_obs.h"
#include "box_console.h"
#if defined(BOX_HAVE_PERSIST)
#include "box_flash.h"
#endif
#include "box_net_usb.h"
#include "box_uplink.h"
#include "box_event.h"
#if defined(CONFIG_NETWORKING)
#include "box_net_eth.h"
#include "box_ptp.h"
#endif
#if defined(CONFIG_BT)
#include "box_ble.h"
#endif

static box_config_t   cfg;
static dserv_framer_t rx_framer;
static group_rt_t     groups[BOX_NGROUPS];   /* DI chord-settle state machines */
static box_clock_t    boxclk;                /* box time -> dserv time alignment */
static uint64_t       obs_begin_us;          /* box time of the last beginobs anchor */
#if defined(BOX_HAVE_PERSIST)
static int            box_persist_mount_err;   /* 0 = NVS mounted; else the errno */
#endif

/* ---- PTP anchoring -------------------------------------------------------
 *
 * When the box's 1588 clock is disciplined to the host NIC's PHC by PTP, the
 * host can hand us ONE constant:
 *
 *     ptp_offset_us = dserv_us - ptp_us
 *
 * It is constant because both terms are: dserv time is CLOCK_MONOTONIC plus a
 * fixed epoch offset, and the PHC is rate-locked to that same clock by phc2sys
 * (0.002 ppm -- see PORTING.md). The host measures it locally, with no wire
 * involved, so unlike an obs anchor it carries NO TRANSPORT TERM AT ALL. That is
 * the whole point: the one-way delay that has been so hard to subtract simply is
 * not in this path.
 *
 * With it we can re-anchor box_clock from the PTP clock alone, as often as we
 * like, without exchanging a single packet.
 */
#if defined(CONFIG_PTP_CLOCK)
static int64_t  ptp_offset_us;        /* dserv_us - ptp_us, from the host */
static int      ptp_offset_valid;

/* Sandwich a PTP read between two local reads, exactly as host/phc_offset.c
 * does: the midpoint pairs the two clocks and the spread bounds our own error.
 * Returns 0 and fills *box_us / *ptp_us, or -1 if the PTP clock is not ready. */
static int ptp_pair_read(uint64_t *box_us, uint64_t *ptp_us, uint32_t *window_us)
{
	if (!box_ptp_ready()) {
		return -1;
	}
	uint64_t b0 = box_gpio_now_us();
	uint64_t p  = box_ptp_now_ns();
	uint64_t b1 = box_gpio_now_us();

	if (!p) {
		return -1;
	}
	*box_us = (b0 + b1) / 2;
	*ptp_us = p / 1000u;
	if (window_us) {
		*window_us = (uint32_t)(b1 - b0);
	}
	return 0;
}

/* Re-anchor box_clock from PTP. Trusted, so it also teaches the rate estimator
 * the box-local-vs-PTP crystal error -- which is exactly what that estimator
 * was built for, now fed by a source with no transport jitter. */
static int ptp_reanchor(uint32_t *window_us)
{
	uint64_t box_us, ptp_us;

	if (!ptp_offset_valid || ptp_pair_read(&box_us, &ptp_us, window_us) != 0) {
		return -1;
	}
	box_clock_sync(&boxclk, (uint64_t)((int64_t) ptp_us + ptp_offset_us),
		       box_us, 1 /* trusted */);
	return 0;
}
#endif /* CONFIG_PTP_CLOCK */

/* A hardware sync edge may anchor an obs toggle only if it is RECENT: well under
 * the shortest obs on/off cadence (seconds), so a stale latched edge from the
 * previous toggle can never anchor the current one. */
#define SYNC_EDGE_WINDOW_US 250000

/* Every published event time goes through here. Before the first sync this
 * returns 0, which tells dserv to arrival-stamp -- events still publish, they
 * are just not aligned yet. */
static inline uint64_t event_stamp(uint64_t t_us)
{
	return box_clock_stamp(&boxclk, t_us);
}

/* Publish one settled chord: extio/<name>/state/group/<label>, value = the
 * member bitmask (bit i = i-th LOWEST member pin, the order the manifest
 * announces), stamped at the episode's onset edge. */
static void publish_group(int g, uint8_t bits, uint64_t t_us)
{
	uint8_t f[DSERV_MSG_LEN];
	char gn[BOX_LABEL_MAX + 4], leaf[40], nm[80];

	dserv_group_name(&cfg, g, gn, sizeof gn);
	snprintf(leaf, sizeof leaf, "group/%s", gn);
	dserv_state_name(&cfg, nm, sizeof nm, leaf);
	dserv_msg_int(f, nm, t_us ? event_stamp(t_us) : 0, bits);
	box_uplink_send(f, DSERV_MSG_LEN);
}

/* Publish a DO level: extio/<name>/state/do/<pin>, stamped at the instant the
 * pin actually moved (not at command arrival). Matches the Pico exactly --
 * SET only, from the datapoint path only -- so hosts decode both boxes alike.
 * Consumers: UIs showing live output state, and host/loopback_rtt.tcl, which
 * reads it to pick the next level (the .sh variant just alternates). */
static void publish_do(uint8_t pin, uint8_t level, uint64_t stamp)
{
	uint8_t f[DSERV_MSG_LEN];
	char leaf[24], nm[80];

	snprintf(leaf, sizeof leaf, "do/%u", pin);
	dserv_state_name(&cfg, nm, sizeof nm, leaf);
	dserv_msg_int(f, nm, stamp, level);
	box_uplink_send(f, DSERV_MSG_LEN);
}

/* Clock-alignment telemetry, published at every obs anchor. This is how you tell
 * from the host side whether stamping is trustworthy: `source` says whether the
 * anchor was the hardware TTL edge or mere frame arrival, `transport_us` is the
 * frame's transit lag measured against that edge (hw anchors only -- it is
 * exactly the error a hw anchor removes), and `rate_ppb` appears once enough
 * trusted pairs have taught the crystal rate. */
static void publish_sync(uint64_t dserv_us, uint64_t box_us, int64_t offset_us,
			 int hw, int64_t transport_us)
{
	uint8_t f[DSERV_MSG_LEN];
	char nm[80];

	dserv_state_name(&cfg, nm, sizeof nm, "sync/dserv_us");
	dserv_msg_int64(f, nm, dserv_us, (int64_t) dserv_us);
	box_uplink_send(f, DSERV_MSG_LEN);

	dserv_state_name(&cfg, nm, sizeof nm, "sync/box_us");
	dserv_msg_int64(f, nm, dserv_us, (int64_t) box_us);
	box_uplink_send(f, DSERV_MSG_LEN);

	dserv_state_name(&cfg, nm, sizeof nm, "sync/offset_us");
	dserv_msg_int64(f, nm, dserv_us, offset_us);
	box_uplink_send(f, DSERV_MSG_LEN);

	dserv_state_name(&cfg, nm, sizeof nm, "sync/source");
	dserv_msg_string(f, nm, dserv_us, hw ? "hw" : "sw");
	box_uplink_send(f, DSERV_MSG_LEN);

	if (transport_us >= 0) {
		dserv_state_name(&cfg, nm, sizeof nm, "sync/transport_us");
		dserv_msg_int64(f, nm, dserv_us, transport_us);
		box_uplink_send(f, DSERV_MSG_LEN);
	}
	if (boxclk.rate_valid) {          /* learned crystal rate: hw anchors only */
		dserv_state_name(&cfg, nm, sizeof nm, "sync/rate_ppb");
		dserv_msg_int(f, nm, dserv_us, boxclk.rate_ppb);
		box_uplink_send(f, DSERV_MSG_LEN);
	}
}

/* (Re)seed every group from the pins' CURRENT logical levels, so a switch
 * already held when a group is (re)configured is the baseline rather than a
 * phantom edge. Call after any change to the group/pin map. */
static void groups_resync(void)
{
	uint8_t levels[BOX_NPINS];

	box_gpio_read_di_levels(&cfg, levels);
	for (int g = 0; g < BOX_NGROUPS; g++) {
		group_reset(&groups[g], &cfg, g, levels);
	}
}

/* Demo pins, per board. box pin n -> <box-gpio-port>.n (see box_gpio.h), so
 * these land on real hardware on each target. */
#if defined(CONFIG_BOARD_FRDM_RW612)
#define LED_PIN  12   /* hsgpio0.12 = user LED (active low physically) */
#define BTN_PIN  11   /* hsgpio0.11 = User SW2 (active low, pull-up)    */
#else                 /* Teensy 4.x: board LED is gpio2.3 */
#define LED_PIN  3    /* gpio2.3 = on-board LED                         */
#define BTN_PIN  4    /* gpio2.4 = a free pin for a test button         */
#endif

/* The rig's obs begin/end edge. The host module forwards this to EVERY box
 * (config/extioconf.tcl: dservAddMatch ess/in_obs -> usbio_forward), and unlike
 * everything else inbound it is NOT an extio/<name>/... key -- so dserv_dispatch
 * never matches it and it must be handled before the dispatch, exactly as the
 * Pico's frame handler does. */
#define BOX_SYNC_DP "ess/in_obs"

/* One inbound 128-byte frame (config/cmd/ess-in_obs) from the host module:
 * dispatch it into the config, and run any GPIO command it produced. */
static void on_usb_frame(const uint8_t *frame, void *ud)
{
	ARG_UNUSED(ud);
	dserv_msg_t m;
	if (dserv_msg_parse(frame, &m) != 0) {
		return;
	}

#if defined(CONFIG_PTP_CLOCK)
	/* <prefix>/cmd/ptp/offset <us> -- the host's PHC->dserv constant.
	 *
	 * Handled HERE rather than in dserv_cfg__cmd() because src/core/ is shared
	 * verbatim with the Pico and this is Zephyr/PTP-only. Same precedent as
	 * ess/in_obs below, which is also intercepted before dispatch.
	 *
	 * Anchoring is gated on !in_obs for the reason PORTING.md records: a
	 * re-anchor STEPS the offset, and applying that inside a data-collection
	 * window puts two events of one trial on different mappings. With PTP the
	 * step is sub-us rather than the hundreds of us an obs anchor moves, but
	 * the rule is about correctness, not magnitude.
	 */
	{
		char pfx[64], key[96];
		dserv_cfg_prefix(&cfg, pfx, sizeof pfx);
		snprintf(key, sizeof key, "%s/cmd/ptp/offset", pfx);
		if (dserv_msg_name_eq(&m, key)) {
			ptp_offset_us    = (int64_t) dserv_msg_as_long(&m);
			ptp_offset_valid = 1;

			uint32_t win = 0;
			int ok = box_obs_active() ? -1 : ptp_reanchor(&win);

			uint8_t f2[DSERV_MSG_LEN];
			char nm[80];
			dserv_state_name(&cfg, nm, sizeof nm, "ptp/offset_us");
			dserv_msg_int64(f2, nm, 0, ptp_offset_us);
			box_uplink_send(f2, DSERV_MSG_LEN);

			if (ok == 0) {
				/* A FOURTH sync source beside hw / swc / sw, so a datafile
				 * records which mechanism produced its timestamps. */
				dserv_state_name(&cfg, nm, sizeof nm, "sync/source");
				dserv_msg_string(f2, nm, 0, "ptp");
				box_uplink_send(f2, DSERV_MSG_LEN);

				dserv_state_name(&cfg, nm, sizeof nm, "sync/ptp_window_us");
				dserv_msg_int(f2, nm, 0, (int32_t) win);
				box_uplink_send(f2, DSERV_MSG_LEN);

				dserv_state_name(&cfg, nm, sizeof nm, "sync/offset_us");
				dserv_msg_int64(f2, nm, 0, boxclk.offset_us);
				box_uplink_send(f2, DSERV_MSG_LEN);
			}
			return;
		}
	}
#endif

	if (dserv_msg_name_eq(&m, BOX_SYNC_DP)) {
		int obs = (int) dserv_msg_as_long(&m);
		uint64_t now_box = box_gpio_now_us();

		/* ANCHOR. Prefer the IRQ-latched TTL edge (jitter ~us) over frame
		 * arrival (100s of us of transport jitter): a hardware anchor takes
		 * the transport out of the error budget entirely, and only trusted
		 * (hw) anchors are allowed to teach the crystal rate. */
		uint64_t anchor_box = now_box;
		int hw = 0;

		if (sync_input_enabled(&cfg)) {
			uint64_t e = box_gpio_sync_edge_us(obs);   /* rising for obs=1 */

			if (e && now_box - e < SYNC_EDGE_WINDOW_US) {
				anchor_box = e;
				hw = 1;
			}
		}
		box_clock_sync(&boxclk, m.timestamp, anchor_box, hw);
		box_obs_set(obs);            /* keep the LEVEL, not just the edge */
		if (obs) {
			obs_begin_us = anchor_box;   /* epoch for box-scheduled events */
		}

		/* drive the obs-mirror output (LED / scope trace) */
		box_gpio_obs_mirror(&cfg, obs);

		/* publish the box's OWN live copy, so obs state is visible per-box in
		 * dserv without a scope -- honest, since it only updates when THIS box
		 * actually received the edge. */
		uint8_t of[DSERV_MSG_LEN];
		char onm[80];
		dserv_state_name(&cfg, onm, sizeof onm, "in_obs");
		dserv_msg_int(of, onm, m.timestamp, obs);
		box_uplink_send(of, DSERV_MSG_LEN);

		publish_sync(m.timestamp, anchor_box, boxclk.offset_us, hw,
			     hw ? (int64_t)(now_box - anchor_box) : -1);
		return;
	}

	gpio_cmd_t cmd;
	cfg_result_t r = dserv_dispatch(&cfg, &m, &cmd);
	if (r == CFG_GPIO && cmd.op == GPIO_OP_SCHED_PULSE) {
		/* do/<n>/at: pulse at beginobs + delta, width from the pin's
		 * configured pulse_us -- and post state/timer/<n> at the fire. */
		if (obs_begin_us == 0) {
			box_console_printf("sched: no beginobs yet, ignoring\n");
		} else {
			uint32_t w = cfg.do_pulse_us[cmd.pin] ? cfg.do_pulse_us[cmd.pin] : 1000;
			if (box_sched_arm(&cfg, cmd.pin, cmd.pin, w,
					  obs_begin_us + cmd.value) != 0) {
				box_console_printf("sched: table full\n");
			}
		}
	} else if (r == CFG_GPIO && cmd.op == GPIO_OP_SCHED_TIMER) {
		/* timer/<t>/at: notify-only at beginobs + delta */
		if (obs_begin_us == 0) {
			box_console_printf("sched: no beginobs yet, ignoring\n");
		} else if (box_sched_arm(&cfg, BOX_SCHED_NOTIFY_ONLY, cmd.pin, 0,
					 obs_begin_us + cmd.value) != 0) {
			box_console_printf("sched: table full\n");
		}
	} else if (r == CFG_GPIO && cmd.op != GPIO_OP_NONE) {
		box_gpio_exec(&cfg, &cmd);            /* immediate DO set/pulse */
		if (cmd.op == GPIO_OP_SET) {
			/* the pin has just moved -> stamp its actuation, not arrival */
			publish_do(cmd.pin, (uint8_t) (cmd.value ? 1 : 0),
				   event_stamp(box_gpio_now_us()));
		}
	} else if (r == CFG_CONSOLE) {
		box_console_printf("console=%s -- save+reboot to apply\n",
		       dserv_console_str((uint8_t) dserv_cfg_console_mode(&cfg)));
	} else if (r == CFG_GROUP || r == CFG_LABEL || r == CFG_DESC) {
		/* group/label/desc change: reseed the chord machines from the
		 * pins' current levels, and re-announce so the edit reaches
		 * consumers without waiting for a reconnect. */
		if (r == CFG_GROUP) {
			groups_resync();
		}
		box_announce_manifest(&cfg);
	} else if (r == CFG_PIN_MODE || r == CFG_OBS_PIN || r == CFG_SYNC_PIN ||
		   r == CFG_ACTIVE_LOW || r == CFG_DEBOUNCE) {
		/* ANY change to the pin map has to be pushed to the hardware. Notably
		 * obs/sync pins are claimed (as output / edge-latched input) only by
		 * apply_config -- without this, `config/obs/pin` set the config but
		 * left the pin unclaimed, so the mirror drove nothing. The console CLI
		 * path already re-applied (CLI_PIN); the datapoint path did not. */
		box_gpio_apply_config(&cfg);
		groups_resync();          /* DI levels may have changed meaning */
		box_announce_manifest(&cfg);   /* pins/in|out, obs_pin, sync_pin moved */
	} else if (r == CFG_SAVE) {
#if defined(BOX_HAVE_PERSIST)
		if (box_obs_active()) {      /* never program flash mid-trial */
			box_obs_defer(BOX_DEFER_SAVE);
			box_console_printf("cmd/save -> deferred to end of obs\n");
			return;
		}
		/* Persist the whole config blob so it survives reboot/power-cycle. */
		uint8_t blob[BOX_PERSIST_BLOB_MAX];
		uint32_t n = box_persist_serialize(&cfg, blob, sizeof blob);
		int rc = box_flash_save(blob, n);
		box_console_printf("cmd/save -> %s (%u bytes)\n", rc == 0 ? "ok" : "FAILED", n);
#else
		box_console_printf("cmd/save -> no persistence on this board\n");
#endif
	} else if (r == CFG_REBOOT) {
		/* Warm reset: the firmware restarts. Portable on every board. NOTE this
		 * does NOT enter the bootloader -- see CFG_BOOTSEL below. */
		box_console_printf("cmd/reboot -> warm reset\n");
		k_msleep(100);                        /* let the console drain */
		sys_reboot(SYS_REBOOT_WARM);
	} else if (r == CFG_BOOTSEL) {
		/* Program-mode entry is board-specific and NOT universally reachable:
		 *   RP2350   reset_usb_boot() -- trivial (the Pico's `bootsel`)
		 *   Teensy   bootloader is a separate chip watching the Program button;
		 *            Teensyduino's handshake is not exposed by Zephyr
		 *   RW612    moot -- MCUboot + mcumgr does DFU over the live link
		 * Report honestly rather than silently doing nothing. */
		box_console_printf("cmd/bootsel -> not supported on this board; "
		       "press the Program button to enter the bootloader\n");
	}
}

static cfg_result_t feed(const uint8_t *frame, gpio_cmd_t *cmd)
{
	dserv_msg_t m;
	if (dserv_msg_parse(frame, &m) != 0) {
		return CFG_NONE;
	}
	return dserv_dispatch(&cfg, &m, cmd);
}

int main(void)
{
	uint8_t f[DSERV_MSG_LEN];
	gpio_cmd_t cmd;
	cfg_result_t r;

	/* NOTE: NVS mount is deferred until AFTER the console is up (below) so a
	 * flash fault/hang is visible rather than silent -- an XIP-from-flash erase
	 * on the RT1062 is a known hazard. Demo pins for now; a loaded config
	 * re-applies after the mount. */
	int cfg_loaded = 0;
	cfg.pin_mode[LED_PIN] = 1;   /* demo output   */
	cfg.pin_mode[BTN_PIN] = 3;   /* demo in_pullup */

	/* Bring the platform up BEFORE any printk. On boards whose console is the
	 * box's own USB CDC (see the board overlays), output produced before the
	 * device enumerates is lost -- so init GPIO (so inbound commands can act
	 * immediately), then the uplink, then give the host a moment to open the
	 * console port before we start talking. */
	if (box_gpio_init() != 0) {
		/* nothing to print to yet; the LED demo below simply won't run */
	}
	box_sched_init();
	box_gpio_apply_config(&cfg);

	/* Load the persisted config BEFORE the uplink AND before the console.
	 *
	 * Before the UPLINK because transport_mode is persisted policy that the
	 * uplink acts on at init: `mode eth` suppresses the USB data pipe entirely
	 * (box_usbd_start), and enumeration happens inside box_uplink_init. Load it
	 * afterwards and that decision is made against the FACTORY DEFAULT -- the box
	 * enumerates a data pipe a declared-Ethernet box was told not to have, with
	 * no symptom other than the setting appearing to do nothing.
	 *
	 * Before the CONSOLE because console_mode (v22) decides WHICH device the
	 * console binds to, so the saved choice has to be known first.
	 *
	 * A flash fault here has no box-console to report on, but Zephyr's own
	 * printk/LOG is on the board UART (chosen zephyr,console) and still works --
	 * the boot-log channel PORTING.md documents. */
#if defined(BOX_HAVE_PERSIST)
	int frc = box_flash_init();
	if (frc != 0) {
		box_persist_mount_err = frc;   /* surfaced in the boot banner */
	}
	if (frc == 0) {
		uint8_t lb[BOX_PERSIST_BLOB_MAX];
		int ln = box_flash_load(lb, sizeof lb);

		cfg_loaded = (ln > 0 && box_persist_deserialize(lb, (uint32_t) ln, &cfg) == 0);
		if (cfg_loaded) {
			box_gpio_apply_config(&cfg);   /* apply the loaded pin map/name */
			groups_resync();               /* seed chords from real pin state */
		}
	}
#endif

#if defined(BOX_BRINGUP_NET_IP)
	/* BRING-UP SCAFFOLD -- now RETIRABLE, kept only as a factory-reset default.
	 *
	 * Added while `cmd/save` was broken on the FRDM-RW612, which made the box's
	 * own IP unconfigurable: net/ip applies only in box_net_eth_init() at boot,
	 * so a runtime change needs a reboot, and a reboot without persistence
	 * forgets it. Circular -- hence a compiled-in default.
	 *
	 * That is FIXED (PARTITION_OFFSET + NVS_INIT_BAD_MEMORY_REGION; see
	 * PORTING.md 2026-07-25), so this is inert whenever a saved config exists:
	 * it is applied ONLY when nothing loaded from flash. Its remaining value is
	 * that a factory-reset box still lands on the bench network.
	 *
	 * DO NOT let it become load-bearing -- it hardcodes an IP in firmware.
	 * Delete the block and the CMakeLists definitions when the rig outgrows it.
	 */
	if (!cfg_loaded) {
		if (dserv_cfg__parse_ip(BOX_BRINGUP_NET_IP, cfg.net_ip) == 0) {
			cfg.net_mode = NET_MODE_STATIC;
		}
		dserv_cfg__parse_ip(BOX_BRINGUP_NET_MASK, cfg.net_sn);
		dserv_cfg__parse_ip(BOX_BRINGUP_DSERV_IP, cfg.net_gw);
		dserv_cfg__parse_ip(BOX_BRINGUP_DSERV_IP, cfg.dserv_ip);
	}
#endif

	box_uplink_init(&cfg);       /* USB (and Ethernet where present) up */

	box_console_init(&cfg);      /* binds CDC or console UART per console_mode */
	dserv_framer_reset(&rx_framer);
	k_msleep(2000);              /* let the host enumerate + open the console */

	box_console_printf("\n=== extio core smoke test (Zephyr %s) ===\n", KERNEL_VERSION_STRING);

	/* config datapoint: set pin 5 to output */
	dserv_msg_int(f, "extio/box/config/pin/5/mode", 0, 1);
	r = feed(f, &cmd);
	box_console_printf("apply config/pin/5/mode=out    -> %-9s pin_mode[5]=%u\n",
	       dserv_cfg_result_str(r), cfg.pin_mode[5]);

	/* No demo dserv target: leaving dserv_ip unset keeps eth from claiming the
	 * uplink (it needs a configured target), so a bench box stays on USB and
	 * reachable. A real box gets its target from persisted config / the console
	 * CLI (a later block). */

	/* transient command: box-timed pulse on pin 6 -> a gpio_cmd for the platform */
	dserv_msg_int(f, "extio/box/cmd/do/6/pulse_us", 0, 500);
	r = feed(f, &cmd);
	box_console_printf("apply cmd/do/6/pulse_us=500    -> %-9s op=%d pin=%u value=%u\n",
	       dserv_cfg_result_str(r), cmd.op, cmd.pin, cmd.value);

	/* persistence round-trip (the flash write itself is platform; the codec is core) */
	uint8_t blob[BOX_PERSIST_BLOB_MAX];
	uint32_t n = box_persist_serialize(&cfg, blob, sizeof blob);
	box_config_t restored = {0};
	int ok = box_persist_deserialize(blob, n, &restored);
	box_console_printf("persist round-trip             -> %-9s %u bytes, applied_count=%u\n",
	       ok == 0 ? "ok" : "FAIL", n, restored.applied_count);

#if defined(BOX_HAVE_PERSIST)
	/* (the store was mounted + loaded above, before the console bound) */
	box_console_printf("persist store                  -> config %s\n",
	       cfg_loaded ? "LOADED from flash" : "fresh (defaults)");
#else
	box_console_printf("persist store                  -> none on this board\n");
#endif

	/* the box's datapoint identity */
	char pfx[64];
	dserv_cfg_prefix(&cfg, pfx, sizeof pfx);
	box_console_printf("datapoint prefix               -> %s  (dserv port %u)\n",
	       pfx, dserv_cfg_port(&cfg));

	box_console_printf("=== codec smoke test done ===\n\n");

	/* ---- block #3: GPIO (already initialised above, before the console) ---- */
	box_console_printf("gpio: pin %d=out (LED), pin %d=in_pullup\n", LED_PIN, BTN_PIN);

	/* Boot heartbeat: three hardware-timed LED pulses (the hardware counter
	 * drops each falling edge, not software). */
	gpio_cmd_t pulse = { .op = GPIO_OP_PULSE, .pin = LED_PIN, .value = 120000 };
	for (int i = 0; i < 3; i++) {
		box_gpio_exec(&cfg, &pulse);
		k_msleep(250);
	}

#if defined(CONFIG_NETWORKING)
	/* one-shot status: DHCP lease (if eth came up) + the PTP hardware clock */
	uint8_t ip[4];
	if (box_net_eth_wait_ip(ip, 5000)) {
		box_console_printf("eth DHCP IPv4: %u.%u.%u.%u  link=%d\n",
		       ip[0], ip[1], ip[2], ip[3], box_net_eth_link());
	} else {
		box_console_printf("eth: no lease (link=%d)\n", box_net_eth_link());
	}
#if defined(BOX_HAVE_PERSIST)
	/* Say so LOUDLY when persistence is dead: every `save` will fail and every
	 * reboot silently reverts to defaults, which reads as a firmware bug. */
	if (box_persist_mount_err) {
		/* Everything needed to diagnose it, in one line: a bare "FAILED" sent
		 * this hunt into the FlexSPI driver for hours when the answer was a
		 * wrong partition offset. See PORTING.md 2026-07-25. */
		int pirc = 0; uint32_t pisz = 0, poff = 0, ss = 0, sc = 0;
		box_flash_debug(&pirc, &pisz, &poff);
		box_flash_geometry(&ss, &sc);
		box_console_printf("persist: NVS MOUNT FAILED err=%d -- `save` will fail, "
		                   "config will NOT survive reboot\n", box_persist_mount_err);
		box_console_printf("persist:   page_info(off=0x%x) rc=%d size=%u; "
		                   "sectors %ux%u\n",
		                   (unsigned) poff, pirc, (unsigned) pisz,
		                   (unsigned) sc, (unsigned) ss);
	}
#endif
	box_console_printf("PTP hw clock: ready=%d  now=%llu ns\n",
	       (int) box_ptp_ready(), (unsigned long long) box_ptp_now_ns());
#else
	box_console_printf("no Ethernet on this board -- USB-only uplink\n");
#endif
	box_console_printf("console: %s (config/console cdc|uart; save+reboot)\n",
	       dserv_console_str((uint8_t) dserv_cfg_console_mode(&cfg)));
	box_console_printf("active uplink: %s\n", box_uplink_active_name());

#if defined(CONFIG_BT)
	/* ---- block #6 (ingress): multi-peripheral BLE central ---- */
	if (box_ble_init() == 0) {
		box_console_printf("BLE central up; scanning for d5e7000x peripherals (max %d)\n\n",
		       CONFIG_BT_MAX_CONN);
	} else {
		box_console_printf("BLE init failed (continuing wired-only)\n\n");
	}
#else
	box_console_printf("no radio on this board -- BLE ingress disabled\n\n");
#endif

	/* ---- converged box service loop (blocks #2-6 together) ----
	 * arbitrate the uplink; inbound frames -> dispatch -> GPIO; local DI, BLE
	 * ingress, and a 1 Hz watchdog -> whichever uplink is active. */
	uint8_t rx[256];
	int watchdog = 0;
	uint32_t loop_last_us = 0, loop_max_us = 0, loop_t0 = 0;
	uint32_t disp_last_us = 0, disp_max_us = 0;
	int64_t next_wd = k_uptime_get() + 1000;
	char name[80];

	while (1) {
		box_uplink_service(&cfg);         /* carrier/strap selection + (re)connect */
		box_console_service(&cfg);        /* two-way CLI (non-blocking, bounded) */

		int n = box_uplink_poll(rx, sizeof rx);
		if (n == BOX_NET_RESET) {
			dserv_framer_reset(&rx_framer);
			/* A host just opened the pipe. dserv only learns what it is
			 * told while listening, so describe the box now. */
			box_announce_burst(&cfg, groups);
		} else if (n > 0) {
			/* Cost of turning received bytes into an executed command
			 * (framer + dispatch + GPIO write). Completes the inbound
			 * split alongside wake_us / recv_us in box_net_eth.c. */
			uint32_t d0 = k_cycle_get_32();
			dserv_framer_feed(&rx_framer, rx, (uint32_t) n, on_usb_frame, NULL);
			uint32_t dd = k_cyc_to_us_floor32(k_cycle_get_32() - d0);
			disp_last_us = dd;
			if (dd > disp_max_us) {
				disp_max_us = dd;
			}
		}

		box_di_event_t ev;
		while (box_gpio_poll_di(&cfg, &ev)) {
			uint8_t f[DSERV_MSG_LEN];
			char leaf[24];
			/* publish the LOGICAL level, so `pin N active_low 1` means
			 * something on the wire (box_gpio reports raw). */
			int lvl = di_logical(&cfg, ev.pin, ev.level);

			/* Feed every configured chord group. A member of a `quiet`
			 * group is reported ONLY as part of its settled chord -- the
			 * group replaces the per-pin event rather than doubling it. */
			int quiet = 0;
			for (int g = 0; g < BOX_NGROUPS; g++) {
				if (cfg.group_pins[g] &&
				    group_feed(&groups[g], &cfg, g, ev.pin, lvl, ev.t_us) &&
				    cfg.group_quiet[g]) {
					quiet = 1;
				}
			}
			if (quiet) {
				continue;
			}
			snprintf(leaf, sizeof leaf, "di/%u", ev.pin);
			dserv_state_name(&cfg, name, sizeof name, leaf);   /* extio/<name>/state/di/<pin> */
			dserv_msg_int(f, name, event_stamp(ev.t_us), lvl);
			box_uplink_send(f, DSERV_MSG_LEN);
		}

		/* Settle windows expire between edges, so poll them every pass --
		 * a chord closes settle_ms after its LAST member edge, and is stamped
		 * at the FIRST (the true movement onset). */
		{
			group_out_t go[2];
			uint64_t gnow = box_gpio_now_us();

			for (int g = 0; g < BOX_NGROUPS; g++) {
				if (!cfg.group_pins[g]) {
					continue;
				}
				int gn = group_poll(&groups[g], &cfg, g, gnow, go);

				for (int k = 0; k < gn; k++) {
					publish_group(g, go[k].bits, go[k].t_us);
				}
			}
		}

		/* Scheduled events that fired (timer ISR already drove the pulse):
		 * post state/timer/<tid>, stamped at the INTENDED fire instant --
		 * not at this drain -- so host-side timing analysis sees the truth. */
		{
			box_sched_fired_t sf;

			while (box_sched_poll(&sf)) {
				uint8_t f[DSERV_MSG_LEN];
				char leaf[20];

				snprintf(leaf, sizeof leaf, "timer/%u", sf.tid);
				dserv_state_name(&cfg, name, sizeof name, leaf);
				dserv_msg_int(f, name, event_stamp(sf.fire_us), 1);
				box_uplink_send(f, DSERV_MSG_LEN);
			}
		}

#if defined(CONFIG_BT)
		/* BLE ingress: each peripheral's frame is already source-stamped
		 * (extio/<client>/...); relay it out the active uplink verbatim. */
		uint8_t bframe[DSERV_MSG_LEN];
		while (box_ble_poll(bframe)) {
			box_uplink_send(bframe, DSERV_MSG_LEN);
		}
#endif

		if (k_uptime_get() >= next_wd) {
			next_wd += 1000;
			uint8_t f[DSERV_MSG_LEN];
			dserv_state_name(&cfg, name, sizeof name, "watchdog");
			dserv_msg_int(f, name, 0, watchdog++);
			{
				/* USB caller cost + TX-ring drops. Published on EVERY
				 * board: the comparison against the Ethernet numbers is
				 * what turns "how many datapoints can I push" into a
				 * budget rather than a guess. */
				uint32_t ul = 0, um = 0, ud = 0;
				box_net_usb_send_stats(&ul, &um, &ud);
				box_uplink_send(f, DSERV_MSG_LEN);
				dserv_state_name(&cfg, name, sizeof name, "dbg/usb_send_us");
				dserv_msg_int(f, name, 0, (int32_t) ul);
				box_uplink_send(f, DSERV_MSG_LEN);
				dserv_state_name(&cfg, name, sizeof name, "dbg/usb_send_max_us");
				dserv_msg_int(f, name, 0, (int32_t) um);
				box_uplink_send(f, DSERV_MSG_LEN);
				dserv_state_name(&cfg, name, sizeof name, "dbg/usb_drops");
				dserv_msg_int(f, name, 0, (int32_t) ud);
				box_uplink_send(f, DSERV_MSG_LEN);
				dserv_state_name(&cfg, name, sizeof name, "watchdog");
				dserv_msg_int(f, name, 0, watchdog - 1);
			}
#if defined(CONFIG_NETWORKING)
			/* Publish-latency investigation: how long does one
			 * zsock_send() actually take, and how long is a full
			 * service-loop pass? Two frames the box emits us apart
			 * arrive at dserv ~650 us apart and three host-side
			 * theories were wrong -- these two numbers separate
			 * "the send is expensive" from "the loop is slow"
			 * from "neither, look outside the box". */
			{
				uint32_t sl = 0, sm = 0;
				box_net_eth_send_stats(&sl, &sm);
				dserv_state_name(&cfg, name, sizeof name, "dbg/send_us");
				dserv_msg_int(f, name, 0, (int32_t) sl);
				box_uplink_send(f, DSERV_MSG_LEN);
				dserv_state_name(&cfg, name, sizeof name, "dbg/send_max_us");
				dserv_msg_int(f, name, 0, (int32_t) sm);
				box_uplink_send(f, DSERV_MSG_LEN);
				dserv_state_name(&cfg, name, sizeof name, "dbg/loop_us");
				dserv_msg_int(f, name, 0, (int32_t) loop_last_us);
				box_uplink_send(f, DSERV_MSG_LEN);
				dserv_state_name(&cfg, name, sizeof name, "dbg/loop_max_us");
				dserv_msg_int(f, name, 0, (int32_t) loop_max_us);
				box_uplink_send(f, DSERV_MSG_LEN);
				{
					uint32_t wu = 0, wm = 0, ru = 0, rm = 0;
					box_net_eth_rx_stats(&wu, &wm, &ru, &rm);
					dserv_state_name(&cfg, name, sizeof name, "dbg/wake_us");
					dserv_msg_int(f, name, 0, (int32_t) wu);
					box_uplink_send(f, DSERV_MSG_LEN);
					dserv_state_name(&cfg, name, sizeof name, "dbg/recv_us");
					dserv_msg_int(f, name, 0, (int32_t) ru);
					box_uplink_send(f, DSERV_MSG_LEN);
					dserv_state_name(&cfg, name, sizeof name, "dbg/disp_us");
					dserv_msg_int(f, name, 0, (int32_t) disp_last_us);
					box_uplink_send(f, DSERV_MSG_LEN);
					dserv_state_name(&cfg, name, sizeof name, "dbg/disp_max_us");
					dserv_msg_int(f, name, 0, (int32_t) disp_max_us);
					box_uplink_send(f, DSERV_MSG_LEN);
				}
				{
					/* The previously-dark segment: the stack's own
					 * residence measurement (see box_net_eth.h).
					 * Interval means; a side with no traffic since
					 * the last tick publishes nothing. */
					box_eth_stack_stats_t ss;
					if (box_net_eth_stack_stats(&ss) == 0) {
						char d[64];
						int off;
						if (ss.rx_count) {
							dserv_state_name(&cfg, name, sizeof name, "dbg/rxstack_us");
							dserv_msg_int(f, name, 0, (int32_t) ss.rx_avg_us);
							box_uplink_send(f, DSERV_MSG_LEN);
							dserv_state_name(&cfg, name, sizeof name, "dbg/rxstack_n");
							dserv_msg_int(f, name, 0, (int32_t) ss.rx_count);
							box_uplink_send(f, DSERV_MSG_LEN);
						}
						if (ss.rx_count && ss.rx_detail_n) {
							off = 0;
							for (int i = 0; i < ss.rx_detail_n; i++) {
								off += snprintf(d + off, sizeof d - off,
										i ? "/%u" : "%u",
										ss.rx_detail_us[i]);
							}
							dserv_state_name(&cfg, name, sizeof name, "dbg/rxstack_detail");
							dserv_msg_string(f, name, 0, d);
							box_uplink_send(f, DSERV_MSG_LEN);
						}
						if (ss.tx_count) {
							dserv_state_name(&cfg, name, sizeof name, "dbg/txstack_us");
							dserv_msg_int(f, name, 0, (int32_t) ss.tx_avg_us);
							box_uplink_send(f, DSERV_MSG_LEN);
							dserv_state_name(&cfg, name, sizeof name, "dbg/txstack_n");
							dserv_msg_int(f, name, 0, (int32_t) ss.tx_count);
							box_uplink_send(f, DSERV_MSG_LEN);
						}
						if (ss.tx_count && ss.tx_detail_n) {
							off = 0;
							for (int i = 0; i < ss.tx_detail_n; i++) {
								off += snprintf(d + off, sizeof d - off,
										i ? "/%u" : "%u",
										ss.tx_detail_us[i]);
							}
							dserv_state_name(&cfg, name, sizeof name, "dbg/txstack_detail");
							dserv_msg_string(f, name, 0, d);
							box_uplink_send(f, DSERV_MSG_LEN);
						}
					}
				}
#if defined(CONFIG_SOC_SERIES_IMXRT10XX)
				{
					/* TEMP: DI-silence hunt. Raw IGPIO interrupt
					 * state + ISR entry count, once a second. */
					uint32_t r[5];
					char g[96];
					box_gpio_dbg_regs(r);
					snprintf(g, sizeof g,
						 "imr=%lx isr=%lx icr1=%lx edge=%lx psr=%lx isrn=%lu",
						 (unsigned long) r[0], (unsigned long) r[1],
						 (unsigned long) r[2], (unsigned long) r[3],
						 (unsigned long) r[4],
						 (unsigned long) box_gpio_di_isr_count());
					dserv_state_name(&cfg, name, sizeof name, "dbg/gpio");
					dserv_msg_string(f, name, 0, g);
					box_uplink_send(f, DSERV_MSG_LEN);
				}
#endif
				dserv_state_name(&cfg, name, sizeof name, "watchdog");
				dserv_msg_int(f, name, 0, watchdog - 1);
			}
#endif
			box_uplink_send(f, DSERV_MSG_LEN);

			/* box status as datapoints -- observable any time over the active
			 * uplink, not just at boot: active transport, and (where present)
			 * the Ethernet link/lease and the PTP hardware clock. */
			dserv_state_name(&cfg, name, sizeof name, "uplink");
			dserv_msg_string(f, name, 0, box_uplink_active_name());
			box_uplink_send(f, DSERV_MSG_LEN);
#if defined(CONFIG_NETWORKING)
			dserv_state_name(&cfg, name, sizeof name, "net/link");
			dserv_msg_int(f, name, 0, box_net_eth_link());
			box_uplink_send(f, DSERV_MSG_LEN);

			uint8_t ip[4];
			if (box_net_eth_get_ip(ip)) {
				char ips[16];
				snprintf(ips, sizeof ips, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
				dserv_state_name(&cfg, name, sizeof name, "net/ip");
				dserv_msg_string(f, name, 0, ips);
				box_uplink_send(f, DSERV_MSG_LEN);
			}
			dserv_state_name(&cfg, name, sizeof name, "ptp/ns");
			dserv_msg_int64(f, name, 0, (int64_t) box_ptp_now_ns());
			box_uplink_send(f, DSERV_MSG_LEN);

#if defined(CONFIG_PTP_CLOCK)
			/* Re-anchor from PTP once a second. Costs NO packets -- the
			 * offset the host gave us is constant, so this is two local
			 * clock reads. Contrast the obs anchor, which needs a frame to
			 * arrive and inherits its transport delay.
			 *
			 * Skipped during an obs (a re-anchor steps the offset), which is
			 * also why the 1 Hz cadence is harmless: the drift it corrects is
			 * the box crystal against PTP, and box_clock's rate estimator
			 * covers the gap across a trial. */
			if (ptp_offset_valid && !box_obs_active()) {
				uint32_t win = 0;

				if (ptp_reanchor(&win) == 0) {
					dserv_state_name(&cfg, name, sizeof name,
							 "sync/ptp_window_us");
					dserv_msg_int(f, name, 0, (int32_t) win);
					box_uplink_send(f, DSERV_MSG_LEN);

					dserv_state_name(&cfg, name, sizeof name,
							 "sync/offset_us");
					dserv_msg_int64(f, name, 0, boxclk.offset_us);
					box_uplink_send(f, DSERV_MSG_LEN);
				}
			}
#endif
#endif
			/* status is available on demand via the `show` CLI command and as
			 * these datapoints -- no periodic console spam. */
		}

		/* Work parked while the rig was mid-trial (see box_obs.h): run it now
		 * that we are idle. Nothing here is time-critical BY DEFINITION -- it
		 * was deferred precisely because it can block. */
		{
			uint32_t due = box_obs_take_deferred();

#if defined(BOX_HAVE_PERSIST)
			if (due & BOX_DEFER_SAVE) {
				uint8_t blob[BOX_PERSIST_BLOB_MAX];
				uint32_t bn = box_persist_serialize(&cfg, blob, sizeof blob);

				int src = box_flash_save(blob, bn);
				if (src == 0) {
					box_console_printf("deferred save -> ok (%u bytes)\n", bn);
				} else {
					/* Report the errno. A bare "FAILED" sent the
					 * RW612 hunt toward the FlexSPI driver when the
					 * answer was -EDEADLK (-45): NVS refusing an
					 * unrecognised region. */
					box_console_printf("deferred save -> FAILED (%u bytes, err=%d)\n",
					       bn, src);
				}
			}
#else
			(void) due;
#endif
		}

		/* Cost of the pass we just finished -- measured to the point of
		 * blocking, so the wait itself is excluded. */
		if (loop_t0) {
			uint32_t d = k_cyc_to_us_floor32(k_cycle_get_32() - loop_t0);
			loop_last_us = d;
			if (d > loop_max_us) {
				loop_max_us = d;
			}
		}

		/* Block until an ISR has work for us (CDC RX / DI edge), or the
		 * watchdog tick is due. Replaces a flat k_msleep(1) that added up to
		 * 1 ms to BOTH halves of every round trip. */
		box_event_wait(K_MSEC(1));
		loop_t0 = k_cycle_get_32();
	}
	return 0;
}
