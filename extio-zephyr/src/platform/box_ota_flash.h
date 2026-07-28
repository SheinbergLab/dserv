/*
 * box_ota_flash.h -- write access to the INACTIVE MCUboot slot (slot1_partition).
 *
 * OTA step 2 (see wiznet-io/OTA.md + PORTING.md "MCUboot on the RW612"). This is
 * the platform half only: erase/program/read one region, plus the timing counters
 * the execute-while-write question needs. The portable receive logic -- page
 * buffering, sha256, the STAGING/VERIFY/DONE state machine -- is
 * wiznet-io/pico/pico_ota.h and gets ported separately; it reaches flash through
 * exactly this shape of erase/program pair, which is why the signatures match it.
 *
 * ALL OFFSETS ARE RELATIVE TO THE SLOT, not to the flash device and not to the
 * XIP address. flash_area_* works in area-relative offsets, so the caller never
 * has to know slot1 lives at 0x320000 -- and cannot accidentally write slot0.
 * (box_flash.c documents the sibling trap: DT_REG_ADDR on these
 * `zephyr,mapped-partition` nodes yields the memory-mapped XIP address, which the
 * flash API rejects. Using FIXED_PARTITION_ID + flash_area_* sidesteps it.)
 *
 * The RW612 XIPs from the same FlexSPI device these writes land on, so every
 * erase/program here suspends instruction fetch. CONFIG_FLASH_MCUX_FLEXSPI_XIP
 * relocates the driver into RAM to make that survivable, but the stall is real
 * and lands on the single service loop -- hence box_ota_flash_stats().
 */
#ifndef BOX_OTA_FLASH_H
#define BOX_OTA_FLASH_H

#include <stdint.h>

/* Open/close the slot1 flash area. Safe to call open() repeatedly. */
int  box_ota_flash_open(void);
void box_ota_flash_close(void);

/* Geometry, valid after a successful open(). */
uint32_t box_ota_flash_size(void);     /* slot capacity in bytes            */
uint32_t box_ota_flash_sector(void);   /* erase granularity                 */
uint32_t box_ota_flash_align(void);    /* required program alignment        */

/* Erase the sector containing `off`. Returns 0 or a negative errno. */
int box_ota_flash_erase(uint32_t off);

/* Program `len` bytes at `off`. The span must already be erased. */
int box_ota_flash_program(uint32_t off, const uint8_t *data, uint32_t len);

/* Read back, for verification. */
int box_ota_flash_read(uint32_t off, uint8_t *buf, uint32_t len);

/* Worst-case single-operation stall, in microseconds, since the last reset --
 * the number that decides whether an OTA can run alongside anything real-time.
 * Measured with k_cycle_get_32(), which on this SoC is the 1 MHz OS timer
 * counter and keeps advancing even when the driver has interrupts masked, so it
 * reports true wall time rather than tick time. */
void box_ota_flash_stats(uint32_t *erase_max_us, uint32_t *prog_max_us,
			 uint32_t *erase_n, uint32_t *prog_n);
void box_ota_flash_stats_reset(void);

#endif /* BOX_OTA_FLASH_H */
