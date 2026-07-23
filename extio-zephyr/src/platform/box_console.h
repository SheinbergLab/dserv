/*
 * box_console.h -- the box's two-way management console over the USB console CDC.
 *
 * Implemented on Zephyr's Shell subsystem (see box_console.c): the standard
 * shell_uart backend owns cdc_acm_console via chosen zephyr,shell-uart, and each
 * box_cli verb is registered as a shell command. The command grammar is
 * unchanged -- `pin 3 mode out` still reaches box_cli_exec verbatim.
 *
 * Commands are marshaled from the shell thread to the service loop, so config
 * and GPIO stay single-threaded exactly as before (no cfg locking); the loop
 * runs the line in box_console_service() and hands the response back.
 *
 * Zephyr's own printk/LOG stays on the board's hardware UART (we don't override
 * zephyr,console), so driver diagnostics remain available on a serial adapter
 * while the shell owns the USB console cleanly.
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
