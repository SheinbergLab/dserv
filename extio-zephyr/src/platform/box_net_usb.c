/*
 * box_net_usb.c -- RW612/Zephyr implementation of the box_net_usb.h seam.
 * CDC-ACM data pipe over interrupt-driven UART with RX/TX ring buffers.
 */
#include "box_net_usb.h"
#include "box_usbd.h"
#include "box_event.h"

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
				box_event_signal();   /* inbound frame bytes: wake the loop */
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

int box_net_usb_init(int with_data_pipe)
{
	data_dev    = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_data));
	console_dev = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_console));
	if (!device_is_ready(data_dev)) {
		return -1;
	}

	int err = box_usbd_start(NULL, with_data_pipe);
	if (err) {
		return err;
	}

	/* No data pipe -> no RX interrupt to arm. The uart device still exists (it is
	 * a devicetree node and its driver inits regardless), but its class was never
	 * registered with the USB stack, so there are no endpoints behind it. Arming
	 * the ISR anyway would be harmless-but-dishonest; skipping it keeps "no data
	 * pipe" true all the way down. The console is unaffected -- box_console owns
	 * it separately, which is what keeps a declared-Ethernet box diagnosable. */
	if (with_data_pipe) {
		uart_irq_callback_user_data_set(data_dev, data_isr, NULL);
		uart_irq_rx_enable(data_dev);
	}
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

/* ---- send-cost instrumentation (mirrors box_net_eth.c) ----
 * USB is ASYNCHRONOUS BY CONSTRUCTION: this copies into a ring and enables the
 * TX interrupt, so the caller pays a memcpy and the wire time happens in the ISR.
 * Ethernet's send was a full stack traversal INLINE in the caller -- 275 us
 * before the ITCM fix, 61 us after. Publishing both lets a datapoint budget be
 * derived rather than guessed.
 *
 * For USB the binding limit is NOT loop time, it is the ring filling faster than
 * the host drains it: drops are whole frames (never partial), counted here. A
 * rising drop count is the real "too many datapoints" signal on this transport. */
static uint32_t usend_last_us, usend_max_us, usend_drops;

void box_net_usb_send_stats(uint32_t *last_us, uint32_t *max_us, uint32_t *drops)
{
	if (last_us) *last_us = usend_last_us;
	if (max_us)  *max_us  = usend_max_us;
	if (drops)   *drops   = usend_drops;
}

int box_net_usb_client_send(const uint8_t *buf, int len)
{
	if (!box_net_usb_reading()) {
		return -1;                          /* host not draining */
	}
	if (ring_buf_space_get(&tx_rb) < (uint32_t) len) {
		usend_drops++;
		return -2;                          /* no room -> drop whole frame, never partial */
	}
	uint32_t t0 = k_cycle_get_32();
	(void) ring_buf_put(&tx_rb, buf, (uint32_t) len);
	uart_irq_tx_enable(data_dev);
	uint32_t dt = k_cyc_to_us_floor32(k_cycle_get_32() - t0);

	usend_last_us = dt;
	if (dt > usend_max_us) {
		usend_max_us = dt;
	}
	return 0;
}

int box_net_usb_client_send_stream(const uint8_t *buf, int len)
{
	if (!box_net_usb_reading()) {
		return -1;
	}
	/* Whole frames only: the ring is drained by the CDC ISR with no notion of
	 * record boundaries, so a partial frame here would be a partial frame on
	 * the wire. Truncating to frame granularity is what makes the caller's
	 * resume-at-offset arithmetic stay record-aligned. */
	uint32_t space = ring_buf_space_get(&tx_rb);
	int take = (int) MIN((uint32_t) len, space);

	take -= take % 128;                     /* the ring is declared in 128s too */
	if (take == 0) {
		return 0;
	}
	uint32_t t0 = k_cycle_get_32();

	(void) ring_buf_put(&tx_rb, buf, (uint32_t) take);
	uart_irq_tx_enable(data_dev);
	uint32_t dt = k_cyc_to_us_floor32(k_cycle_get_32() - t0);

	usend_last_us = dt;
	if (dt > usend_max_us) {
		usend_max_us = dt;
	}
	return take;
}
