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
#include "box_group.h"
#include "box_clock.h"
#include "box_announce.h"
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

		/* NOTE: GPIO_OP_SCHED_PULSE / SCHED_TIMER (fire at beginobs + N us)
		 * are still unhandled in box_gpio_exec -- obs_begin_us above is the
		 * epoch they will need. */
		return;
	}

	gpio_cmd_t cmd;
	cfg_result_t r = dserv_dispatch(&cfg, &m, &cmd);
	if (r == CFG_GPIO && cmd.op != GPIO_OP_NONE) {
		box_gpio_exec(&cfg, &cmd);            /* immediate DO set/pulse */
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
	box_gpio_apply_config(&cfg);

	box_uplink_init(&cfg);       /* USB (and Ethernet where present) up */
	box_console_init();          /* two-way CLI on the USB console CDC */
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
	/* Mount the settings store (NVS on RW612, SD-card FAT on Teensy 4.1). */
	if (box_flash_init() == 0) {
		uint8_t lb[BOX_PERSIST_BLOB_MAX];
		int ln = box_flash_load(lb, sizeof lb);
		cfg_loaded = (ln > 0 && box_persist_deserialize(lb, (uint32_t) ln, &cfg) == 0);
		if (cfg_loaded) {
			box_gpio_apply_config(&cfg);   /* apply the loaded pin map/name */
			groups_resync();               /* seed chords from real pin state */
		}
	}
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
	box_console_printf("PTP hw clock: ready=%d  now=%llu ns\n",
	       (int) box_ptp_ready(), (unsigned long long) box_ptp_now_ns());
#else
	box_console_printf("no Ethernet on this board -- USB-only uplink\n");
#endif
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
			dserv_framer_feed(&rx_framer, rx, (uint32_t) n, on_usb_frame, NULL);
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
#endif
			/* status is available on demand via the `show` CLI command and as
			 * these datapoints -- no periodic console spam. */
		}

		/* Block until an ISR has work for us (CDC RX / DI edge), or the
		 * watchdog tick is due. Replaces a flat k_msleep(1) that added up to
		 * 1 ms to BOTH halves of every round trip. */
		box_event_wait(K_MSEC(1));
	}
	return 0;
}
