/*
 * box_console.c -- interrupt-driven, non-blocking CLI console (see box_console.h).
 */
#include "box_console.h"
#include "box_cli.h"
#include "box_gpio.h"
#if defined(CONFIG_PTP_CLOCK)
#include "box_ptp.h"
#endif
#include "box_obs.h"
#include "box_event.h"
#include "box_uplink.h"
#if defined(BOX_BLE_CENTRAL)
#include "box_ble.h"
#endif
#if defined(CONFIG_BOX_BLE_PERIPHERAL)
#include "box_ble_periph.h"
#include "dserv_ble.h"        /* DSERV_BLE_MTU_MIN: the whole-frame floor */
#endif
#include "box_status_led.h"
#if defined(CONFIG_NETWORKING)
#include "box_net_eth.h"
#endif
#if defined(BOX_HAVE_ADC)
#include "box_ain.h"
#endif
#if defined(BOX_HAVE_ADC) && defined(CONFIG_DAC)
#include "box_adc.h"
#include <zephyr/drivers/dac.h>
#endif

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/reboot.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#if defined(BOX_HAVE_PERSIST)
#include "box_flash.h"
#include "box_persist.h"
#endif

/* Defined in box_announce.c (platform-specific); the CLI_OBS path re-announces
 * the obs role, same as main.c's CFG_OBS_MODE. Forward-declared rather than
 * pulling box_announce.h into the console TU. */
void box_announce_obs_role(const box_config_t *cfg);

static const struct device *con;

RING_BUF_DECLARE(con_rx, 256);
RING_BUF_DECLARE(con_tx, 2048);

/* CDC console ISR: RX -> con_rx, drain con_tx -> TX FIFO. */
static void con_isr(const struct device *dev, void *user)
{
	ARG_UNUSED(user);
	while (uart_irq_update(dev), uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t tmp[64];
			int n = uart_fifo_read(dev, tmp, sizeof tmp);
			if (n > 0) {
				(void) ring_buf_put(&con_rx, tmp, (uint32_t) n);
				box_event_signal();   /* wake the loop to service the CLI */
			}
		}
		if (uart_irq_tx_ready(dev)) {
			uint8_t *p;
			uint32_t sz = ring_buf_get_claim(&con_tx, &p, 64);
			if (sz == 0) {
				uart_irq_tx_disable(dev);
			} else {
				int w = uart_fifo_fill(dev, p, sz);
				ring_buf_get_finish(&con_tx, w > 0 ? (uint32_t) w : 0);
			}
		}
	}
}

void box_console_write(const char *s)
{
	if (!con) {
		return;
	}
	/* Translate a bare '\n' to '\r\n' so a raw terminal returns to column 0
	 * (box_cli already emits '\r\n'; the '\r' guard avoids doubling those). */
	char prev = 0;
	for (const char *p = s; *p; p++) {
		if (*p == '\n' && prev != '\r') {
			(void) ring_buf_put(&con_tx, (const uint8_t *) "\r\n", 2);
		} else {
			(void) ring_buf_put(&con_tx, (const uint8_t *) p, 1);
		}
		prev = *p;
	}
	uart_irq_tx_enable(con);
}

void box_console_printf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	box_console_write(buf);
}

/* Which device the console actually BOUND (vs what the config asked for):
 * 0 = dead, 1 = as configured, 2 = fell back to the other device. Announced
 * as state/console_bound -- before v34 a failed bind meant the console died
 * SILENTLY everywhere (no output, no error, no observable), which is a
 * miserable thing to debug over a network. */
int box_console_bound;

int box_console_init(const box_config_t *cfg)
{
	/* Bind per config (v22 console_mode). Claiming the console CDC costs ~0.21 ms
	 * of median round-trip latency; the same console on the board's console UART
	 * is essentially free (see PORTING.md). Binding the UART leaves the console
	 * CDC enumerated but UNCLAIMED, which measures free. */
	const struct device *want, *other;

	if (dserv_cfg_console_mode(cfg) == CONSOLE_MODE_UART) {
		want  = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
		other = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_console));
	} else {
		want  = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_console));
		other = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	}
	con = want;
	box_console_bound = 1;
	if (!device_is_ready(con)) {
		/* v34: any console beats none -- fall back to the other device
		 * and say so there, instead of dying silently on both. */
		con = other;
		box_console_bound = 2;
	}
	if (!device_is_ready(con)) {
		con = NULL;
		box_console_bound = 0;
		return -1;
	}
	uart_irq_callback_user_data_set(con, con_isr, NULL);
	uart_irq_rx_enable(con);
	if (box_console_bound == 2) {
		box_console_write("console: configured device not ready -- "
				  "fell back to the other one\r\n");
	}
	return 0;
}

#if defined(BOX_HAVE_ADC) && defined(CONFIG_DAC)
/* ---- `adccal`: an on-board DAC -> ADC calibration sweep ----
 *
 * WHY THIS EXISTS. Two questions about an on-chip analog front end cannot be
 * answered by reading source:
 *   1. Which physical pad is box ain channel N? The devicetree says, but a
 *      devicetree that is WRONG says so just as confidently -- and a mis-mapped
 *      channel produces perfectly plausible numbers from the wrong pin.
 *   2. What is the ADC's actual full scale? The reference comes from CFG[REFSEL],
 *      and the FSL headers only name the options Alt1/Alt2/Alt3. Whether the
 *      board's `voltage-ref = <1>` means 1.8 V, VDDA, or something else is a
 *      reference-manual question -- or one measurement.
 *
 * DAC0_OUT is on P4_2 (J1-4) and the board already pinmuxes it, so ONE jumper
 * from there to an analog input turns both questions into an experiment that
 * needs no bench supply and no external parts. Drive a known code, read it back.
 *
 * Deliberately NOT added to box_cli.h: that grammar is shared verbatim with the
 * deployed RP2350 boxes, which have no DAC, and PORTING.md already records "CLI
 * overstates the box" as a live complaint. Same reasoning as the `show` and
 * `ble enable` additions below -- platform truth belongs in the platform layer.
 *
 * Note this BLOCKS the service loop for roughly (steps x settle) ms. That is
 * accepted for a command a human types on purpose; it is not something to call
 * from anywhere else.
 */
#define ADCCAL_STEPS   8
#define ADCCAL_SETTLE  5   /* ms; a DAC settles far faster, this is slack */

static void adc_cal(uint8_t ch)
{
	const struct device *dac = DEVICE_DT_GET(DT_NODELABEL(dac0));
	struct dac_channel_cfg dcfg = {
		.channel_id = 0,
		.resolution = 12,
		.buffered   = true,
	};
	uint16_t v[BOX_ADC_MAX_CH];
	char ln[112];
	uint16_t vref = box_adc_vref_mv();
	uint32_t full = (1u << box_adc_bits()) - 1u;

	if (!box_adc_ready()) {
		box_console_write("ERR adc not ready\r\n");
		return;
	}
	if (ch >= box_adc_channels()) {
		box_console_write("ERR no such ain channel\r\n");
		return;
	}
	if (!device_is_ready(dac) || dac_channel_setup(dac, &dcfg) != 0) {
		box_console_write("ERR dac0 not ready\r\n");
		return;
	}

	/* TAKE THE CONVERTER before touching it. Two reasons, and the second is the
	 * one that bites: the sampler may have SUSPENDED the device (analog
	 * disabled, or held aside for an OTA), in which case every sweep below
	 * would return -EAGAIN and this command -- the one you reach for precisely
	 * when analog is otherwise idle -- would print a column of errors. And the
	 * ain thread otherwise owns the device outright, so sweeping underneath it
	 * would interleave our ramp with its cadence.
	 *
	 * The borrow outlives the sweep on purpose: (steps+1) * settle plus the
	 * console writes, rounded well up, so a wedge here costs analog a bounded
	 * outage rather than a permanent one. */
	if (box_ain_borrow((ADCCAL_STEPS + 1) * ADCCAL_SETTLE + 2000) != 0) {
		box_console_write("ERR sampler would not yield the converter\r\n");
		return;
	}

	snprintf(ln, sizeof ln,
		 "dac0 (P4_2 / J1-4) -> ain ch%u; adc %u-bit, vref %u mV\r\n",
		 ch, box_adc_bits(), vref);
	box_console_write(ln);
	box_console_write("  dac_code   adc_code   adc_mV\r\n");

	for (int i = 0; i <= ADCCAL_STEPS; i++) {
		uint16_t code = (uint16_t) ((4095 * i) / ADCCAL_STEPS);
		int rc;

		if (dac_write_value(dac, 0, code) != 0) {
			box_console_write("ERR dac write\r\n");
			break;
		}
		k_msleep(ADCCAL_SETTLE);

		rc = box_adc_sweep(BIT(ch), v, (uint8_t) ARRAY_SIZE(v), NULL);
		if (rc < 1) {
			snprintf(ln, sizeof ln, "  %8u   sweep err %d\r\n", code, rc);
		} else {
			uint32_t mv = ((uint32_t) v[0] * vref) / full;
			snprintf(ln, sizeof ln, "  %8u   %8u   %6u\r\n", code, v[0], mv);
		}
		box_console_write(ln);
	}

	/* Leave the pin low rather than parked at full scale -- a DAC left driving
	 * is a surprise for whatever gets jumpered next. */
	(void) dac_write_value(dac, 0, 0);

	/* Hand it back. The sampler decides whether that means resuming or leaving
	 * it suspended -- we do not know, and guessing is how the two views of the
	 * device drift apart. */
	box_ain_return();
}
#endif /* BOX_HAVE_ADC && CONFIG_DAC */

/* Append-into-a-buffer sink for box_cli_dump_to (see box_console.h). */
static char *dump_buf;
static int   dump_cap, dump_len, dump_cut;

static void dump_sink(void *ud, const char *line)
{
	ARG_UNUSED(ud);
	int n = (int) strlen(line);

	if (dump_len + n + 2 > dump_cap) {   /* +1 newline, +1 NUL */
		dump_cut = 1;
		return;
	}
	memcpy(dump_buf + dump_len, line, (size_t) n);
	dump_len += n;
	dump_buf[dump_len++] = '\n';
	dump_buf[dump_len] = '\0';
}

int box_console_config_dump_form(const box_config_t *cfg, char *buf, int cap, int form)
{
	if (!buf || cap < 2) {
		return 0;
	}
	dump_buf = buf; dump_cap = cap; dump_len = 0; dump_cut = 0;
	buf[0] = '\0';
	box_cli_dump_to_form(cfg, dump_sink, NULL, form);
	return dump_cut ? -dump_len : dump_len;
}

int box_console_config_dump(const box_config_t *cfg, char *buf, int cap)
{
	return box_console_config_dump_form(cfg, buf, cap, BOX_DUMP_CLI);
}

void box_console_set_ain_channels(int n)
{
	box_cli_set_ain_channels(n);
}

void box_console_set_reserved_pins(uint32_t mask)
{
	box_cli_set_reserved_pins(mask);
}

/* ---- line assembly + dispatch to box_cli ---- */
static char line_buf[128];
static int  line_len;

/* `dump` to the CDC the operator is typing at.
 *
 * box_cli_dump's body uses printf, which goes to zephyr,console -- and on the
 * Teensy that is lpuart6 (pins 0/1, an external USB-serial adapter), NOT this
 * USB CDC. So `dump` printed a perfect config into a device nobody was
 * watching and looked like it did nothing. Intercept it here and re-emit
 * through box_console_write, which is where the CLI's own output goes. The
 * sink strips the CRLF the format carries, so put it back. */
static void dump_console_sink(void *ud, const char *line)
{
	ARG_UNUSED(ud);
	box_console_write(line);
	box_console_write("\r\n");
}

static void run_line(box_config_t *cfg, const char *line)
{
	char resp[1024];   /* `help`/`show` output is large */
	gpio_cmd_t cmd = { .op = GPIO_OP_NONE };
	cli_action_t a;

	if (strcmp(line, "dump") == 0) {
		box_cli_dump_to(cfg, dump_console_sink, NULL);
		return;
	}

	/* `verbose 0|1` -- routine registration chatter. Platform-local for the
	 * same reason as adccal: it names a behaviour of this port's uplink, not a
	 * concept the shared grammar (and the RP2350 boxes that fork it) has. */
	if (strncmp(line, "verbose", 7) == 0) {
		if (line[7] == ' ') {
			box_uplink_set_verbose(atoi(line + 8));
		}
		box_console_printf("OK verbose=%d (routine reg messages; full/failed always print)\n",
				   box_uplink_verbose());
		return;
	}

	/* `now` -- what time does the box think it is, and does that make SENSE?
	 *
	 * Platform-local for the same reason as verbose/adccal: it reaches into the
	 * 1588 clock and the app's box_clock, neither of which the shared grammar (or
	 * the RP2350 boxes that fork it) knows about.
	 *
	 * THE THIRD LINE IS THE POINT. On 2026-07-29 this box ran for hours with its
	 * 1588 counter free-running from boot -- 56 years wrong -- while every status
	 * field read healthy and rig_check.sh scored 11/11, because every other PTP
	 * field is a proxy (sync/source=ptp means a paired READ succeeded; anchored
	 * means ptpconf PUSHED an offset). The raw counter is the one value that
	 * cannot lie, and the epoch-vs-uptime distinction is visible at a glance:
	 * unsynced reads single-digit seconds, synced reads ~1.78e9.
	 *
	 * All three lines are printed as integer seconds + fractional digits rather
	 * than floats, so this needs no CBPRINTF_FP_SUPPORT. */
	if (strcmp(line, "now") == 0) {
		uint64_t b = box_gpio_now_us();
		uint64_t d = box_app_dserv_now_us();

		box_console_printf("box    %llu.%06llu s  (local monotonic, since boot)\n",
				   (unsigned long long) (b / 1000000ULL),
				   (unsigned long long) (b % 1000000ULL));
		if (d) {
			box_console_printf("dserv  %llu.%06llu s  (what event timestamps use)\n",
					   (unsigned long long) (d / 1000000ULL),
					   (unsigned long long) (d % 1000000ULL));
		} else {
			box_console_printf("dserv  UNALIGNED -- no anchor yet; events publish"
					   " arrival-stamped\n");
		}
#if defined(CONFIG_PTP_CLOCK)
		if (box_ptp_ready()) {
			uint64_t p = box_ptp_now_ns();
			uint64_t ps = p / 1000000000ULL;

			/* 1e9 s is ~2001, so nothing below it can be a wall clock. */
			box_console_printf("ptp    %llu.%09llu s  %s\n",
					   (unsigned long long) ps,
					   (unsigned long long) (p % 1000000000ULL),
					   ps > 1000000000ULL
					   ? "EPOCH -- 1588 clock is SET (looks synced)"
					   : "UPTIME -- 1588 clock is FREE-RUNNING, NOT synced");
		} else {
			box_console_printf("ptp    unavailable (clock device not ready)\n");
		}
#else
		box_console_printf("ptp    not built (no CONFIG_PTP_CLOCK on this board)\n");
#endif
		return;
	}

#if defined(BOX_BLE_CENTRAL)
	/* `ble` (bare) -- what the radio is ACTUALLY doing.
	 *
	 * Platform-local for the same reason as `now` and `verbose`: it reaches into
	 * box_ble, which the shared grammar (and the RP2350 boxes that fork it) does
	 * not have. The core CLI only offers `ble enable 0|1`, i.e. the setting --
	 * and the setting was the only thing observable here until 2026-08-31, on a
	 * central whose scan/connect path had never been run against a peer.
	 *
	 * THREE distinct states, because collapsing them is what makes an unproven
	 * central undebuggable: radio down; radio up but not scanning (fleet full,
	 * or the controller refused -- box_ble_scan_err tells which); radio up and
	 * scanning. "Nothing connected" means something different in each. */
	if (strcmp(line, "ble") == 0) {
		if (!box_ble_active()) {
			box_console_printf("ble: radio DOWN (`ble enable 1` -- live)\n");
			return;
		}
		{
			uint32_t seen = 0, matched = 0;

			box_ble_scan_counts(&seen, &matched);
			uint32_t tr = 0, ce = 0, ee = 0, dr = 0;
			int le = 0;

			box_ble_conn_counts(&tr, &ce, &ee, &dr, &le);
			box_console_printf("ble: radio up, conns %d/%d, adv seen=%u matched=%u\n"
					   "  conn try=%u create_err=%u est_err=%u dropped=%u last=%d\n",
					   box_ble_conn_count(), CONFIG_BT_MAX_CONN,
					   seen, matched, tr, ce, ee, dr, le);
		}
		if (box_ble_scanning()) {
			box_console_printf("  scanning for d5e7000x peripherals\n");
		} else if (box_ble_scan_err()) {
			box_console_printf("  NOT scanning -- scan start failed (%d);"
					   " this central is DEAF\n", box_ble_scan_err());
		} else if (box_ble_conn_count() >= CONFIG_BT_MAX_CONN) {
			box_console_printf("  not scanning -- fleet full (normal)\n");
		} else {
			box_console_printf("  not scanning -- a connect is in flight (normal)\n");
		}
		/* Per-peer echo-sync. `synced=0` is the line that matters: that peer's
		 * events are reaching dserv ARRIVAL-stamped, not source-stamped, so
		 * they carry radio latency (~10-50 ms) instead of the tier's ~1 ms.
		 * min_rtt bounds how good the mapping can get -- the midpoint
		 * assumption is only as good as the fastest round trip seen. */
		for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
			box_ble_peer_info_t pi;

			if (!box_ble_peer_info(i, &pi)) {
				continue;
			}
			/* Interval printed in ms from 1.25 ms units, as integer
			 * quarters (ci*125/100) -- no float, so this needs no
			 * CBPRINTF_FP_SUPPORT, same rule as `now`. */
			box_console_printf("  %-16s conn_int=%u.%02u ms lat=%u/%u"
					   " echo tx=%u rx=%u min_rtt=%u us  %s\n",
					   pi.name,
					   (pi.conn_int * 125u) / 100u,
					   (pi.conn_int * 125u) % 100u,
					   pi.lat_applied, cfg->ble_latency,
					   pi.echo_tx, pi.echo_rx, pi.min_rtt_us,
					   pi.synced ? "SYNCED (source-stamped)"
						     : "not synced -- events arrival-stamped");
		}
		return;
	}
#endif

#if defined(CONFIG_BOX_BLE_PERIPHERAL)
	/* `ble` on the OTHER end of the same pipe. The central has had this since
	 * the day its scan path was first run against a peer; the peripheral had
	 * only a boot banner, so every question after boot ("did it ever connect?",
	 * "is the receiver still syncing me?") needed a reflash to answer.
	 *
	 * Deliberately the same three-way split as the central's, because the
	 * states that matter here are the ones the RECEIVER cannot see: from its
	 * side, a peripheral that never advertised, one out of range, and one with
	 * a flat battery are one indistinguishable silence. */
	if (strcmp(line, "ble") == 0) {
		uint16_t mtu = 0;
		uint32_t erx = 0, tok = 0, tdrop = 0;

		box_ble_periph_stats(&mtu, &erx, &tok, &tdrop);
		if (!box_ble_periph_active()) {
			box_console_printf("ble: radio DOWN -- this box is mute\n");
			return;
		}
		box_console_printf("ble: peripheral up as extio-%s\n"
				   "  tx ok=%u dropped=%u (dropped = nobody subscribed)\n",
				   dserv_cfg_name(cfg), tok, tdrop);
		if (box_ble_periph_ready()) {
			/* echo_rx is the load-bearing number, not the link: it is
			 * the receiver running the estimator that makes this box's
			 * source stamps mean anything. Frozen here = timestamps
			 * decaying while everything still looks connected. */
			box_console_printf("  LINKED, mtu %u, echo answered=%u"
					   " (must keep rising, ~3/s)\n", mtu, erx);
		} else if (mtu) {
			box_console_printf("  connected, mtu %u, but NOT subscribed%s"
					   " -- nothing this box publishes goes anywhere\n",
					   mtu,
					   mtu < DSERV_BLE_MTU_MIN ? " and the MTU is too small" : "");
		} else if (box_ble_periph_advertising()) {
			box_console_printf("  advertising, no receiver connected\n");
		} else {
			box_console_printf("  NOT advertising (err %d) -- INVISIBLE;"
					   " indistinguishable from switched off\n",
					   box_ble_periph_adv_err());
		}
		if (box_status_led_claimed()) {
			box_console_printf("  led: %s%s\n",
					   box_status_led_state_name(box_status_led_state()),
					   box_status_led_overridden() ? "  [OVERRIDDEN -- `led auto`]" : "");
		}
		return;
	}
#endif

	/* `led auto|off|<rgb>` -- bench override for the status LED.
	 *
	 * Answers the one question the LED itself cannot: whether a colour that
	 * never appears means the state machine never selects it, or that that
	 * leg of the LED has never lit on this board. Only ever runtime -- a box
	 * that boots into a forced colour has a status light that lies. */
	if (strcmp(line, "led") == 0 || strncmp(line, "led ", 4) == 0) {
		const char *a = (line[3] == ' ') ? line + 4 : "";

		if (!box_status_led_claimed()) {
			box_console_printf("ERR no status LED on this build"
					   " (the board LEDs are ordinary pins here)\n");
			return;
		}
		if (*a == '\0') {
			box_console_printf("led: %s%s\n",
					   box_status_led_state_name(box_status_led_state()),
					   box_status_led_overridden() ? "  [OVERRIDDEN]" : "");
			return;
		}
		if (strcmp(a, "auto") == 0) {
			box_status_led_override(1, 0);
			box_console_printf("OK led auto\n");
			return;
		}
		if (strcmp(a, "off") == 0) {
			box_status_led_override(0, 0);
			box_console_printf("OK led off (forced)\n");
			return;
		}
		{
			uint8_t m = 0;

			for (const char *p = a; *p; p++) {
				if (*p == 'r' || *p == 'R') m |= 1;
				else if (*p == 'g' || *p == 'G') m |= 2;
				else if (*p == 'b' || *p == 'B') m |= 4;
				else {
					box_console_printf("ERR led auto|off|<rgb letters>"
							   " e.g. `led rg` = amber\n");
					return;
				}
			}
			box_status_led_override(0, m);
			box_console_printf("OK led %s (forced; `led auto` to release)\n", a);
		}
		return;
	}

#if defined(BOX_HAVE_ADC) && defined(CONFIG_DAC)
	/* Handled BEFORE the core CLI, which would answer `ERR unknown` first. */
	if (strncmp(line, "adccal", 6) == 0) {
		unsigned ch = 0;
		if (line[6] == ' ') {
			ch = (unsigned) atoi(line + 7);
		}
		adc_cal((uint8_t) ch);
		return;
	}
#endif

	a = box_cli_exec(cfg, line, resp, sizeof resp, &cmd);

	box_console_write(resp);   /* the OK/ERR line box_cli produced */

#if defined(BOX_BLE_CENTRAL)
	/* `ble enable 1` is documented as live, so honour it here rather than
	 * making the user reboot. Turning it back OFF needs a reboot: bringing the
	 * controller down cleanly is not something this module supports, and
	 * pretending otherwise would be another field that lies. */
	if (strncmp(line, "ble enable", 10) == 0) {
		if (cfg->ble_en && !box_ble_active()) {
			/* Report the SCAN, not just bt_enable(). This line used to say
			 * "radio up, scanning" whenever box_ble_init() returned 0 --
			 * which tests only bt_enable() -- so a failed scan start was
			 * announced as a working central. `ble` prints the full state. */
			if (box_ble_init() != 0) {
				box_console_write("  ble: radio FAILED to start\r\n");
			} else if (box_ble_scanning()) {
				box_console_write("  ble: radio up, scanning\r\n");
			} else {
				box_console_printf("  ble: radio up but NOT SCANNING"
						   " (scan err %d) -- deaf; see `ble`\r\n",
						   box_ble_scan_err());
			}
		} else if (!cfg->ble_en && box_ble_active()) {
			box_console_write("  ble: still running -- reboot to stop\r\n");
		}
	}
#endif

#if defined(CONFIG_NETWORKING)
	/* `show` prints the CONFIGURED net fields, so in DHCP mode it reads
	 * net.ip=0.0.0.0 no matter what lease the box actually holds. That is
	 * actively misleading rather than merely incomplete -- on 2026-07-27 a box
	 * that HAD a lease read as unconfigured, and the live address was only
	 * available from the ONE-SHOT boot banner, so a lease acquired after boot
	 * (the normal case, DHCP is slower than init) appeared nowhere at all.
	 *
	 * Appended here and not in box_cli.h on purpose: the core CLI is
	 * platform-agnostic and shared with boards that have no Ethernet, so it
	 * must not reach into box_net_eth.h. */
	if (strncmp(line, "show", 4) == 0) {
		uint8_t ip[4];
		char nl[80];

		if (box_net_eth_get_ip(ip)) {
			snprintf(nl, sizeof nl, "  net.live=%u.%u.%u.%u link=%d\r\n",
				 ip[0], ip[1], ip[2], ip[3], box_net_eth_link());
		} else {
			snprintf(nl, sizeof nl, "  net.live=none link=%d\r\n",
				 box_net_eth_link());
		}
		box_console_write(nl);
	}
#endif

	/* ...and the same question for dserv: the main line shows the CONFIGURED
	 * target, which says nothing about whether the box is actually talking to
	 * it. `session` catches a dead/never-established TCP link; `cmds_rx` is the
	 * one that catches "publishing but deaf", where the session is fine and the
	 * %match registration is not. A frozen cmds_rx next to a live box IS the
	 * diagnosis -- see main.c. */
	if (strncmp(line, "show", 4) == 0) {
		const char *up   = box_uplink_active_name();
		const char *sess = "n/a";
		char nl[96];

#if defined(CONFIG_NETWORKING)
		if (strcmp(up, "eth") == 0) {
			sess = box_net_eth_connected() ? "connected" : "down";
		}
#endif
		snprintf(nl, sizeof nl, "  dserv.live=%s uplink=%s cmds_rx=%u\r\n",
			 sess, up, (unsigned) box_cmds_rx());
		box_console_write(nl);
	}

	switch (a) {
	case CLI_GPIO:
		box_gpio_exec(cfg, &cmd);
		break;
	case CLI_OBS:
		/* obs mode changed: re-announce the role so a host's leader scan
		 * sees it live, exactly as main.c does for CFG_OBS_MODE. No pin
		 * re-apply -- the mode is a role, not a mux change. */
		box_announce_obs_role(cfg);
		break;
	case CLI_PIN:
	case CLI_GROUP:
	case CLI_AIN:
		box_gpio_apply_config(cfg);   /* pin/group/analog change -> re-apply */
#if defined(BOX_HAVE_ADC)
		/* ...and tell the SAMPLER too. The datapoint path (main.c, CFG_AIN)
		 * has always done this; the console path never did, so every `ain
		 * group ...` typed at the console needed a reboot to take effect --
		 * and with runtime `pin N mode ain` it would be worse, re-muxing the
		 * pad while the sweep mask stayed stale. Same rule as main.c states
		 * it: config that applies only after a reboot is a config field that
		 * lies. */
		box_ain_apply();
#endif
		break;
	case CLI_SAVE:
#if defined(BOX_HAVE_PERSIST)
		if (box_obs_active()) {      /* never program flash mid-trial */
			box_obs_defer(BOX_DEFER_SAVE);
			box_console_write("deferred to end of obs\r\n");
			break;
		}
	{
		uint8_t blob[BOX_PERSIST_BLOB_MAX];
		uint32_t n = box_persist_serialize(cfg, blob, sizeof blob);
		{
			int rc = box_flash_save(blob, n);
			if (rc == 0) {
				box_console_printf("saved (%u bytes)\r\n", n);
			} else {
				/* Print the errno. "save FAILED" alone cost hours on the
				 * RW612 bring-up: the answer was in the return value. */
				box_console_printf("save FAILED (%u bytes, err=%d)\r\n", n, rc);
			}
		}
	}
#else
		box_console_write("no persistence on this board\r\n");
#endif
		break;
	case CLI_FACTORY:
		/* ERASE THE STORE, not just the copy in RAM.
		 *
		 * box_cli.h has documented this as "caller erases storage + resets
		 * cfg" since it was written, and the caller only ever did the second
		 * half -- so `factory` looked like it worked and the next reboot
		 * reloaded everything it claimed to have cleared. The obvious
		 * workaround, `factory` then `save`, is worse than useless here: it
		 * writes a ZEROED blob, which still loads, and console_mode 0 is
		 * CONSOLE_MODE_CDC -- on a board consoled through the MCU-Link UART
		 * that hands you a box you cannot talk to. */
		memset(cfg, 0, sizeof *cfg);
		/* Put back what main() sets before the store is mounted, so the RAM
		 * state now equals a fresh boot rather than "all zeros". Without this
		 * the console would move the moment the memset lands, before anyone
		 * could reboot. */
		cfg->console_mode = BOX_DEFAULT_CONSOLE_MODE;
		box_gpio_apply_config(cfg);
#if defined(BOX_HAVE_PERSIST)
		{
			int frc = box_flash_clear();

			box_console_printf("factory reset -- store %s\r\n",
					   frc == 0 ? "erased" : "ERASE FAILED");
			if (frc != 0) {
				box_console_printf("  err=%d -- saved config still on flash\r\n",
						   frc);
			}
		}
		box_console_write("reboot for a true out-of-box state\r\n");
#else
		box_console_write("factory reset (no persistent store on this board)\r\n");
#endif
		break;
	case CLI_REBOOT:
		box_console_write("rebooting\r\n");
		k_msleep(50);
		sys_reboot(SYS_REBOOT_WARM);
		break;
	case CLI_BOOTSEL:
		box_console_write("bootsel unsupported here; press the Program button\r\n");
		break;
	default:
		break;
	}
}

/* Batch echo per chunk: fast typing must not turn into a burst of 1-byte USB
 * packets (that stresses the host CDC driver -- it dropped a macOS `screen`
 * session mid-type). We build up the echo for a whole RX chunk and write it in
 * as few calls as possible, flushing before a command's own output. */
static char ech[192];
static int  ech_n;
static void echo_flush(void)
{
	if (ech_n > 0) {
		ech[ech_n] = '\0';
		box_console_write(ech);
		ech_n = 0;
	}
}
static void echo_puts(const char *s)
{
	while (*s && ech_n < (int) sizeof(ech) - 1) {
		ech[ech_n++] = *s++;
	}
	if (*s) {                     /* buffer full -> flush and continue */
		echo_flush();
		box_console_write(s);
	}
}

void box_console_service(box_config_t *cfg)
{
	uint8_t chunk[64];
	uint32_t got;
	while ((got = ring_buf_get(&con_rx, chunk, sizeof chunk)) > 0) {
		for (uint32_t i = 0; i < got; i++) {
			uint8_t c = chunk[i];
			if (c == '\r' || c == '\n') {
				echo_puts("\r\n");
				echo_flush();                 /* flush echo before the reply */
				if (line_len > 0) {
					line_buf[line_len] = '\0';
					run_line(cfg, line_buf);
				}
				line_len = 0;
				box_console_write("> ");
			} else if (c == 0x08 || c == 0x7f) {  /* backspace / DEL */
				if (line_len > 0) {
					line_len--;
					echo_puts("\b \b");
				}
			} else if (line_len < (int) sizeof(line_buf) - 1) {
				line_buf[line_len++] = (char) c;
				char e[2] = { (char) c, '\0' };
				echo_puts(e);
			}
		}
		echo_flush();   /* one write for the whole chunk's remaining echo */
	}
}

