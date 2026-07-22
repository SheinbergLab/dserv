/*
 * extio-rw612 -- Zephyr application entry.
 *
 * Building block #2: prove the portable core (forked verbatim from wiznet-io,
 * pico_* renamed to box_*) compiles and RUNS under the Zephyr toolchain, first
 * on native_sim, then on the FRDM-RW612 board. This is the on-target twin of
 * tools/box_sim.c --selftest: build datapoint frames, dispatch them into a
 * box_config_t, exercise a gpio command, and round-trip persist -- all with
 * zero platform dependencies beyond printk.
 *
 * The transport/GPIO/flash platform layer (src/platform/) is deliberately NOT
 * wired yet; that is building block #3+ (USB-HS, Ethernet+PTP, BLE, GAU ADC).
 */
#include <zephyr/kernel.h>
#include <zephyr/version.h>
#include <stdio.h>

#include "dserv_config.h"
#include "box_persist.h"
#include "box_gpio.h"
#include "box_net_usb.h"
#include "box_net_eth.h"
#include "box_ptp.h"
#include "box_uplink.h"
#include "box_ble.h"

static box_config_t   cfg;
static dserv_framer_t rx_framer;

/* FRDM-RW612: box pin n -> hsgpio0.n, so these land on real board hardware. */
#define LED_PIN  12   /* hsgpio0.12 = user LED (active low physically) */
#define BTN_PIN  11   /* hsgpio0.11 = User SW2 (active low, pull-up)    */

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

	printk("\n=== extio-rw612 core smoke test (Zephyr %s) ===\n", KERNEL_VERSION_STRING);

	/* config datapoint: set pin 5 to output */
	dserv_msg_int(f, "extio/pico/config/pin/5/mode", 0, 1);
	r = feed(f, &cmd);
	printk("apply config/pin/5/mode=out    -> %-9s pin_mode[5]=%u\n",
	       dserv_cfg_result_str(r), cfg.pin_mode[5]);

	/* config datapoint: string-typed dserv IP */
	dserv_msg_string(f, "extio/pico/config/dserv/ip", 0, "192.168.11.1");
	r = feed(f, &cmd);
	printk("apply config/dserv/ip          -> %-9s %u.%u.%u.%u\n",
	       dserv_cfg_result_str(r), cfg.dserv_ip[0], cfg.dserv_ip[1],
	       cfg.dserv_ip[2], cfg.dserv_ip[3]);

	/* transient command: box-timed pulse on pin 6 -> a gpio_cmd for the platform */
	dserv_msg_int(f, "extio/pico/cmd/do/6/pulse_us", 0, 500);
	r = feed(f, &cmd);
	printk("apply cmd/do/6/pulse_us=500    -> %-9s op=%d pin=%u value=%u\n",
	       dserv_cfg_result_str(r), cmd.op, cmd.pin, cmd.value);

	/* persistence round-trip (the flash write itself is platform; the codec is core) */
	uint8_t blob[BOX_PERSIST_BLOB_MAX];
	uint32_t n = box_persist_serialize(&cfg, blob, sizeof blob);
	box_config_t restored = {0};
	int ok = box_persist_deserialize(blob, n, &restored);
	printk("persist round-trip             -> %-9s %u bytes, applied_count=%u\n",
	       ok == 0 ? "ok" : "FAIL", n, restored.applied_count);

	/* the box's datapoint identity */
	char pfx[64];
	dserv_cfg_prefix(&cfg, pfx, sizeof pfx);
	printk("datapoint prefix               -> %s  (dserv port %u)\n",
	       pfx, dserv_cfg_port(&cfg));

	printk("=== codec smoke test done ===\n\n");

	/* ---- block #3: real GPIO on the FRDM-RW612 ---- */
	printk("=== box_gpio hardware test ===\n");
	if (box_gpio_init() != 0) {
		printk("box_gpio_init FAILED (gpio/counter device not ready)\n");
		return 0;
	}

	/* Configure LED pin as output, button pin as pulled-up input, via the same
	 * config path the host uses -- then hand the config to the platform layer. */
	cfg.pin_mode[LED_PIN] = 1;   /* output   */
	cfg.pin_mode[BTN_PIN] = 3;   /* in_pullup */
	box_gpio_apply_config(&cfg);
	printk("configured hsgpio0.%d=out (LED), hsgpio0.%d=in_pullup (SW2)\n",
	       LED_PIN, BTN_PIN);

	/* Boot heartbeat: three hardware-timed LED pulses (CTIMER drops each edge). */
	gpio_cmd_t pulse = { .op = GPIO_OP_PULSE, .pin = LED_PIN, .value = 120000 };
	for (int i = 0; i < 3; i++) {
		box_gpio_exec(&cfg, &pulse);
		k_msleep(250);
	}
	printk("=== box_gpio ready ===\n\n");

	/* ---- blocks #4-6: transports behind the uplink arbiter ---- */
	printk("=== box_uplink (USB + Ethernet, arbitrated) ===\n");
	box_uplink_init(&cfg);          /* brings up both transports; picks the active one */
	dserv_framer_reset(&rx_framer);

	/* one-shot status: DHCP lease (if eth came up) + the PTP hardware clock */
	uint8_t ip[4];
	if (box_net_eth_wait_ip(ip, 5000)) {
		printk("eth DHCP IPv4: %u.%u.%u.%u  link=%d\n",
		       ip[0], ip[1], ip[2], ip[3], box_net_eth_link());
	} else {
		printk("eth: no lease (link=%d)\n", box_net_eth_link());
	}
	printk("PTP hw clock: ready=%d  now=%llu ns\n",
	       (int) box_ptp_ready(), (unsigned long long) box_ptp_now_ns());
	printk("active uplink: %s\n", box_uplink_active_name());

	/* ---- block #6 (ingress): multi-peripheral BLE central ---- */
	if (box_ble_init() == 0) {
		printk("BLE central up; scanning for d5e7000x peripherals (max %d)\n\n",
		       CONFIG_BT_MAX_CONN);
	} else {
		printk("BLE init failed (continuing wired-only)\n\n");
	}

	/* ---- converged box service loop (blocks #2-6 together) ----
	 * arbitrate the uplink; inbound frames -> dispatch -> GPIO; local DI, BLE
	 * ingress, and a 1 Hz watchdog -> whichever uplink is active. */
	uint8_t rx[256];
	int watchdog = 0;
	int64_t next_wd = k_uptime_get() + 1000;
	char name[80];

	while (1) {
		box_uplink_service(&cfg);         /* carrier/strap selection + (re)connect */

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

		/* BLE ingress: each peripheral's frame is already source-stamped
		 * (extio/<client>/...); relay it out the active uplink verbatim. */
		uint8_t bframe[DSERV_MSG_LEN];
		while (box_ble_poll(bframe)) {
			box_uplink_send(bframe, DSERV_MSG_LEN);
		}

		if (k_uptime_get() >= next_wd) {
			next_wd += 1000;
			uint8_t f[DSERV_MSG_LEN];
			dserv_state_name(&cfg, name, sizeof name, "watchdog");
			dserv_msg_int(f, name, 0, watchdog++);
			box_uplink_send(f, DSERV_MSG_LEN);
		}

		k_msleep(1);
	}
	return 0;
}
