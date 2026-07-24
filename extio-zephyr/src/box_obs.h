/*
 * box_obs.h -- observation-period state, and work deferred until the box is idle.
 *
 * `ess/in_obs` is the rig saying "I am collecting data right now". The box
 * already uses its EDGES to anchor the clock; this keeps the LEVEL, which is a
 * different and useful thing: a live "is this box mid-trial?" flag.
 *
 * Two uses:
 *   1. any subsystem can ask `box_obs_active()`;
 *   2. a parking lot for work that must not run mid-trial -- specifically
 *      anything that can BLOCK the single service loop (flash programming, OTA).
 *      Rather than REFUSING such a request, the caller parks it and the loop
 *      runs it on the first idle pass, so nothing is silently dropped and the
 *      requester still gets its effect, just later.
 *
 * Deliberately NOT a "priority mode". A box that behaves differently during obs
 * than between obs is harder to trust, because every bench measurement is taken
 * outside obs and would stop predicting in-obs behaviour. This only defers work
 * that is pathological (blocking), never work that is merely costly.
 *
 * Measured 2026-07-23: a plain `cmd/save` does NOT stall the loop -- the 1 Hz
 * watchdog held a flat 999-1000 ms straight through an NVS write. So deferral is
 * belt-and-braces for the common case. It is kept because the NVS
 * garbage-collect/erase path is UNTESTED (that one can take tens of ms), and
 * because OTA, when it lands, genuinely blocks -- the Pico gates it on !in_obs
 * for exactly this reason.
 *
 * Single-threaded by construction: the frame handler, the console and the loop
 * all run in the main service thread, so plain statics are safe here.
 */
#ifndef BOX_OBS_H
#define BOX_OBS_H

#include <stdint.h>

/* Deferred-work IDs (bitmask -- add OTA and friends as they land). */
#define BOX_DEFER_SAVE   (1u << 0)

/* Track the rig's obs level. Call from the ess/in_obs handler, both edges. */
void box_obs_set(int in_obs);

/* 1 while the rig is mid-observation. */
int box_obs_active(void);

/* Park work to run on the first idle pass. Repeated requests coalesce. */
void box_obs_defer(uint32_t what);

/* Loop side: returns (and clears) the pending set when the box is idle, else 0.
 * Call once per service pass. */
uint32_t box_obs_take_deferred(void);

#endif /* BOX_OBS_H */
