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

/* Where an image must START within the slot, in bytes.
 *
 * ZERO for most swap modes, but ONE SECTOR for swap-using-offset -- which is
 * what this board resolves to, having no scratch partition. Zephyr states the
 * rule as SWAP_USING_OFFSET_SECTOR_UPDATE_BEGIN = 1: "Sector at which firmware
 * update should be placed by application in swap using offset mode." The first
 * sector is working space for the swap itself.
 *
 * Getting this wrong is INVISIBLE to a byte-level check: the image writes and
 * reads back perfectly and MCUboot simply never finds it, because it is looking
 * one sector further in. OTA step 3 verified the bytes and explicitly did NOT
 * verify bootability, which is exactly the gap this closes. */
uint32_t box_ota_flash_image_base(void);

/* Geometry, valid after a successful open(). */
uint32_t box_ota_flash_size(void);     /* slot capacity in bytes            */
uint32_t box_ota_flash_sector(void);   /* erase granularity                 */
uint32_t box_ota_flash_align(void);    /* required program alignment        */

/* Erase the MCUboot image trailer at the END of the slot, so its magic reads
 * UNSET.
 *
 * Required before arming. boot_set_next() switches on the trailer magic: UNSET
 * means "write it and proceed", GOOD means "proceed", and ANYTHING ELSE falls
 * through to an error -- which surfaces from Zephyr as a bare -EFAULT from
 * boot_request_upgrade() with no hint at the cause.
 *
 * The trailer lives at the end of the slot and our OTA never touches it: erase
 * happens as-you-go over the sectors the IMAGE occupies, which are all at the
 * start. So a slot whose tail holds factory content reports magic=bad forever,
 * and every arm fails while the image itself verifies perfectly. That is exactly
 * what the very first MCUboot boot log on this board said, and it took an arm
 * failure to notice:
 *
 *   I: Secondary image: magic=bad, swap_type=0x2, copy_done=0x2, image_ok=0x2
 *
 * Erases only from the trailer status offset onward, not the whole 3 MB slot --
 * boot_erase_img_bank() would be ~46 s of blocking erase here. */
int box_ota_flash_clear_trailer(void);

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
