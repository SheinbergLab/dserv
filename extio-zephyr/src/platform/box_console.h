/*
 * box_console.h -- the box's two-way management console over the USB console CDC.
 *
 * Owns cdc_acm_console with interrupt-driven RX/TX ring buffers, so it is
 * non-blocking BY CONSTRUCTION: box_console_write() copies into the TX ring and
 * returns; the CDC TX interrupt drains it. If the host isn't reading and the
 * ring fills, output is DROPPED, never blocked -- the real-time service loop can
 * never stall on the console. Input is drained the same way and fed to box_cli
 * in the service loop (single-threaded -> no cfg locking).
 *
 * Zephyr's own printk/LOG stays on the board's hardware UART (we don't override
 * zephyr,console), so driver diagnostics remain available on a serial adapter
 * while this owns the USB console cleanly.
 */
#ifndef BOX_CONSOLE_H
#define BOX_CONSOLE_H

#include "dserv_config.h"

/* Claim cdc_acm_console + start the RX interrupt. 0 on success. */
int box_console_init(void);

/* Drain input, run any complete CLI line against cfg, queue the reply. Bounded
 * and non-blocking; call once per service pass. */
void box_console_service(box_config_t *cfg);

/* Queue text for output (non-blocking; silently drops if the TX ring is full). */
void box_console_write(const char *s);

/* printf-style convenience over box_console_write (non-blocking). */
__attribute__((format(printf, 1, 2)))
void box_console_printf(const char *fmt, ...);

#endif /* BOX_CONSOLE_H */
