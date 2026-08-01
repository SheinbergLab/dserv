/*
 * box_net_usb.h -- USB-CDC transport for the extio box (RW612/Zephyr).
 *
 * Fills the same seam as wiznet-io/net/box_net_usb.h on the Pico, but over a
 * Zephyr CDC-ACM interface (interrupt-driven UART + ring buffers) instead of
 * TinyUSB. The box speaks the identical 128-byte dserv frames; a host-side dserv
 * module (modules/usbio) reads them off the data tty and injects them. Config
 * and data share this one binary pipe -- the framer separates them by datapoint
 * name -- so "config alongside the data pump" needs no second data channel; the
 * console CDC is only the human CLI.
 */
#ifndef BOX_NET_USB_H
#define BOX_NET_USB_H

#include <zephyr/device.h>
#include <stdint.h>

#define BOX_NET_RESET (-1)   /* server_poll: host (re)opened the pipe; reset the framer */

/* usbd + CDC-ACM bring-up (data pipe irq + enable). 0 on success. */
/* with_data_pipe = 0 enumerates the console CDC only (declared-Ethernet box);
 * see box_usbd.h for why. The console is always registered. */
int box_net_usb_init(int with_data_pipe);

/* Drain inbound bytes (host -> box) into buf. Returns bytes read (0 if none), or
 * BOX_NET_RESET the pass the host opens the data tty (partial frame is stale). */
int box_net_usb_server_poll(uint8_t *buf, int max);

/* Publish one frame (box -> host), all-or-nothing: enqueued only if the TX ring
 * has room for the whole frame (never a partial frame on the wire). 0 ok, <0 if
 * the host isn't draining or the ring is full (best-effort drop). */
/* Caller-side cost of a USB publish, and frames dropped for a full TX ring.
 * The drop count is the meaningful "too many datapoints" signal here -- USB's
 * limit is the host's drain rate, not service-loop time. */
void box_net_usb_send_stats(uint32_t *last_us, uint32_t *max_us, uint32_t *drops);

int box_net_usb_client_send(const uint8_t *buf, int len);

/* Gathered send (box_pub.c): len = k * 128. Puts as many WHOLE frames as the
 * TX ring has room for; returns that byte count (0 = ring full, -1 = host not
 * draining). Frame-atomic by construction -- the ring never takes a partial. */
int box_net_usb_client_send_stream(const uint8_t *buf, int len);

/* 1 iff the host has the DATA tty open (DTR asserted) -- i.e. actually draining. */
int box_net_usb_reading(void);

/* The console CDC device (human CLI / bootstrap), for a later CLI block. */
const struct device *box_net_usb_console(void);

#endif /* BOX_NET_USB_H */
