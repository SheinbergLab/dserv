/*
 * box_ipc.h -- the cpu0<->cpu1 shared-memory contract (MCXN947 dual-core).
 *
 * Lives in SRAMH (0x20060000, 32 KB), the one bank NEITHER core's linker owns:
 * cpu0's image ends at 0x20050000 (sram0, 320 KB), cpu1's image fills SRAMG
 * (0x20050000, 64 KB). SRAMH is addressed by pointer from both sides, never by
 * a linker section, so neither image's layout can silently move it -- the
 * address IS the contract, and this header is its single definition.
 *
 * v1 is a heartbeat only: cpu1 proves it booted and keeps proving it is alive;
 * cpu0 watches and publishes. The ring buffers for the compute-consumer design
 * (samples over, derived events back) will occupy the rest of the bank behind
 * the same magic/version gate when they exist.
 *
 * WRITE ORDERING: cpu1 fills every field, issues a DMB, and writes `magic`
 * LAST -- so cpu0 never trusts a half-written block. cpu0 treats wrong magic
 * or version as "cpu1 not up", not as an error: a cpu0-only image on a box
 * with no cpu1 blob reads erased/garbage RAM here and simply reports dead.
 * These M33s have no data cache, so barriers are all that is needed.
 */
#ifndef BOX_IPC_H
#define BOX_IPC_H

#include <stdint.h>

#define BOX_IPC_BASE       0x20060000UL          /* SRAMH -- see above */

#define BOX_IPC_HB_MAGIC   0x62783163UL          /* "bx1c" */
#define BOX_IPC_HB_VERSION 1

typedef struct {
	uint32_t magic;                  /* BOX_IPC_HB_MAGIC, written LAST      */
	uint32_t version;                /* BOX_IPC_HB_VERSION                  */
	uint32_t counter;                /* +1 per cpu1 beat (10 ms cadence)    */
	uint32_t uptime_ms;              /* cpu1's k_uptime, its own clock      */
	uint32_t reserved[4];            /* future: ring descriptors go here    */
} box_ipc_hb_t;

#define BOX_IPC_HB ((volatile box_ipc_hb_t *) BOX_IPC_BASE)

#endif /* BOX_IPC_H */
