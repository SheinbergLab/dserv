/*
 * box_flash_sd.c -- SD-card settings backend (Teensy 4.1), behind box_flash.h.
 *
 * The Teensy XIP-executes from its only NOR flash, so writing that flash is
 * hazardous; the SD card is a separate device on usdhc1 and sidesteps it
 * entirely. Same box_flash.h contract as the NVS backend (box_flash.c) -- the
 * board's CMake picks exactly one. The blob is stored as a plain file on a FAT
 * card so it's inspectable on any computer.
 *
 * NOTE: needs a card inserted; a box with no card simply has no persistence
 * (box_flash_init returns -1, the box falls back to defaults).
 */
#include "box_flash.h"

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(box_flash_sd, LOG_LEVEL_INF);

#define CFG_PATH  "/SD:/extio.cfg"

static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type        = FS_FATFS,
	.fs_data     = &fat_fs,
	.mnt_point   = "/SD:",
	.storage_dev = (void *)"SD",   /* the disk-name from the devicetree mmc node */
};
static bool mounted;

int box_flash_init(void)
{
	int rc = fs_mount(&mp);
	if (rc != 0) {
		LOG_WRN("SD mount failed (%d) -- card inserted/formatted FAT?", rc);
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
	struct fs_file_t f;
	fs_file_t_init(&f);
	if (fs_open(&f, CFG_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC) != 0) {
		return -1;
	}
	ssize_t w = fs_write(&f, blob, len);
	fs_close(&f);                   /* flushes to the card */
	return (w == (ssize_t) len) ? 0 : -1;
}

int box_flash_load(uint8_t *buf, uint32_t max)
{
	if (!mounted) {
		return -1;
	}
	struct fs_file_t f;
	fs_file_t_init(&f);
	if (fs_open(&f, CFG_PATH, FS_O_READ) != 0) {
		return -1;              /* no file yet == fresh box */
	}
	ssize_t r = fs_read(&f, buf, max);
	fs_close(&f);
	return (r < 0) ? -1 : (int) r;
}
