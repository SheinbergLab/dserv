/*
 * box_cpu1.c -- copy the embedded cpu1 image into SRAMG and let it run.
 *
 * The release sequence is soc.c's second_core_boot() (zephyr/soc/nxp/mcx/mcxn)
 * transplanted, with one deliberate difference: the vendor hook runs at
 * PRE_KERNEL_2 and boots cpu1 from a FLASH partition named in devicetree --
 * which on this box is slot1, the OTA staging area, a collision we refuse.
 * Running from SRAM instead means someone must COPY the image first, so the
 * whole ceremony moves here, into app code, where the copy and the release
 * can be sequenced explicitly and CONFIG_SECOND_CORE_MCUX stays off (its
 * SYS_INIT would boot a stale slot1 before main() ever ran).
 *
 * SRAM residence also buys the property the RT-delegation design wants:
 * cpu1 keeps executing through every flash irq_lock blackout (OTA pages,
 * NVS saves) that stalls XIP on cpu0.
 */
#include "box_cpu1.h"
#include "box_ipc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <soc.h>
#include <string.h>

/* Generated from the cpu1 sysbuild image's zephyr.bin (CMakeLists.txt);
 * CONFIG_XIP=n over there means these bytes ARE the RAM image, vector
 * table first, linked for exactly this address. */
static const uint8_t cpu1_fw[] = {
#include <cpu1_fw.inc>
};

/* SRAMG through the SECURE alias (bit 29 set): the cpu1 image links itself at
 * 0x30050000 -- readelf shows its entry inside 0x3005xxxx -- because the cpu1
 * board target builds with the secure address map, exactly like cpu0's own
 * image lives at flash alias 0x10000000. The 0x2005/0x3005 views are the same
 * physical bytes, but CPBOOT should hand the core the address family the
 * image believes in, so both the copy and the boot pointer use the alias the
 * linker used. (CPBOOT's mask is 0xFFFFFF80: bit 29 survives it.) */
#define CPU1_SRAM_BASE 0x30050000UL      /* SRAMG -- cpu1's bank (64 KB) */
#define CPU1_SRAM_SIZE (64 * 1024)

int box_cpu1_start(void)
{
	if (sizeof(cpu1_fw) > CPU1_SRAM_SIZE) {
		printk("cpu1: image %u B exceeds SRAMG (%u) -- NOT released\n",
		       (unsigned) sizeof(cpu1_fw), (unsigned) CPU1_SRAM_SIZE);
		return -1;
	}

	/* HOLD cpu1 in reset before touching anything it may be executing.
	 *
	 * A cpu0 reboot does NOT reset a running cpu1: measured on box02, the
	 * heartbeat ran 22 minutes straight through THREE cpu0 reflashes, and
	 * the original copy-then-pulse-reset sequence here never actually
	 * reset it (the pulse lands on a live core and evidently bounces
	 * off; the copy landed on executing code -- harmless only while the
	 * bytes were identical). So: assert reset FIRST, keep it held across
	 * the scrub and the copy, and make the release at the bottom the one
	 * and only un-reset this function performs. */
	uint32_t temp = SYSCON->CPUCTRL | 0xc0c40000U;

	SYSCON->CPUCTRL = temp | SYSCON_CPUCTRL_CPU1RSTEN_MASK | SYSCON_CPUCTRL_CPU1CLKEN_MASK;
	barrier_dsync_fence_full();

	/* Scrub the handshake block so this boot can only ever see a heartbeat
	 * written by the core we are about to start, never a leftover. Must
	 * follow the reset assert: a still-running cpu1 would just keep the
	 * old block alive under us (that is exactly what happened). */
	BOX_IPC_HB->magic = 0;
	barrier_dmem_fence_full();

	memcpy((void *) CPU1_SRAM_BASE, cpu1_fw, sizeof(cpu1_fw));
	barrier_dsync_fence_full();

	/* soc.c second_core_boot(), verbatim except the boot address:
	 * bus access level for the cpu1 master, the vector base, then the
	 * single release. */
	AHBSC->MASTER_SEC_LEVEL |= AHBSC_MASTER_SEC_LEVEL_CPU1(3);
	AHBSC->MASTER_SEC_ANTI_POL_REG =
		(~AHBSC->MASTER_SEC_LEVEL &
		 ~AHBSC_MASTER_SEC_ANTI_POL_REG_MASTER_SEC_LEVEL_ANTIPOL_LOCK_MASK) |
		AHBSC_MASTER_SEC_ANTI_POL_REG_MASTER_SEC_LEVEL_ANTIPOL_LOCK(2);

	SYSCON->CPBOOT = ((uint32_t) CPU1_SRAM_BASE) & SYSCON_CPBOOT_CPBOOT_MASK;

	SYSCON->CPUCTRL = (temp | SYSCON_CPUCTRL_CPU1CLKEN_MASK) & (~SYSCON_CPUCTRL_CPU1RSTEN_MASK);

	printk("cpu1: released (image %u B in SRAMG, heartbeat in SRAMH)\n",
	       (unsigned) sizeof(cpu1_fw));
	return 0;
}

void box_cpu1_stats(uint32_t *count, int *alive)
{
	static uint32_t last;
	static int seen;
	volatile box_ipc_hb_t *hb = BOX_IPC_HB;

	if (hb->magic != BOX_IPC_HB_MAGIC || hb->version != BOX_IPC_HB_VERSION) {
		*count = 0;
		*alive = 0;
		return;
	}
	uint32_t c = hb->counter;

	*count = c;
	*alive = (seen && c != last);
	last = c;
	seen = 1;
}
