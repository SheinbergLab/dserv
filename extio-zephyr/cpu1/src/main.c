/*
 * cpu1 main -- prove the second core boots, then keep proving it is alive.
 *
 * The whole program is the write ordering: every field first, DMB, magic
 * LAST, so cpu0 (polling from the other side of the AHB matrix) never reads
 * a half-initialized block as a live heartbeat. After that it is counter++
 * at a 10 ms cadence forever -- 100 Hz is fast enough that cpu0's 1 Hz
 * monitor sees ~100 ticks between looks (a frozen counter is unambiguous
 * within one look), slow enough to be invisible in the power budget.
 *
 * k_uptime rides along so a future debugging session can tell "cpu1 wedged"
 * from "cpu1 rebooted" (a reboot restarts uptime; a wedge freezes both).
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>

#include "box_ipc.h"

int main(void)
{
	volatile box_ipc_hb_t *hb = BOX_IPC_HB;

	hb->magic     = 0;               /* scrub first: never a stale-valid block */
	barrier_dmem_fence_full();

	hb->version   = BOX_IPC_HB_VERSION;
	hb->counter   = 0;
	hb->uptime_ms = 0;
	for (unsigned i = 0; i < ARRAY_SIZE(hb->reserved); i++) {
		hb->reserved[i] = 0;
	}
	barrier_dmem_fence_full();
	hb->magic = BOX_IPC_HB_MAGIC;    /* the block is now, and only now, live */

	for (;;) {
		hb->counter++;
		hb->uptime_ms = k_uptime_get_32();
		k_msleep(10);
	}
	return 0;
}
