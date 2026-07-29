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

/* Bind the console device per cfg->console_mode (CDC or the board's console
 * UART) and start its RX interrupt. 0 on success. MUST be called AFTER the
 * persisted config is loaded, or the saved choice is ignored. */
int box_console_init(const box_config_t *cfg);

/* Drain input, run any complete CLI line against cfg, queue the reply. Bounded
 * and non-blocking; call once per service pass. */
void box_console_service(box_config_t *cfg);

/* Queue text for output (non-blocking; silently drops if the TX ring is full). */
/* Tell the CLI how many analog channels the platform actually fitted, so
 * `ain group G channels ...` validates against reality instead of against the
 * MCP3204's four. Call AFTER box_ain_init(). Routed through the console rather
 * than calling box_cli_set_ain_channels() directly, so box_cli.h keeps exactly
 * one includer (see the note in that header). */
void box_console_set_ain_channels(int n);

void box_console_write(const char *s);

/* printf-style convenience over box_console_write (non-blocking). */
__attribute__((format(printf, 1, 2)))
void box_console_printf(const char *fmt, ...);

/* Supplied by the APP (main.c), reported by `show`.
 *
 * Inbound frames accepted since boot. main.c's cmds_rx comment explains why a
 * plain counter is the only thing that reveals "publishing but deaf" -- the
 * uplink works, state/* keeps flowing, every status field reads healthy, and
 * cmd/* silently never arrives. That comment ends "no field on the box revealed
 * it", which was true: the counter was published to dserv but the box's OWN
 * console could not show it, so a box you were holding could not tell you its
 * downlink was dead. This is that field. */
uint32_t box_cmds_rx(void);

#endif /* BOX_CONSOLE_H */