/*
 * pico_flash.h -- RP2350 flash storage for the config blob. Device-side backend
 * for pico_persist.h (the sim uses a file instead). Reserves the last 4 KB
 * sector of flash; the ~180-byte blob fits in one 256-byte page.
 *
 * flash_range_erase/program stall XIP (all execution) for the duration, so we
 * disable interrupts around them. Single-core here; on multicore you'd also
 * pause the other core (flash_safe_execute / multicore_lockout). Incoming TCP
 * bytes buffer in the W6300's socket RAM during the brief write, so none lost.
 *
 * Requires linking `hardware_flash` (see CMakeLists.txt).
 */
#ifndef PICO_FLASH_H_STORE
#define PICO_FLASH_H_STORE

#include "pico_persist.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"   /* XIP_BASE */
#include <string.h>

/* last sector of flash */
#define FLASH_STORE_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
/* store region = 2 flash pages: the config blob outgrew one 256B page once WiFi
 * creds were added. Must stay a multiple of FLASH_PAGE_SIZE for flash_range_program. */
#define FLASH_STORE_BYTES (FLASH_PAGE_SIZE * 2)

static inline int flash_store_save(const pico_config_t *cfg)
{
    uint8_t page[FLASH_STORE_BYTES];
    memset(page, 0xFF, sizeof page);
    if (pico_persist_serialize(cfg, page, sizeof page) == 0) return -1;

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_STORE_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_STORE_OFFSET, page, FLASH_STORE_BYTES);
    restore_interrupts(ints);
    return 0;
}

/* Load config from flash. Returns 0 on success (valid blob), -1 otherwise. */
static inline int flash_store_load(pico_config_t *cfg)
{
    const uint8_t *p = (const uint8_t *)(XIP_BASE + FLASH_STORE_OFFSET);
    return pico_persist_deserialize(p, FLASH_STORE_BYTES, cfg);
}

static inline void flash_store_erase(void)
{
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_STORE_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}

#endif /* PICO_FLASH_H_STORE */
