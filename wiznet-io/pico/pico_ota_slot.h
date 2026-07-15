/*
 * pico_ota_slot.h -- RP2350 bootrom A/B partition + boot-info probe (Stage 1).
 *
 * READ-ONLY so far: asks the bootrom what the current flash partitioning + boot
 * looks like, and where a new image would land (= the inactive slot, the future
 * OTA write target). Writes NOTHING. This is the Stage-1 foundation: the same
 * `target` it resolves is what `pico_ota_begin` will be pointed at once we
 * partition, and the boot-info is what the buy/self-test SM reads to know it's
 * on a TBYB trial.
 *
 * Bootrom mechanics (all verified in pico-sdk):
 *   rom_get_boot_info          -> boot_type / booted partition / TBYB+update flags
 *   rom_load_partition_table   -> populate the resident PT into a workarea
 *   rom_get_partition_table_info(PT_INFO) -> partition count
 *   rom_get_uf2_target_partition(RP2350_ARM_S) -> which partition a new ARM-S
 *                                 image lands in == the inactive slot for OTA
 * The workarea must be >= 3264 bytes (SDK) and word-aligned.
 *
 * On an UNPARTITIONED card (today): pt_count==0, no target -> reports "no PT",
 * which is exactly the baseline we expect before the one-time migration.
 */
#ifndef PICO_OTA_SLOT_H
#define PICO_OTA_SLOT_H

#include "pico/bootrom.h"
#include "boot/bootrom_constants.h"
#include "boot/picobin.h"          /* PICOBIN_PARTITION_LOCATION_* */
#include "boot/picoboot_constants.h" /* REBOOT2_FLAG_* : TBYB flash-update reboot */
#include <stdint.h>
#include <string.h>

/* From boot/uf2.h (not on the include path here); the UF2 family for a signed
 * RP2350 Arm-S image -- the one our build emits, so the slot the bootrom would
 * place it in is our OTA write target. Architecturally fixed. */
#ifndef RP2350_ARM_S_FAMILY_ID
#define RP2350_ARM_S_FAMILY_ID 0xe48bff59u
#endif

typedef struct {
    int8_t   pt_loaded;      /* rom_load_partition_table rc >= 0                 */
    int8_t   pt_count;       /* partitions in the table (0 = unpartitioned card) */
    int8_t   boot_type;      /* BOOT_TYPE_{NORMAL,BOOTSEL,RAM_IMAGE,FLASH_UPDATE}*/
    int8_t   boot_partition; /* partition we booted from (-1 if none / no PT)    */
    uint8_t  tbyb_flags;     /* boot_info.tbyb_and_update_info (BUY_PENDING etc.)*/
    int8_t   target_valid;   /* a new ARM-S image has a resolved home slot       */
    int8_t   target_part;    /* that partition number (the inactive slot)        */
    uint32_t target_base;    /* its storage byte offset (flash-relative)         */
    uint32_t target_size;    /* its size in bytes                                */
    int32_t  last_rc;        /* rc of the last ROM call that failed (diagnostics)*/
    uint32_t boot_diag;      /* boot_info.boot_diagnostic: per-slot search/verify/
                              * launch bits (lo halfword = slot0/partA, hi = slot1/
                              * partB). WHY a slot boot did/didn't launch. */
} pico_ota_slot_t;

/* 3.25K+ word-aligned workarea for the PT-loading ROM calls (SDK min 3264). */
static uint8_t pico_ota_workarea[4096] __attribute__((aligned(4)));

#define PICO_OTA_PT_SECTOR 4096u   /* PT locations are in 4K sectors (arch fixed) */
static inline void pico_ota_slot_decode_loc(uint32_t perms_and_loc,
                                            uint32_t *base, uint32_t *size)
{
    uint32_t first = (perms_and_loc & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS)
                     >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    uint32_t last  = (perms_and_loc & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS)
                     >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
    *base = first * PICO_OTA_PT_SECTOR;                 /* sectors are 4K        */
    *size = (last - first + 1u) * PICO_OTA_PT_SECTOR;   /* inclusive range       */
}

static inline void pico_ota_slot_probe(pico_ota_slot_t *s)
{
    memset(s, 0, sizeof *s);
    s->boot_partition = -1;
    s->target_part    = -1;

    boot_info_t bi;
    if (rom_get_boot_info(&bi)) {
        s->boot_type      = (int8_t) bi.boot_type;
        s->boot_partition = (int8_t) bi.partition;
        s->tbyb_flags     = bi.tbyb_and_update_info;
        s->boot_diag      = bi.boot_diagnostic;
    }

    int rc = rom_load_partition_table(pico_ota_workarea, sizeof pico_ota_workarea, false);
    s->pt_loaded = rc >= 0;
    s->last_rc   = rc;
    if (!s->pt_loaded) return;                    /* no PT (today) -> baseline done */

    uint32_t out[4];                              /* PT_INFO: {supported_flags, count, ...} */
    int w = rom_get_partition_table_info(out, 4, PT_INFO_PT_INFO);
    if (w >= 2 && (out[0] & PT_INFO_PT_INFO)) s->pt_count = (int8_t) out[1];
    else s->last_rc = w;

    /* Where would a new ARM-S image go? That's the inactive slot -- the future
     * OTA write target. Errors (no PT / not secure) leave target_valid = 0. */
    resident_partition_t tp;
    int t = rom_get_uf2_target_partition(pico_ota_workarea, sizeof pico_ota_workarea,
                                         RP2350_ARM_S_FAMILY_ID, &tp);
    if (t >= 0) {
        s->target_valid = 1;
        s->target_part  = (int8_t) t;
        pico_ota_slot_decode_loc(tp.permissions_and_location, &s->target_base, &s->target_size);
    } else if (s->last_rc >= 0) {
        s->last_rc = t;
    }
}

static inline const char *pico_ota_boot_type_str(int bt)
{
    switch (bt) {
        case 0x0: return "normal";
        case 0x2: return "bootsel";
        case 0x3: return "ram_image";
        case 0x4: return "flash_update";
        case 0xd: return "pc_sp";
        default:  return "?";
    }
}

/* boot_info_t.partition is a SIGNED sentinel for a slot boot (BOOT_PARTITION_*),
 * NOT an index: -1 none, -2 slot0, -3 slot1, -4 window; >=0 = a real PT partition
 * index. Decode for readable telemetry (raw "-2" reads as junk). */
static inline const char *pico_ota_boot_part_str(int p)
{
    switch (p) {
        case BOOT_PARTITION_NONE:   return "none";
        case BOOT_PARTITION_SLOT0:  return "slot0";
        case BOOT_PARTITION_SLOT1:  return "slot1";
        case BOOT_PARTITION_WINDOW: return "window";
        default:                    return "part";   /* >=0: a PT partition index (caller appends it) */
    }
}

/* Decode one halfword of boot_diagnostic into compact tokens (which stages the
 * bootrom reached for that slot). The telling pattern for our wedge: CHOSEN +
 * idOK (verified) but NO launched -> chosen+verified but failed to execute (the
 * copy_to_ram-from-slot problem); condfail -> image condition (e.g. version)
 * rejected it. Writes at most sz-1 chars. */
static inline void pico_ota_bootdiag_str(uint16_t d, char *buf, int sz)
{
    struct { uint16_t bit; const char *tok; } m[] = {  /* boot order; short tokens */
        { BOOT_DIAGNOSTIC_WINDOW_SEARCHED,               "srch"   },
        { BOOT_DIAGNOSTIC_VALID_BLOCK_LOOP,              "loop"   },
        { BOOT_DIAGNOSTIC_INVALID_BLOCK_LOOP,            "badloop"},
        { BOOT_DIAGNOSTIC_VALID_IMAGE_DEF,               "imgdef" },
        { BOOT_DIAGNOSTIC_HAS_PARTITION_TABLE,           "pt"     },
        { BOOT_DIAGNOSTIC_CONSIDERED,                    "cons"   },
        { BOOT_DIAGNOSTIC_CHOSEN,                        "CHOSEN" },
        { BOOT_DIAGNOSTIC_IMAGE_DEF_VERIFIED_OK,         "ok"     },
        { BOOT_DIAGNOSTIC_LOAD_MAP_ENTRIES_LOADED,       "ldmap"  },
        { BOOT_DIAGNOSTIC_IMAGE_LAUNCHED,                "RUN"    },
        { BOOT_DIAGNOSTIC_IMAGE_CONDITION_FAILURE,       "FAIL"   },
    };
    int n = 0;
    if (sz > 0) buf[0] = '\0';
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++)
        if (d & m[i].bit) {
            int w = snprintf(buf + n, (size_t)(sz - n), "%s%s", n ? "," : "", m[i].tok);
            if (w <= 0 || w >= sz - n) break;
            n += w;
        }
    if (n == 0 && sz > 1) snprintf(buf, (size_t) sz, "%s", d ? "?" : "-");
}

/* BUY_PENDING bit in tbyb_and_update_info -> we're on a TBYB trial that must buy. */
#ifndef BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING
#define BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING 0x1
#endif
static inline int pico_ota_buy_pending(const pico_ota_slot_t *s)
{ return (s->tbyb_flags & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING) ? 1 : 0; }

/* ---- TBYB lifecycle (try-before-you-buy + rollback) ----------------------
 * The official pico-examples ota_update writes the inactive slot then
 * rom_reboot(FLASH_UPDATE, base) and buys IMMEDIATELY -- no self-test, no
 * rollback. We instead GATE the buy behind a self-test so a wedged image
 * (never buys) is auto-reverted by the bootrom on the next reset (our Stage-3
 * watchdog forces that reset). TBYB here is driven by the FLASH_UPDATE reboot,
 * NOT an image flag (picotool seal has no --tbyb). */

/* We're on a TRIAL boot: booted via FLASH_UPDATE and a buy is still pending.
 * If we don't pico_ota_buy() before the next reset, the bootrom reverts. */
static inline int pico_ota_boot_is_trial(const boot_info_t *bi)
{ return bi->boot_type == BOOT_TYPE_FLASH_UPDATE
      && (bi->tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING); }

/* Arm an update: reboot into the inactive slot as a flash-update (buy-pending)
 * boot. update_base = that slot's storage offset (pico_ota_slot_t.target_base).
 * NO_RETURN_ON_SUCCESS: does not return; the box reboots after delay_ms. */
static inline int pico_ota_arm_update(uint32_t update_base, uint32_t delay_ms)
{
    return rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS,
                      delay_ms, update_base, 0);
}

/* Commit the trial image: clears buy-pending, makes this slot permanent. Uses
 * the workarea; rom_explicit_buy is internally flash_safe_execute'd, so call it
 * from the flash-owning core (core 0 here). 0 = committed. */
static inline int pico_ota_buy(void)
{ return rom_explicit_buy(pico_ota_workarea, sizeof pico_ota_workarea); }

#endif /* PICO_OTA_SLOT_H */
