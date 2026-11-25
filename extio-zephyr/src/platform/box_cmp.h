/*
 * box_cmp.h -- an on-chip comparator as a light detector (PIN_MODE_CMP).
 *
 * A pin in `cmp` mode is an ain channel AND a DI line: the ADC samples it as
 * usual, and an on-chip comparator watches the same pad against a threshold
 * from its own DAC. The comparator's edges are reported as the pin's DI events
 * through box_gpio's FIFO / debounce latch, stamped at ISR entry -- so on the
 * host side a cmp pin is just a labelled digital input (`photo din`).
 *
 * THE THRESHOLD IS LEARNED, THEN LOCKED. box_cmp_feed() watches the
 * channel's samples for plateaus (the signal still for a few ms). Once it has
 * seen one level, a second at least a minimum step away, and a return to the
 * first, it sets the DAC to their midpoint, arms the interrupt, and holds the
 * threshold there -- for the session, like a pot. Before that no edges are
 * reported from a threshold nobody chose. It moves again only on a relearn
 * (box_cmp_relearn), which waits for the end of an obs. While locked the box
 * keeps tracking the levels to report their `drift` from the threshold.
 * The box samples the channel for this itself (box_cmp_ain_mask) with ain
 * enabled, so no ain group is needed: the edges and state/cmp are all that
 * cross the wire. A group on the channel is only for SEEING the analog.
 *
 * Implemented, always as box pin 15, for:
 *   FRDM-MCXN947  A3 = P0_22 = ain ch 3 = CMP1_IN2 (the line's own pad); 8-bit DAC
 *   Teensy 4.0/4.1  A0 (T14) = GPIO_AD_B1_02 = ain ch 7 = CMP1_IN3; the line is
 *                 box pin 15, a pad not bonded out, since analog pads are not
 *                 box pins there; 6-bit DAC, so a 192-count minimum step
 * On every other board these are stubs and a cmp pin behaves as plain ain.
 */
#ifndef BOX_CMP_H
#define BOX_CMP_H

#include "dserv_config.h"
#include <stdint.h>

/* Re-read the config: arm/disarm the comparator for the cmp pin, if any.
 * Called at the end of box_gpio_apply_config(), after the pads are muxed. */
void box_cmp_apply(const box_config_t *c);

/* The ain channel(s) a cmp pin in `c` needs sampled, as a channel mask: the
 * sampler adds them to what the groups ask for, so a cmp pin needs no group
 * (box_ain.c recompute). 0 on boards without comparator support. */
uint8_t box_cmp_ain_mask(const box_config_t *c);

/* The comparator's output right now (1 = signal above threshold), or 0 when
 * no comparator is running. */
int box_cmp_level(void);

/* One full ain scan (scan[ch] for ch in mask), from the sampling thread. */
void box_cmp_feed(const int16_t *scan, uint8_t mask, uint64_t t_us);

/* Forget the levels and learn again (console `cmp relearn`, cmd/cmp/relearn).
 * Edges stop until the new threshold locks. Deferred to the end of an obs. */
void box_cmp_relearn(void);

/* state/cmp: the comparator's state as a Tcl-dict string,
 *   "state armed lo 812 hi 2960 dac 117 drift 3 mismatch 0"
 * lo/hi in ADC counts (learning: the candidate levels); the threshold is
 * (dac + 1) * 4096 / 256 counts; drift is how far the levels' midpoint has
 * moved from it since it locked, % of the step. " vref V hyst H" is appended
 * only when changed from 0 / 2. state is off | unsupported | no_samples |
 * learning | armed. Returns 1 and fills buf when it is due: at once on a
 * change of state, DAC, mismatch count or relearn count; a drift move of 5
 * points at most every 10 s. `force` (the announce) fills it whenever a cmp
 * pin is or was configured. */
int box_cmp_status(char *buf, int sz, int force);

/* state/cmp/level: a new settled level of the channel (ADC counts), stamped
 * when the light reached it. Returns 1 and fills both when one is due: when
 * a still period lands more than 32 counts from the last level reported, at
 * most 20/s (the newest wins). About one per patch change; nothing while the
 * light stays put, except that each announce (box_cmp_status force) sends the
 * last one again, so a host that just connected has it. */
int box_cmp_settled(int32_t *counts, uint64_t *t_us);

/* Console: `cmp` shows state; `cmp relearn`; `cmp hyst <0-3>`,
 * `cmp vref <0|1>`, `cmp dac <code>|auto` (code 0-255 on the MCXN947, 0-63
 * on a Teensy) are runtime-only overrides. `args` is what followed the verb
 * ("" for bare `cmp`). */
void box_cmp_console(const char *args);

#endif /* BOX_CMP_H */
