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

/* Demo pins, per board. box pin n -> <box-gpio-port>.n (see box_gpio.h), so
 * these land on real hardware on each target. */
#if defined(CONFIG_BOARD_FRDM_RW612)
#define LED_PIN  12   /* hsgpio0.12 = user LED (active low physically) */
#define BTN_PIN  11   /* hsgpio0.11 = User SW2 (active low, pull-up)    */
#else                 /* Teensy 4.x: board LED is gpio2.3 */
#define LED_PIN  3    /* gpio2.3 = on-board LED                         */
#define BTN_PIN  4    /* gpio2.4 = a free pin for a test button         */
#endif

/* One inbound 128-byte frame (config/cmd/ess-in_obs) from the host module:
 * dispatch it into the config, and run any GPIO command it produced. */
static void on_usb_frame(const uint8_t *frame, void *ud)
{
	ARG_UNUSED(ud);
	dserv_msg_t m;
	if (dserv_msg_parse(frame, &m) != 0) {
		return;
	}
	gpio_cmd_t cmd;
	cfg_result_t r = dserv_dispatch(&cfg, &m, &cmd);
	if (r == CFG_GPIO && cmd.op != GPIO_OP_NONE) {
		box_gpio_exec(&cfg, &cmd);            /* immediate DO set/pulse */
	} else if (r == CFG_PIN_MODE) {
		box_gpio_apply_config(&cfg);          /* re-apply on a pin mode change */
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
		} else if (n > 0) {
			dserv_framer_feed(&rx_framer, rx, (uint32_t) n, on_usb_frame, NULL);
		}

		box_di_event_t ev;
		while (box_gpio_poll_di(&cfg, &ev)) {
			uint8_t f[DSERV_MSG_LEN];
			char leaf[24];
			snprintf(leaf, sizeof leaf, "di/%u", ev.pin);
			dserv_state_name(&cfg, name, sizeof name, leaf);   /* extio/<name>/state/di/<pin> */
			dserv_msg_int(f, name, ev.t_us, ev.level);
			box_uplink_send(f, DSERV_MSG_LEN);
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
