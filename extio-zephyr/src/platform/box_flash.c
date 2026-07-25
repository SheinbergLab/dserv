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

int box_flash_load(uint8_t *buf, uint32_t max)
{
	if (!mounted) {
		return -1;
	}
	ssize_t r = nvs_read(&fs, BOX_CFG_ID, buf, max);
	return (r < 0) ? -1 : (int) r;
}
