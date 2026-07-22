/*
 * box_net_usb.c -- RW612/Zephyr implementation of the box_net_usb.h seam.
 * CDC-ACM data pipe over interrupt-driven UART with RX/TX ring buffers.
 */
#include "box_net_usb.h"
#include "box_usbd.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

static const struct device *data_dev;      /* cdc_acm_data: the 128-byte frame pipe */
static const struct device *console_dev;   /* cdc_acm_console: human CLI            */

/* 8 frames each way of slack; a stalled host costs at most this before drops. */
RING_BUF_DECLARE(rx_rb, 8 * 128);
RING_BUF_DECLARE(tx_rb, 8 * 128);

/* CDC-ACM data ISR: pump RX into rx_rb, drain tx_rb into the TX FIFO. */
static void data_isr(const struct device *dev, void *user)
{
	ARG_UNUSED(user);
	while (uart_irq_update(dev), uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t tmp[64];
			int n = uart_fifo_read(dev, tmp, sizeof tmp);
			if (n > 0) {
				(void) ring_buf_put(&rx_rb, tmp, (uint32_t) n);
			}
		}
		if (uart_irq_tx_ready(dev)) {
			uint8_t *p;
			uint32_t sz = ring_buf_get_claim(&tx_rb, &p, 64);
			if (sz == 0) {
				uart_irq_tx_disable(dev);       /* nothing queued */
			} else {
				int wrote = uart_fifo_fill(dev, p, sz);
				ring_buf_get_finish(&tx_rb, wrote > 0 ? (uint32_t) wrote : 0);
			}
		}
	}
}

int box_net_usb_reading(void)
{
	uint32_t dtr = 0;
	if (!data_dev) {
		return 0;
	}
	uart_line_ctrl_get(data_dev, UART_LINE_CTRL_DTR, &dtr);
	return dtr ? 1 : 0;
}

const struct device *box_net_usb_console(void) { return console_dev; }

int box_net_usb_init(void)
{
	data_dev    = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_data));
	console_dev = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_console));
	if (!device_is_ready(data_dev)) {
		return -1;
	}

	int err = box_usbd_start(NULL);
	if (err) {
		return err;
	}

	uart_irq_callback_user_data_set(data_dev, data_isr, NULL);
	uart_irq_rx_enable(data_dev);
	return 0;
}

int box_net_usb_server_poll(uint8_t *buf, int max)
{
	static int prev_dtr;
	int dtr = box_net_usb_reading();
	if (dtr && !prev_dtr) {                     /* host just opened the tty */
		prev_dtr = dtr;
		ring_buf_reset(&rx_rb);
		return BOX_NET_RESET;
	}
	prev_dtr = dtr;
	if (max <= 0) {
		return 0;
	}
	return (int) ring_buf_get(&rx_rb, buf, (uint32_t) max);
}

int box_net_usb_client_send(const uint8_t *buf, int len)
{
	if (!box_net_usb_reading()) {
		return -1;                          /* host not draining */
	}
	if (ring_buf_space_get(&tx_rb) < (uint32_t) len) {
		return -2;                          /* no room -> drop whole frame, never partial */
	}
	(void) ring_buf_put(&tx_rb, buf, (uint32_t) len);
	uart_irq_tx_enable(data_dev);
	return 0;
}
