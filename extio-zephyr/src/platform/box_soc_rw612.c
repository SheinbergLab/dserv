/*
 * box_soc_rw612.c -- repair SoC clock state that MCUboot tears down.
 *
 * ONLY built for frdm_rw612 AND only when chainloaded from MCUboot.
 *
 * ---------------------------------------------------------------------------
 * The bug this exists for
 * ---------------------------------------------------------------------------
 * Zephyr's RW6xx SoC init (soc/nxp/rw/soc.c) ends with:
 *
 *     #if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(enet)) && \
 *         CONFIG_NET_L2_ETHERNET && CONFIG_ETH_DRIVER
 *             RESET_PeripheralReset(kENET_IPG_RST_SHIFT_RSTn);
 *             RESET_PeripheralReset(kENET_IPG_S_RST_SHIFT_RSTn);
 *     #else
 *             CLOCK_DeinitTddrRefClk();          <-- powers DOWN the TDDR PLL
 *     #endif
 *
 * MCUboot is a Zephyr app with no networking, so it takes the #else and powers
 * the TDDR PLL down. Our app takes the #if -- which only RESETS the peripheral.
 * Nothing in the entire Zephyr tree ever calls CLOCK_InitTddrRefClk(); the ENET
 * path silently assumes the boot ROM left that PLL running, which is true when
 * the ROM launches the app directly and FALSE the moment a bootloader sits in
 * front. Put MCUboot in the path and the app inherits a dead tddr_mci_enet_clk.
 *
 * ---------------------------------------------------------------------------
 * Why it presents as a total freeze rather than "Ethernet doesn't work"
 * ---------------------------------------------------------------------------
 * The IPG bus clock is fine, so the ENET registers read back normally and MDIO
 * works well enough that the PHY driver logs "entering autonegotiation
 * sequence". Only the 1588 timer is unclocked. Then the first PTP clock read
 * lands in the MCUX SDK:
 *
 *     base->ATCR |= ENET_ATCR_CAPTURE_MASK;
 *     while (0U != (base->ATCR & ENET_ATCR_CAPTURE_MASK)) { }   // fsl_enet.c
 *
 * -- an UNBOUNDED spin, and its caller ENET_Ptp1588GetTimer() holds
 * DisableGlobalIRQ() across it. With no 1588 clock the CAPTURE bit never
 * self-clears, so the box wedges with PRIMASK=1 and every interrupt masked
 * forever. Diagnosed on silicon 2026-07-27: ticks froze at a repeatable
 * 8.37 s (10.43 s with BT built in -- BT only shifts WHEN the first PTP read
 * happens, it is not the cause), ATCR read 0x291 -> 0xA91 (bit 11 = CAPTURE,
 * stuck), ATVR stayed 0, and the OS timer IRQ sat enabled-and-pending but never
 * taken. From outside it looks like a board that boots, blinks, autonegotiates
 * forever and never gets a DHCP address.
 *
 * ---------------------------------------------------------------------------
 * The repair
 * ---------------------------------------------------------------------------
 * Mirror CLOCK_DeinitTddrRefClk() exactly, in reverse, before any driver runs.
 * Note CLOCK_InitTddrRefClk() does NOT undo the output gates its own deinit
 * sets -- it only re-powers the PLL -- so we clear the ENET output gate by hand.
 *
 * We deliberately leave TDDR_MCI_FLEXSPI_CLK gated, exactly as MCUboot left it:
 * FlexSPI on this board runs from the TCPU branch, and the proof is empirical --
 * MCUboot itself calls the deinit and keeps XIP-ing happily out of the same
 * flash. Un-gating a clock we do not need, in the one place a mistake costs XIP,
 * buys nothing.
 *
 * The div argument only scales tddr_mci_flexspi_clk, which stays gated;
 * tddr_mci_enet_clk is a fixed 50 MHz (CLOCK_GetTddrMciEnetClkFreq()), so the
 * value is immaterial here. Passing the enum's 0 entry keeps it honest.
 *
 * PRE_KERNEL_1 puts this ahead of the ENET MAC and the PTP clock driver
 * (POST_KERNEL / ETH_INIT_PRIORITY), which is the whole requirement.
 *
 * This is an upstream Zephyr defect, not a local design choice: soc.c's ENET
 * branch should re-init the TDDR ref clock instead of assuming the ROM's state.
 * If that is ever fixed upstream, this file becomes a harmless no-op (the
 * SDK's init re-deinits first when the PLL is already powered) and can go.
 */
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <fsl_clock.h>
#include <fsl_device_registers.h>

static int box_soc_rw612_restore_tddr(void)
{
	/* Only act if the PLL really is powered down -- i.e. a bootloader shut
	 * it off. Booting straight from ROM leaves PDB set and we do nothing,
	 * so the non-MCUboot image keeps its long-validated clock path. */
	if ((SYSCTL2->PLL_CTRL & SYSCTL2_PLL_CTRL_TDDR_PDB_MASK) != 0U) {
		return 0;
	}

	CLOCK_InitTddrRefClk(kCLOCK_TddrFlexspiDiv11);

	/* Un-gate tddr_mci_enet_clk (the 1588 timer clock). The deinit gated
	 * this; the init does not clear it. */
	SYSCTL2->SOURCE_CLK_GATE &= ~SYSCTL2_SOURCE_CLK_GATE_TDDR_MCI_ENET_CLK_CG_MASK;

	/* Symmetric partner of the deinit's CLOCK_DisableClock(). */
	CLOCK_EnableClock(kCLOCK_RefClkTddr);

	return 0;
}

SYS_INIT(box_soc_rw612_restore_tddr, PRE_KERNEL_1, 0);
