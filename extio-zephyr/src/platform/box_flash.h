/*
 * box_flash.h -- persistent settings storage (the Zephyr platform backend for
 * box_persist). The Pico used a raw last-flash-sector write (pico_flash.h); the
 * idiomatic Zephyr equivalent is NVS on a `storage_partition`, which handles
 * wear-levelling and the XIP-from-flash hazard (erase/write runs from RAM) for
 * us. The blob is the same versioned+CRC box_persist image, stored under one id.
 *
 * Boards supply the partition: the FRDM-RW612 already has `storage_partition`;
 * the Teensy overlays carve one from the FlexSPI NOR (see the board overlays).
 */
#ifndef BOX_FLASH_H
#define BOX_FLASH_H

#include <stdint.h>

/* Mount the settings store. 0 on success, negative if flash/NVS isn't ready. */
int box_flash_init(void);

/* Persist a serialized config blob. 0 on success. */
int box_flash_save(const uint8_t *blob, uint32_t len);

/* Last mount/flash errno (0 = ok). Report this rather than a bare -1: the
 * FRDM-RW612 failed with -EDEADLK ("region is not an NVS"), which a plain -1
 * hid twice and sent the hunt toward the FlexSPI driver instead. */
int box_flash_last_error(void);

/* NVS geometry actually in force (not what the devicetree says). */
void box_flash_geometry(uint32_t *sector_size, uint32_t *sector_count);

/* Raw inputs to the mount, for diagnosing a geometry failure. */
void box_flash_debug(int *pi_rc, uint32_t *pi_size, uint32_t *offset);

/* Erase probe, run ONLY when the mount failed: 8 bytes at the partition offset
 * before and after an explicit one-sector erase, plus the three return codes
 * (rd/rd2/er; 1 = the probe did not run). All-0xFF `after` means the erase took
 * and NVS's readback verify is the liar; `after` == `before` means the erase is
 * a silent no-op -- chip, LUT, or block-protect bits. The two need opposite
 * fixes, and -ENXIO alone cannot tell them apart. */
void box_flash_probe(const uint8_t **before, const uint8_t **after,
                     int *rd, int *rd2, int *er);

/* Full-sector erase scan, also mount-failure only. For each of `n` sectors:
 * er[] = flash_erase rc, bad[] = first offset that is NOT the erase value
 * (-1 = the whole sector erased clean, -2 = read error), val[] = the byte
 * found there. This is the question NVS actually asks -- nvs_flash_cmp_const()
 * compares the entire sector, so a PARTIAL erase passes a few-byte check and
 * still fails the mount. */
void box_flash_sector_scan(uint32_t *n, const int32_t **bad,
                           const uint8_t **val, const int **er);

/* Load the stored blob into buf (<= max). Returns byte count, or -1 if none/err. */
int box_flash_load(uint8_t *buf, uint32_t max);
/* Delete the saved config so the next boot uses the compiled defaults. NOT the
 * same as saving a zeroed blob -- see the definition. Returns 0 also when
 * there was nothing saved. */
int box_flash_clear(void);

/* A SECOND, small record, stored independently of the config blob: the OTA/boot
 * breadcrumb (box_boot.h). Same store, separate key.
 *
 * Separate on purpose. The config blob is written by `cmd/save` -- an operator
 * action carrying whatever the live config happens to be -- while this record is
 * written by the OTA state machine. Sharing one key would mean arming an update
 * silently persists unsaved config edits, and an operator `save` clobbers OTA
 * bookkeeping. Same reason the two have different names here rather than one
 * "save everything" call. */
int box_flash_save_boot(const uint8_t *blob, uint32_t len);
int box_flash_load_boot(uint8_t *buf, uint32_t max);

#endif /* BOX_FLASH_H */
