/*
 * box_fast.h -- the inbound schedule fast path (+29).
 *
 * Implemented in main.c (it owns every piece of state the handlers touch:
 * the clock, the obs epoch, the sched table, the ledger counters). Called
 * from the eth reader thread (box_net_eth.c) for every complete inbound
 * frame, at priority 2, microseconds after the bytes left the wire.
 *
 * Returns 1 when the frame was CONSUMED (one of the schedule classes --
 * cmd/do/<n>/at, cmd/timer/<t>/at, cmd/do/<n>/at_abs -- or the obs epoch
 * carrier ess/in_obs): the caller must not queue it, the service loop never
 * sees it. Returns 0 for everything else: queue it for ordinary dispatch.
 * Also counts the frame into state/cmds_rx either way, so callers on this
 * path must not count again.
 *
 * arr_us is the arrival stamp (box clock, from the reader's recv). The USB
 * path reaches the same handlers through main.c's own dispatch with
 * arr_us = processing time -- the historical conflation, unchanged there.
 */
#ifndef BOX_FAST_H
#define BOX_FAST_H

#include <stdint.h>

int box_main_fast_frame(const uint8_t *frame, uint64_t arr_us);

#endif /* BOX_FAST_H */
