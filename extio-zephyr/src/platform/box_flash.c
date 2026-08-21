/*
 * box_flash.c -- NVS-backed settings store on the `storage_partition`.
 */
#include "box_flash.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>
#include <errno.h>

#define STORAGE_NODE   DT_NODELABEL(storage_partition)
#define STORAGE_LABEL  storage_partition   /* PARTITION_* take the LABEL token */
#define BOX_CFG_ID    1u          /* one NVS entry: the whole box_persist blob */
#define BOX_BOOT_ID   2u          /* ...and the OTA/boot breadcrumb (box_boot.h) */

/* DO NOT "fill the partition". We store ONE blob of ~1 kB, and the cost of
 * extra sectors is not zero:
 *   - nvs_mount() scans every sector at boot;
 *   - with CONFIG_NVS_INIT_BAD_MEMORY_REGION, a region NVS does not recognise
 *     is erased WHOLE. The FRDM-RW612's storage_partition is 58 MB = 14816
 *     sectors of 4 kB; at ~45 ms per sector erase that is ~11 MINUTES, which
 *     presents as `save FAILED` / a wedged boot rather than as a size problem.
 * A handful of sectors gives ample wear-levelling headroom for a 1 kB blob.
 */
#define BOX_NVS_SECTORS  8u

static struct nvs_fs fs;
static bool mounted;
static int  mount_err;            /* last nvs/flash errno, for reporting */
static int  dbg_pi_rc;            /* flash_get_page_info_by_offs() rc */
static uint32_t dbg_pi_size;      /* ...and the page size it reported */

/* Mount-failure erase probe (see box_flash_init) -- valid only when the mount
 * failed; zeroed otherwise. */
static uint8_t dbg_probe_before[8], dbg_probe_after[8];
static int dbg_probe_rd = 1, dbg_probe_rd2 = 1, dbg_probe_er = 1;  /* 1 = not run */

/* Per-sector full-sector erase scan. bad[] is the first non-erased offset in
 * that sector, -1 = fully erased ("clean"), -2 = read error. */
static int32_t dbg_sect_bad[8];
static uint8_t dbg_sect_val[8];
static int     dbg_sect_er[8];
static uint32_t dbg_sect_n;

void box_flash_sector_scan(uint32_t *n, const int32_t **bad,
                           const uint8_t **val, const int **er)
{
	if (n)   *n   = dbg_sect_n;
	if (bad) *bad = dbg_sect_bad;
	if (val) *val = dbg_sect_val;
	if (er)  *er  = dbg_sect_er;
}

void box_flash_probe(const uint8_t **before, const uint8_t **after,
                     int *rd, int *rd2, int *er)
{
	if (before) *before = dbg_probe_before;
	if (after)  *after  = dbg_probe_after;
	if (rd)     *rd     = dbg_probe_rd;
	if (rd2)    *rd2    = dbg_probe_rd2;
	if (er)     *er     = dbg_probe_er;
}

int box_flash_last_error(void) { return mount_err; }

void box_flash_geometry(uint32_t *sector_size, uint32_t *sector_count)
{
	if (sector_size)  *sector_size  = fs.sector_size;
	if (sector_count) *sector_count = fs.sector_count;
}

void box_flash_debug(int *pi_rc, uint32_t *pi_size, uint32_t *offset)
{
	if (pi_rc)   *pi_rc   = dbg_pi_rc;
	if (pi_size) *pi_size = dbg_pi_size;
	if (offset)  *offset  = (uint32_t) fs.offset;
}

int box_flash_init(void)
{
	fs.flash_device = PARTITION_DEVICE(STORAGE_LABEL);
	if (!device_is_ready(fs.flash_device)) {
		mount_err = -ENODEV;
		return mount_err;
	}
	/* FIXED_PARTITION_OFFSET, *not* DT_REG_ADDR. The FRDM-RW612 declares its
	 * partitions `compatible = "zephyr,mapped-partition"`, so DT_REG_ADDR
	 * resolves through the FlexSPI controller's `ranges` and yields the
	 * MEMORY-MAPPED XIP address (0x18620000), while the flash API wants an
	 * offset WITHIN the device (0x620000). Passing the mapped address makes
	 * flash_get_page_info_by_offs() return -EINVAL, which surfaced only as
	 * `save FAILED`. */
	fs.offset = PARTITION_OFFSET(STORAGE_LABEL);

	struct flash_pages_info info;
	info.size = 0;
	int rc = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);
	dbg_pi_rc = rc;
	dbg_pi_size = info.size;
	if (rc != 0) {
		mount_err = rc;
		return rc;
	}
	fs.sector_size = info.size;                              /* one erase block */

	uint32_t avail = PARTITION_SIZE(STORAGE_LABEL) / info.size;
	fs.sector_count = (uint16_t) MIN(avail, BOX_NVS_SECTORS);

	rc = nvs_mount(&fs);
	if (rc != 0) {
		mount_err = rc;                  /* -EDEADLK = unrecognised region */
		/* ERASE PROBE -- runs ONLY on a failed mount, and it exists to split
		 * the one ambiguity -ENXIO leaves.
		 *
		 * -ENXIO out of nvs_mount comes from nvs_flash_erase_sector(): it
		 * erases, reads back, and returns -ENXIO when the readback is not the
		 * erase value. Two very different faults produce that, and they need
		 * opposite fixes:
		 *
		 *   erase never happened   -> chip/LUT/protection (wrong part, or the
		 *                             status-register block-protect bits are
		 *                             set over this region)
		 *   erase happened, read   -> coherence: the FlexSPI AHB buffers or the
		 *   came back stale           D-cache are serving pre-erase bytes
		 *
		 * flash_flexspi_nor_erase() returns 0 unconditionally -- it discards
		 * the result of every erase_sector it issues -- so the driver cannot
		 * tell us which. Read 8 bytes, erase that one sector, read the same 8
		 * bytes again, print both. All-0xFF after means the erase DID take and
		 * NVS's verify is the liar; unchanged bytes mean the erase is a no-op.
		 *
		 * Safe: the partition failed to mount, so it holds nothing we can lose,
		 * and this touches only the first sector of storage_partition.
		 *
		 * Reported through box_flash_probe() rather than printk, for the same
		 * reason the mount errno is: this board's Zephyr console is not the
		 * console a person is reading. The box CDC is, and main() owns it. */
		dbg_probe_rd  = flash_read(fs.flash_device, fs.offset,
		                           dbg_probe_before, sizeof dbg_probe_before);
		dbg_probe_er  = flash_erase(fs.flash_device, fs.offset, fs.sector_size);
		dbg_probe_rd2 = flash_read(fs.flash_device, fs.offset,
		                           dbg_probe_after, sizeof dbg_probe_after);

		/* SCAN THE WHOLE SECTOR, because 8 bytes is not the question NVS asks.
		 *
		 * The first cut of this probe read 8 bytes either side of the erase,
		 * saw ff ff ff ff ff ff ff ff, and concluded the erase was fine --
		 * while nvs_flash_erase_sector() was still returning -ENXIO for the
		 * same sector. nvs_flash_cmp_const() compares ALL sector_size bytes.
		 * A partial erase -- one page cleared, the rest of the sector left
		 * dirty -- passes an 8-byte check and fails NVS's, and every board
		 * where this works would look identical at 8 bytes.
		 *
		 * So: erase each sector in turn and report the FIRST offset that is
		 * not the erase value. `clean` on every sector means the erase really
		 * is complete and the fault is in NVS's read path; a first_bad offset
		 * names the granularity that actually erased. */
		for (uint32_t s = 0; s < fs.sector_count && s < 8; s++) {
			off_t base = fs.offset + (off_t) s * fs.sector_size;
			uint8_t buf[64];
			int er = flash_erase(fs.flash_device, base, fs.sector_size);

			dbg_sect_er[s] = er;
			dbg_sect_bad[s] = -1;          /* -1 = clean */
			dbg_sect_val[s] = 0xff;
			for (uint32_t off = 0; off < fs.sector_size; off += sizeof buf) {
				if (flash_read(fs.flash_device, base + off, buf, sizeof buf)) {
					dbg_sect_bad[s] = -2;  /* -2 = read error */
					break;
				}
				for (uint32_t i = 0; i < sizeof buf; i++) {
					if (buf[i] != 0xff) {
						dbg_sect_bad[s] = (int32_t) (off + i);
						dbg_sect_val[s] = buf[i];
						break;
					}
				}
				if (dbg_sect_bad[s] != -1) {
					break;
				}
			}
		}
		dbg_sect_n = fs.sector_count < 8 ? fs.sector_count : 8;
		return rc;
	}
	mounted = true;
	mount_err = 0;
	return 0;
}

int box_flash_save(const uint8_t *blob, uint32_t len)
{
	if (!mounted) {
		return mount_err ? mount_err : -ENODEV;
	}
	/* Return the REAL errno, not -1. -ENOSPC here means the blob does not fit
	 * the sector geometry, which is a different fix from a mount failure. */
	ssize_t r = nvs_write(&fs, BOX_CFG_ID, blob, len);
	if (r < 0) {
		mount_err = (int) r;
		return (int) r;
	}
	return 0;                  /* r == 0 means "same as stored" -- also success */
}

/* Delete the config record outright, so the next boot finds NOTHING and falls
 * back to the compiled defaults -- a genuine out-of-box state.
 *
 * Deleting is NOT the same as saving a zeroed blob, and the difference is the
 * whole reason this exists. A zeroed blob still LOADS, so it overwrites the
 * defaults main() sets before the mount: console_mode goes to 0
 * (= CONSOLE_MODE_CDC) and the console silently moves off the board UART, and
 * the LED/button demo pins are wiped. A box "factory reset" that way comes
 * back subtly different from one fresh off the reel -- and on a board whose
 * console is the MCU-Link UART, unreachable.
 *
 * The OTA/boot breadcrumb (BOX_BOOT_ID) is deliberately left alone: it records
 * which image is running and whether it is confirmed, which is a fact about
 * the firmware, not user configuration. Wiping it would make a perfectly good
 * box misreport its own OTA state. */
int box_flash_clear(void)
{
	if (!mounted) {
		return mount_err ? mount_err : -ENODEV;
	}
	int rc = nvs_delete(&fs, BOX_CFG_ID);

	/* -ENOENT means there was nothing to delete, which is the state the caller
	 * asked for. Report success rather than making "already factory" an error. */
	return (rc == 0 || rc == -ENOENT) ? 0 : rc;
}

int box_flash_load(uint8_t *buf, uint32_t max)
{
	if (!mounted) {
		return -1;
	}
	ssize_t r = nvs_read(&fs, BOX_CFG_ID, buf, max);
	return (r < 0) ? -1 : (int) r;
}

int box_flash_save_boot(const uint8_t *blob, uint32_t len)
{
	if (!mounted) {
		return mount_err ? mount_err : -ENODEV;
	}
	ssize_t r = nvs_write(&fs, BOX_BOOT_ID, blob, len);

	/* Does NOT touch mount_err: this record is written from the OTA path, and
	 * letting it overwrite the errno the config store reports would blame a
	 * `save` failure on an unrelated write. */
	return (r < 0) ? (int) r : 0;
}

int box_flash_load_boot(uint8_t *buf, uint32_t max)
{
	if (!mounted) {
		return -1;
	}
	ssize_t r = nvs_read(&fs, BOX_BOOT_ID, buf, max);
	return (r < 0) ? -1 : (int) r;
}
