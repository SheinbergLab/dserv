/*
 * box_console.c -- interrupt-driven, non-blocking CLI console (see box_console.h).
 */
#include "box_console.h"
#include "box_cli.h"
#include "box_gpio.h"
#include "box_event.h"

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

static const struct device *con;

RING_BUF_DECLARE(con_rx, 256);
RING_BUF_DECLARE(con_tx, 1024);

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
	(void) ring_buf_put(&con_tx, (const uint8_t *) s, (uint32_t) strlen(s));
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

int box_console_init(void)
{
	con = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_console));
	if (!device_is_ready(con)) {
		con = NULL;
		return -1;
	}
	uart_irq_callback_user_data_set(con, con_isr, NULL);
	uart_irq_rx_enable(con);
	return 0;
}

/* ---- line assembly + dispatch to box_cli ---- */
static char line_buf[128];
static int  line_len;

static void run_line(box_config_t *cfg, const char *line)
{
	char resp[1024];   /* `help`/`show` output is large */
	gpio_cmd_t cmd = { .op = GPIO_OP_NONE };
	cli_action_t a = box_cli_exec(cfg, line, resp, sizeof resp, &cmd);

	box_console_write(resp);   /* the OK/ERR line box_cli produced */

	switch (a) {
	case CLI_GPIO:
		box_gpio_exec(cfg, &cmd);
		break;
	case CLI_PIN:
	case CLI_GROUP:
	case CLI_AIN:
		box_gpio_apply_config(cfg);   /* pin/group/analog change -> re-apply */
		break;
	case CLI_SAVE:
#if defined(BOX_HAVE_PERSIST)
	{
		uint8_t blob[BOX_PERSIST_BLOB_MAX];
		uint32_t n = box_persist_serialize(cfg, blob, sizeof blob);
		box_console_printf("%s\r\n", box_flash_save(blob, n) == 0 ? "saved" : "save FAILED");
	}
#else
		box_console_write("no persistence on this board\r\n");
#endif
		break;
	case CLI_FACTORY:
		memset(cfg, 0, sizeof *cfg);
		box_gpio_apply_config(cfg);
		box_console_write("factory reset\r\n");
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

void box_console_service(box_config_t *cfg)
{
	uint8_t c;
	while (ring_buf_get(&con_rx, &c, 1) == 1) {
		if (c == '\r' || c == '\n') {
			box_console_write("\r\n");
			if (line_len > 0) {
				line_buf[line_len] = '\0';
				run_line(cfg, line_buf);
			}
			line_len = 0;
			box_console_write("> ");
		} else if (c == 0x08 || c == 0x7f) {          /* backspace / DEL */
			if (line_len > 0) {
				line_len--;
				box_console_write("\b \b");
			}
		} else if (line_len < (int) sizeof(line_buf) - 1) {
			line_buf[line_len++] = (char) c;
			char echo[2] = { (char) c, '\0' };
			box_console_write(echo);              /* echo (extio-setup skips it) */
		}
	}
}
