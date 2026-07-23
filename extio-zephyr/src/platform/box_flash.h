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

/* Load the stored blob into buf (<= max). Returns byte count, or -1 if none/err. */
int box_flash_load(uint8_t *buf, uint32_t max);

#endif /* BOX_FLASH_H */
