/*
 * box_console.c -- the box's management console, on Zephyr's Shell subsystem.
 *
 * This replaces a hand-rolled CDC line editor (own ISR, RX/TX rings, echo, and
 * backspace handling). That version dropped macOS terminal sessions under
 * interaction; the Shell is Zephyr's battle-tested console and owns the CDC
 * through the standard shell_uart backend (chosen zephyr,shell-uart), so all of
 * the flushing/pacing/terminal handling is the stack's problem, not ours.
 *
 * Command surface is UNCHANGED: every verb registers as a shell command whose
 * handler rejoins argv into the one line box_cli_exec already parses, so the
 * grammar stays identical to the Pico box (and to what extio-setup drives) --
 * `pin 3 mode out` reaches box_cli_exec exactly as before, just under a prompt.
 *
 * THREADING: the shell runs in its own thread, but box_cli_exec mutates the
 * config the service loop is reading. Rather than lock the whole loop, a command
 * is MARSHALED: the shell thread parks the line, wakes the loop, and blocks; the
 * loop executes it (config + GPIO stay single-threaded, exactly as before) and
 * hands the response back. Only one shell thread exists, so a single slot is
 * enough. Output side: box_console_write goes to the shell backend, and the
 * shell maps '\n' -> "\r\n" itself (SHELL_FLAG_OLF_CRLF), so we do NOT translate.
 */
#include "box_console.h"
#include "box_cli.h"
#include "box_gpio.h"
#include "box_event.h"

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>

#if defined(BOX_HAVE_PERSIST)
#include "box_flash.h"
#include "box_persist.h"
#endif

/* ---------------- output ---------------- */

void box_console_write(const char *s)
{
	const struct shell *sh = shell_backend_uart_get_ptr();

	if (!sh) {
		return;
	}
	shell_fprintf(sh, SHELL_NORMAL, "%s", s);
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

int box_console_init(void)
{
	/* Nothing to claim: the shell autostarts on the chosen zephyr,shell-uart
	 * (the console CDC) before main() runs. */
	return 0;
}

/* ---------------- shell thread <-> service loop handoff ---------------- */

static box_config_t *g_cfg;             /* published by box_console_service */

static struct {
	char line[192];
	char resp[1024];                /* `show`/`help` output is large */
} req;

static atomic_t req_pending = ATOMIC_INIT(0);
static K_SEM_DEFINE(req_done, 0, 1);

/* Append to the pending response (used by the action handlers below). */
static void resp_add(const char *s)
{
	size_t n = strlen(req.resp);

	if (n < sizeof req.resp - 1) {
		snprintf(req.resp + n, sizeof req.resp - n, "%s", s);
	}
}

/* Hand one CLI line to the service loop and wait for its response. Runs on the
 * shell thread. */
static int submit(const struct shell *sh, const char *line)
{
	if (!g_cfg) {
		shell_error(sh, "box service loop not running yet");
		return -ENODEV;
	}

	snprintf(req.line, sizeof req.line, "%s", line);
	req.resp[0] = '\0';

	atomic_set(&req_pending, 1);
	box_event_signal();             /* wake the loop now, don't wait for its tick */

	if (k_sem_take(&req_done, K_MSEC(3000)) != 0) {
		atomic_set(&req_pending, 0);
		shell_error(sh, "timed out waiting for the box service loop");
		return -ETIMEDOUT;
	}

	if (req.resp[0]) {
		shell_fprintf(sh, SHELL_NORMAL, "%s", req.resp);
	}
	return 0;
}

/* Rejoin argv into the single line box_cli_exec parses, then submit it. */
static int cmd_box(const struct shell *sh, size_t argc, char **argv)
{
	char line[sizeof req.line];
	int k = 0;

	for (size_t i = 0; i < argc; i++) {
		int r = snprintf(line + k, sizeof line - k, "%s%s", i ? " " : "", argv[i]);

		if (r < 0 || r >= (int) (sizeof line - k)) {
			shell_error(sh, "command line too long");
			return -E2BIG;
		}
		k += r;
	}
	return submit(sh, line);
}

/* `help` is a shell built-in, so box_cli's own grammar listing is reached via
 * `cmds` (the built-in help still lists every command registered below). */
static int cmd_box_cmds(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return submit(sh, "help");
}

/* ---------------- service-loop side ---------------- */

void box_console_service(box_config_t *cfg)
{
	int do_reboot = 0;

	g_cfg = cfg;                    /* publish for the shell thread */

	if (atomic_get(&req_pending) != 1) {
		return;
	}

	gpio_cmd_t cmd = { .op = GPIO_OP_NONE };
	cli_action_t a = box_cli_exec(cfg, req.line, req.resp, sizeof req.resp, &cmd);

	switch (a) {
	case CLI_GPIO:
		box_gpio_exec(cfg, &cmd);
		break;
	case CLI_PIN:
	case CLI_GROUP:
	case CLI_AIN:
		box_gpio_apply_config(cfg);     /* pin/group/analog change -> re-apply */
		break;
	case CLI_SAVE:
#if defined(BOX_HAVE_PERSIST)
	{
		uint8_t blob[BOX_PERSIST_BLOB_MAX];
		uint32_t n = box_persist_serialize(cfg, blob, sizeof blob);

		resp_add(box_flash_save(blob, n) == 0 ? "saved\n" : "save FAILED\n");
	}
#else
		resp_add("no persistence on this board\n");
#endif
		break;
	case CLI_FACTORY:
		memset(cfg, 0, sizeof *cfg);
		box_gpio_apply_config(cfg);
		resp_add("factory reset\n");
		break;
	case CLI_REBOOT:
		do_reboot = 1;                  /* after the reply has gone out */
		break;
	case CLI_BOOTSEL:
		resp_add("bootsel unsupported here; press the Program button\n");
		break;
	default:
		break;
	}

	atomic_set(&req_pending, 0);
	k_sem_give(&req_done);

	if (do_reboot) {
		k_msleep(200);                  /* let the shell print + the CDC drain */
		sys_reboot(SYS_REBOOT_WARM);
	}
}

/* ---------------- command registration ---------------- */
/* One shell command per box_cli verb -> tab-completion and built-in `help`
 * listing come free, while the grammar itself stays box_cli's. */

#define BOX_SHELL_CMD(verb, help_text) \
	SHELL_CMD_REGISTER(verb, NULL, help_text, cmd_box)

SHELL_CMD_REGISTER(cmds, NULL, "List the full box command grammar", cmd_box_cmds);

BOX_SHELL_CMD(show,    "Show the box configuration");
BOX_SHELL_CMD(dump,    "Dump the config as replayable commands");
BOX_SHELL_CMD(name,    "name <NAME> -- set the box name (datapoint identity)");
BOX_SHELL_CMD(desc,    "desc <TEXT>|off -- free-form box description");
BOX_SHELL_CMD(channel, "channel [<NAME>|dev] -- firmware update track");
BOX_SHELL_CMD(pin,     "pin <N> mode|pulse|debounce|active_low <V>");
BOX_SHELL_CMD(label,   "label <N> <TEXT>|off -- per-pin role label");
BOX_SHELL_CMD(group,   "group <G> pins|label|settle|quiet|off <V>");
BOX_SHELL_CMD(ain,     "ain group <G> channels|label|mode|deadband|decimate|batch|average|off <V>");
BOX_SHELL_CMD(mcp,     "mcp enable 0|1 | mcp rate <HZ>");
BOX_SHELL_CMD(oled,    "oled enable 0|1");
BOX_SHELL_CMD(ble,     "ble enable|pipe|latency <V>");
BOX_SHELL_CMD(net,     "net mode dhcp|static | net ip|gateway|mask <A.B.C.D>");
BOX_SHELL_CMD(dserv,   "dserv ip <A.B.C.D> | dserv port <N>");
BOX_SHELL_CMD(wifi,    "wifi ssid|pass <V> | wifi pm 0|1");
BOX_SHELL_CMD(obs,     "obs pin <N> | obs off -- obs mirror output");
BOX_SHELL_CMD(sync,    "sync pin <N> | sync off -- TTL obs-sync input");
BOX_SHELL_CMD(do,      "do <N> 0|1 | do <N> pulse <US> -- drive a DO now");
BOX_SHELL_CMD(save,    "Persist the config to storage");
BOX_SHELL_CMD(factory, "Reset the config to defaults");
BOX_SHELL_CMD(reboot,  "Warm-reset the box");
BOX_SHELL_CMD(bootsel, "Enter the USB bootloader (where supported)");
#ifdef BOX_NET_DUAL
BOX_SHELL_CMD(mode,    "mode auto|usb|eth -- boot transport policy");
#endif
