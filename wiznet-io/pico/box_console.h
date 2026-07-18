/*
 * box_console.h -- non-blocking console plumbing for the dual-core split.
 *
 * printf on EITHER core -> that core's SPSC log ring (~1us, never blocks) ->
 * core 0 drains to the sinks: UART (direct; blocking is fine on core 0) and,
 * on the TinyUSB-owning builds (dual/usb), a cdc0-out ring that core 1's
 * ferry writes to the CDC0 console fire-and-forget (write what fits, never a
 * drain loop -- TinyUSB and all tud_* calls live on core 1). Console INPUT
 * mirrors it: UART read direct on core 0; CDC0 bytes ferried core 1 ->
 * cdc0-in ring -> core 0 line editor. The w6300/pico2w single builds have no
 * app-owned TinyUSB; their console is the SDK stdio_usb driver, demoted to a
 * sink/source called directly from core 0.
 *
 * Why: stdio drivers write synchronously (UART at ~87us/char, CDC0 behind a
 * drain loop), so one stray printf on the RT core was a milliseconds-scale
 * stall. Rings make log cost independent of sink speed; overflow drops
 * (counted, flagged in-stream as [log: N dropped]) instead of stalling.
 *
 * Rules: no printf from IRQ handlers (rings are SPSC per core mainline;
 * unchanged from today -- no handler prints). Panic caveat: core 1 panics
 * drain normally and stay visible; a panic ON core 0 can't drain its own
 * ring, so its final words are lost -- keep core 0 simple.
 */
#ifndef BOX_CONSOLE_H
#define BOX_CONSOLE_H

#include <stdio.h>
#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "pico/platform.h"      /* get_core_num */
#include "box_ring.h"

#if LIB_PICO_STDIO_UART
#include "pico/stdio_uart.h"
#include "hardware/uart.h"
#endif
#if LIB_PICO_STDIO_USB
#include "pico/stdio_usb.h"
#endif
#if defined(BOX_NET_DUAL) || defined(BOX_NET_USB) || defined(BOX_NET_BLE)
#include "tusb.h"
#define BOX_CONSOLE_TUSB 1      /* we own TinyUSB: CDC0 console via core-1 ferry
                                 * (BLE handheld: USB is console-only; data rides the radio) */
#endif

#define BOX_LOG_RING_BYTES  2048    /* per core; a full `show` dump bursts ~1KB  */
#define BOX_CDC0_OUT_BYTES  2048
#define BOX_CDC0_IN_BYTES   256

static box_ring_t box_log_ring[2];
static uint8_t    box_log_buf[2][BOX_LOG_RING_BYTES];
#ifdef BOX_CONSOLE_TUSB
static box_ring_t box_cdc0_out, box_cdc0_in;
static uint8_t    box_cdc0_out_buf[BOX_CDC0_OUT_BYTES], box_cdc0_in_buf[BOX_CDC0_IN_BYTES];
#endif

/* ---- the stdio driver: printf -> this core's ring, done ---- */
static void box_console_out(const char *buf, int len)
{ box_ring_write(&box_log_ring[get_core_num()], (const uint8_t *) buf, (uint32_t) len); }
static void box_console_flush(void) { }
static stdio_driver_t box_console_driver = {
    .out_chars    = box_console_out,
    .out_flush    = box_console_flush,
    .crlf_enabled = true,           /* \n -> \r\n happens BEFORE the ring, so    */
};                                  /* sinks write the bytes raw                 */

static inline void box_console_init(void)
{
    box_ring_init(&box_log_ring[0], box_log_buf[0], BOX_LOG_RING_BYTES);
    box_ring_init(&box_log_ring[1], box_log_buf[1], BOX_LOG_RING_BYTES);
#ifdef BOX_CONSOLE_TUSB
    box_ring_init(&box_cdc0_out, box_cdc0_out_buf, BOX_CDC0_OUT_BYTES);
    box_ring_init(&box_cdc0_in,  box_cdc0_in_buf,  BOX_CDC0_IN_BYTES);
#endif
    stdio_set_driver_enabled(&box_console_driver, true);
#if LIB_PICO_STDIO_UART
    stdio_set_driver_enabled(&stdio_uart, false);   /* demoted to a sink (drain writes it directly) */
#endif
#if LIB_PICO_STDIO_USB
    stdio_set_driver_enabled(&stdio_usb, false);    /* single builds: SDK CDC likewise sink-only     */
#endif
}

/* ---- core 0: fan one drained chunk out to every console sink ---- */
static inline void box_console_sink(const uint8_t *b, uint32_t n)
{
#if LIB_PICO_STDIO_UART
    stdio_uart.out_chars((const char *) b, (int) n);
#endif
#ifdef BOX_CONSOLE_TUSB
    box_ring_write(&box_cdc0_out, b, n);            /* core 1 ferries this to CDC0 */
#elif LIB_PICO_STDIO_USB
    stdio_usb.out_chars((const char *) b, (int) n);
#endif
}

/* Core 0, once per loop pass: move a bounded slice of both log rings to the
 * sinks (bounded so a debug flood can't pin core 0 -- the ring absorbs the
 * burst and overflow drops are flagged in-stream). */
static inline void box_console_drain(void)
{
    static uint32_t seen_drops[2];
    uint8_t chunk[64];
    for (int core = 0; core < 2; core++) {
        uint32_t n = box_ring_read(&box_log_ring[core], chunk, sizeof chunk);
        if (n) box_console_sink(chunk, n);
        uint32_t d = box_log_ring[core].drops;
        if (d != seen_drops[core] && box_ring_used(&box_log_ring[core]) == 0) {
            char msg[40];
            int m = snprintf(msg, sizeof msg, "[log: %lu dropped]\r\n",
                             (unsigned long)(d - seen_drops[core]));
            seen_drops[core] = d;
            box_console_sink((const uint8_t *) msg, (uint32_t) m);
        }
    }
}

/* Core 0: one console char from any source, -1 if none this pass. */
static inline int box_console_getc(void)
{
#ifdef BOX_CONSOLE_TUSB
    uint8_t b;
    if (box_ring_read(&box_cdc0_in, &b, 1)) return b;
#elif LIB_PICO_STDIO_USB
    char c;
    if (stdio_usb.in_chars(&c, 1) == 1) return (uint8_t) c;
#endif
#if LIB_PICO_STDIO_UART
    if (uart_is_readable(uart_default)) return uart_getc(uart_default);
#endif
    return -1;
}

#ifdef BOX_CONSOLE_TUSB
/* Terminal-attach event (DTR rising edge), consumed by the app to print a
 * greeting. Everything logged before DTR is deliberately DISCARDED (below), so
 * without this a USB console never sees boot output -- transport choice, boot
 * cause, the ready banner all land in the pre-attach window. */
static volatile uint8_t box_console_attach_evt;

/* Core 1, once per loop pass: pump CDC0 <-> the console rings, strictly
 * bounded. RX drains the CDC fifo into cdc0-in; TX is gated on DTR (a real
 * terminal) exactly like the old console driver -- no terminal means the
 * backlog is discarded fast so stale output never dumps on attach -- and
 * writes only what the CDC fifo will take right now (never a drain loop). */
static inline void box_console_cdc0_ferry(void)
{
    static uint8_t was_attached;
    uint8_t b[32];
    while (tud_cdc_n_available(0)) {
        uint32_t n = tud_cdc_n_read(0, b, sizeof b);
        if (!n) break;
        box_ring_write(&box_cdc0_in, b, n);
    }
    uint8_t attached = tud_ready() && tud_cdc_n_connected(0);
    if (attached && !was_attached) box_console_attach_evt = 1;
    was_attached = attached;
    if (!attached) {
        while (box_ring_read(&box_cdc0_out, b, sizeof b)) ;
        return;
    }
    uint32_t avail = tud_cdc_n_write_available(0);
    uint32_t wrote = 0;
    while (avail) {
        uint32_t n = box_ring_read(&box_cdc0_out, b, avail < sizeof b ? avail : sizeof b);
        if (!n) break;
        tud_cdc_n_write(0, b, n);
        avail -= n; wrote += n;
    }
    if (wrote) tud_cdc_n_write_flush(0);
}
#endif

#endif /* BOX_CONSOLE_H */
