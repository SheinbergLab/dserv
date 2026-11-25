/*
 * box_cmp.h -- an on-chip comparator as a light detector (PIN_MODE_CMP).
 *
 * A pin in `cmp` mode is an ain channel AND a DI line: the ADC samples it as
 * usual, and an on-chip comparator watches the same pad against a threshold
 * from its own DAC. The comparator's edges are reported as the pin's DI events
 * through box_gpio's FIFO / debounce latch, stamped at ISR entry -- so on the
 * host side a cmp pin is just a labelled digital input (`photo din`).
 *
 * THE THRESHOLD FOLLOWS THE SIGNAL. box_cmp_feed() watches the channel's
 * samples for plateaus (the signal still for a few ms), keeps the latest dark
 * and light plateau, and sets the DAC to their midpoint -- only ever while the
 * signal is on a plateau, so the threshold never moves under an edge. Until it
 * has seen two plateaus at least a minimum step apart, the comparator's
 * interrupt stays off: no edges are reported from a threshold nobody chose.
 * The channel must therefore be in an active ain group, or there is nothing to
 * learn from (the console says so).
 *
 * Implemented for the FRDM-MCXN947 (A3 = box pin 15 = P0_22 = CMP1_IN2). On
 * every other board these are stubs and a cmp pin behaves as plain ain.
 */
#ifndef BOX_CMP_H
#define BOX_CMP_H

#include "dserv_config.h"
#include <stdint.h>

/* Re-read the config: arm/disarm the comparator for the cmp pin, if any.
 * Called at the end of box_gpio_apply_config(), after the pads are muxed. */
void box_cmp_apply(const box_config_t *c);

/* The comparator's output right now (1 = signal above threshold), or 0 when
 * no comparator is running. */
int box_cmp_level(void);

/* One full ain scan (scan[ch] for ch in mask), from the sampling thread. */
void box_cmp_feed(const int16_t *scan, uint8_t mask, uint64_t t_us);

/* state/cmp: the comparator's state as a Tcl-dict string,
 *   "state armed lo 812 hi 2960 dac 117 vref 0 hyst 2 mismatch 0"
 * (lo/hi in ADC counts; the threshold is (dac + 1) * 4096 / 256 counts).
 * state is off | unsupported | no_samples | learning | armed. Returns 1 and fills buf when it is due: at once
 * on a state or mismatch change, else when the DAC has moved 2+ codes, or
 * moved at all and 10 s have passed -- at most every 250 ms. `force` (the
 * announce) fills it whenever a cmp pin is or was configured. */
int box_cmp_status(char *buf, int sz, int force);

/* Console: `cmp` shows state; `cmp hyst <0-3>`, `cmp vref <0|1>`,
 * `cmp dac <0-255>|auto` are runtime-only overrides. `args` is what followed
 * the verb ("" for bare `cmp`). */
void box_cmp_console(const char *args);

#endif /* BOX_CMP_H */
