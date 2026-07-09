/*
 * pico_flash.h -- RP2350 flash storage for the config blob. Device-side backend
 * for pico_persist.h (the sim uses a file instead). Reserves the last 4 KB
 * sector of flash; the blob (PICO_PERSIST_BLOB_MAX, ~1 KB since v13's
 * labels/groups) is stored in whole 256-byte pages.
 *
 * DUAL-CORE: flash_range_erase/program require that nothing executes from XIP
 * during the op, so they go through flash_safe_execute(). Two build flavors:
 *   - copy_to_ram + PICO_FLASH_ASSUME_CORE1_SAFE (wired/usb/dual): the whole
 *     image runs from SRAM and core 1 never touches flash, so the op only
 *     disables IRQs on core 0 -- core 1 keeps capturing DI edges through the
 *     save. (flash_store_load's XIP read happens once, at boot, on core 0.)
 *   - pico2w (image too big for RAM): classic lockout -- core 1 registers as
 *     victim (flash_safe_execute_core_init) and parks ~ms per op.
 * Save/erase are called ONLY from core 0, after core 1 is up (save queue).
 *
 * Requires linking `hardware_flash` + `pico_flash` (see CMakeLists.txt).
 */
#ifndef PICO_FLASH_H_STORE
#define PICO_FLASH_H_STORE

#include "pico_persist.h"
#include "hardware/flash.h"
#include "pico/flash.h"                 /* flash_safe_execute */
#include "hardware/regs/addressmap.h"   /* XIP_BASE */
#include <string.h>

/* last sector of flash */
#define FLASH_STORE_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
/* store region: the persist blob rounded UP to whole flash pages (a
 * flash_range_program requirement). Derived from the struct, not hardcoded --
 * the old fixed 2-page region silently under-sized once v13's labels/groups
 * grew the blob to ~952 B, so pico_persist_serialize() returned 0 and every
 * `save` printed `flash FAIL`. */
#define FLASH_STORE_BYTES \
    (((PICO_PERSIST_BLOB_MAX + FLASH_PAGE_SIZE - 1u) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE)
_Static_assert(FLASH_STORE_BYTES <= FLASH_SECTOR_SIZE,
               "config blob outgrew the reserved flash sector");

typedef struct { const uint8_t *page; } flash_store_op_t;   /* page NULL => erase only */

static void flash_store_do_op(void *p)      /* runs IRQ-off, other core locked out */
{
    const flash_store_op_t *op = (const flash_store_op_t *) p;
    flash_range_erase(FLASH_STORE_OFFSET, FLASH_SECTOR_SIZE);
    if (op->page) flash_range_program(FLASH_STORE_OFFSET, op->page, FLASH_STORE_BYTES);
}

static inline int flash_store_save(const pico_config_t *cfg)
{
    static uint8_t page[FLASH_STORE_BYTES];   /* ~1 KB: static, off the core-0 stack
                                               * (save_service already runs one op at a time) */
    memset(page, 0xFF, sizeof page);
    if (pico_persist_serialize(cfg, page, sizeof page) == 0) return -1;
    flash_store_op_t op = { page };
    return flash_safe_execute(flash_store_do_op, &op, 500) == PICO_OK ? 0 : -1;
}

/* Load config from flash. Returns 0 on success (valid blob), -1 otherwise. */
static inline int flash_store_load(pico_config_t *cfg)
{
    const uint8_t *p = (const uint8_t *)(XIP_BASE + FLASH_STORE_OFFSET);
    return pico_persist_deserialize(p, FLASH_STORE_BYTES, cfg);
}

static inline void flash_store_erase(void)
{
    flash_store_op_t op = { NULL };
    flash_safe_execute(flash_store_do_op, &op, 500);
}

#endif /* PICO_FLASH_H_STORE */
