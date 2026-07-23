/*
 * box_flash.c -- NVS-backed settings store on the `storage_partition`.
 */
#include "box_flash.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>

#define STORAGE_NODE  DT_NODELABEL(storage_partition)
#define BOX_CFG_ID    1u          /* one NVS entry: the whole box_persist blob */

static struct nvs_fs fs;
static bool mounted;

int box_flash_init(void)
{
	fs.flash_device = DEVICE_DT_GET(DT_MTD_FROM_FIXED_PARTITION(STORAGE_NODE));
	if (!device_is_ready(fs.flash_device)) {
		return -1;
	}
	fs.offset = DT_REG_ADDR(STORAGE_NODE);

	struct flash_pages_info info;
	if (flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info) != 0) {
		return -1;
	}
	fs.sector_size  = info.size;                                   /* one erase block */
	fs.sector_count = DT_REG_SIZE(STORAGE_NODE) / info.size;       /* fill the partition */

	if (nvs_mount(&fs) != 0) {
		return -1;
	}
	mounted = true;
	return 0;
}

int box_flash_save(const uint8_t *blob, uint32_t len)
{
	if (!mounted) {
		return -1;
	}
	ssize_t r = nvs_write(&fs, BOX_CFG_ID, blob, len);
	return (r < 0) ? -1 : 0;   /* r == 0 means "same as stored" -- also success */
}

int box_flash_load(uint8_t *buf, uint32_t max)
{
	if (!mounted) {
		return -1;
	}
	ssize_t r = nvs_read(&fs, BOX_CFG_ID, buf, max);
	return (r < 0) ? -1 : (int) r;
}
