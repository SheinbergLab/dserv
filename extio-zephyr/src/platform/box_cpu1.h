/*
 * box_cpu1.h -- cpu0's side of the MCXN947 second core (heartbeat bring-up).
 *
 * cpu0 owns the whole lifecycle: it carries cpu1's image as an embedded blob
 * inside its OWN image (so the shelf/OTA/TBYB pipeline updates both cores as
 * one artifact and version skew between them cannot exist), copies it into
 * SRAMG at boot, points SYSCON->CPBOOT at it, and releases the core. See
 * box_ipc.h for the shared-memory contract and why SRAMH holds it.
 *
 * Only built for CONFIG_BOARD_FRDM_MCXN947 under sysbuild (BOX_HAVE_CPU1);
 * every other board, and the plain build-mcxn947 compile-check dir, knows
 * nothing about any of this.
 */
#ifndef BOX_CPU1_H
#define BOX_CPU1_H

#include <stdint.h>

/* Copy the embedded image into SRAMG and release cpu1. Call once, any time
 * after kernel init -- cpu1 needs nothing from the network or the config.
 * 0 ok, <0 if the blob does not fit its bank (a build error, caught loudly
 * here rather than as a silently truncated core). */
int box_cpu1_start(void);

/* Heartbeat check for the 1 Hz monitor: *count is cpu1's beat counter,
 * *alive is 1 iff the block is valid AND the counter moved since the last
 * call. First call after boot reports alive=0 by construction. */
void box_cpu1_stats(uint32_t *count, int *alive);

#endif /* BOX_CPU1_H */
