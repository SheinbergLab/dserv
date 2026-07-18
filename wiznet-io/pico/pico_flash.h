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
#include "hardware/regs/addressmap.h"   /* XIP_BASE, XIP_QMI_BASE */
#include "hardware/regs/qmi.h"          /* QMI_ATRANS* : slot-safe persist read */
#include "hardware/xip_cache.h"         /* xip_cache_invalidate_all */
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

/* 0 ok; PICO_ERROR_* (negative) from flash_safe_execute, or -100 = blob didn't
 * serialize. The DISTINCT codes matter on the lockout builds (radio targets):
 * -4 NOT_PERMITTED = core 1 never registered as lockout victim, -1 TIMEOUT =
 * core 1 didn't park -- first seen 2026-07-17 ("flash FAIL" on the thingplus
 * handheld; the radio builds are the first hardware to run this path). */
static inline int flash_store_save(const pico_config_t *cfg)
{
    static uint8_t page[FLASH_STORE_BYTES];   /* ~1 KB: static, off the core-0 stack
                                               * (save_service already runs one op at a time) */
    memset(page, 0xFF, sizeof page);
    if (pico_persist_serialize(cfg, page, sizeof page) == 0) return -100;
    flash_store_op_t op = { page };
    int rc = flash_safe_execute(flash_store_do_op, &op, 500);
    return rc == PICO_OK ? 0 : rc;
}

/* Load config from flash. Returns 0 on success (valid blob), -1 otherwise.
 *
 * SLOT-SAFE READ: persist lives OUTSIDE the A/B slots at a high flash offset
 * (FLASH_STORE_OFFSET). When we boot from a partition slot, QMI ATRANS0 maps only
 * the slot's window at 0x10000000, so the naive pointer (XIP_BASE +
 * FLASH_STORE_OFFSET) is past ATRANS0.SIZE and faults (BusFault -> HardFault; J-Link
 * caught this 2026-07-13). So read persist through a SPARE ATRANS window instead:
 * ATRANS1 covers the +4MiB virtual window (0x10400000), unused by our <512KB image,
 * so mapping it to the persist sector reaches persist regardless of which slot we
 * booted (ATRANS0 = the running code's window is left untouched). Identical result
 * on an absolute boot (ATRANS1 is spare there too). The XIP cache is virtually
 * addressed, so it's flushed around the ATRANS change. (Writes go through
 * flash_range_program on the PHYSICAL offset, which is already slot-independent.) */
#define FLASH_PERSIST_WIN_VADDR (XIP_BASE + 0x400000u)   /* ATRANS1's +4MiB window */

/* Post-load diagnostic snapshot: what the persist header LOOKED like at boot,
 * inspectable later via `show` (boot prints predate terminal attach and are
 * discarded). Added 2026-07-17 hunting the Thing Plus (16MB) load failure. */
static uint32_t flash_load_diag_magic;
static uint16_t flash_load_diag_ver, flash_load_diag_len;
static int8_t   flash_load_diag_rc = 99;      /* 99 = load never ran */

static inline int flash_store_load(pico_config_t *cfg)
{
    io_rw_32 *atrans1 = (io_rw_32 *)(uintptr_t)(XIP_QMI_BASE + QMI_ATRANS1_OFFSET);
    uint32_t  saved   = *atrans1;
    uint32_t  sector  = FLASH_STORE_OFFSET / FLASH_SECTOR_SIZE;   /* persist sector (4K units) */
    /* 1-sector aperture: the blob is <=1K and, crucially, persist is the LAST
     * sector -- a 2-sector aperture at BASE=4095 (16MB parts) overruns the
     * 4096-sector translation space (4095+2 > 4096). 1 sector never can. */
    *atrans1 = ((1u << QMI_ATRANS0_SIZE_LSB) & QMI_ATRANS0_SIZE_BITS)
             | (sector & QMI_ATRANS0_BASE_BITS);
    __dmb();
    xip_cache_invalidate_all();
    const uint8_t *win = (const uint8_t *) FLASH_PERSIST_WIN_VADDR;
    memcpy(&flash_load_diag_magic, win, 4);            /* header snapshot for `show` */
    memcpy(&flash_load_diag_ver,   win + 4, 2);
    memcpy(&flash_load_diag_len,   win + 6, 2);
    int rc = pico_persist_deserialize(win, FLASH_STORE_BYTES, cfg);
    *atrans1 = saved;                          /* restore the bootrom/reset mapping */
    __dmb();
    xip_cache_invalidate_all();
    flash_load_diag_rc = (int8_t) rc;
    if (rc != 0)
        printf("persist: load FAILED (magic=%08lx ver=%u len=%u; want magic=%08x ver<=%u len<=%u)\n",
               (unsigned long) flash_load_diag_magic, flash_load_diag_ver, flash_load_diag_len,
               PICO_PERSIST_MAGIC, PICO_PERSIST_VERSION, (unsigned) sizeof(pico_config_t));
    return rc;
}

static inline void flash_store_erase(void)
{
    flash_store_op_t op = { NULL };
    flash_safe_execute(flash_store_do_op, &op, 500);
}

/* ---- generic OTA scratch flash ops (pico_ota.h streams through these) ----
 * Erase one sector / program one page at an ARBITRARY flash offset (unlike
 * flash_store_* which are pinned to FLASH_STORE_OFFSET). MUST run on core 0:
 * under copy_to_ram + PICO_FLASH_ASSUME_CORE1_SAFE, flash_safe_execute called
 * from core 1 returns PICO_ERROR_NOT_PERMITTED (core 0 is the assumed actor),
 * so the app marshals the pico_ota_flash_t hooks here via a core-0 handshake.
 * off must be sector- (erase) / page- (program) aligned. 0 ok, -1 fail. */
typedef struct { uint32_t off; const uint8_t *page; } flash_ota_op_t;
static void flash_ota_erase_do(void *p)
{ const flash_ota_op_t *o = (const flash_ota_op_t *) p; flash_range_erase(o->off, FLASH_SECTOR_SIZE); }
static void flash_ota_program_do(void *p)
{ const flash_ota_op_t *o = (const flash_ota_op_t *) p; flash_range_program(o->off, o->page, FLASH_PAGE_SIZE); }

static inline int flash_ota_erase(uint32_t off)
{
    flash_ota_op_t op = { off, NULL };
    return flash_safe_execute(flash_ota_erase_do, &op, 1000) == PICO_OK ? 0 : -1;
}
static inline int flash_ota_program(uint32_t off, const uint8_t *page)
{
    flash_ota_op_t op = { off, page };
    return flash_safe_execute(flash_ota_program_do, &op, 1000) == PICO_OK ? 0 : -1;
}

#endif /* PICO_FLASH_H_STORE */
