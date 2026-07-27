/*
 * box_ota_flash.c -- slot1_partition writer. See box_ota_flash.h.
 */
#include "box_ota_flash.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <errno.h>

#define OTA_SLOT_ID  FIXED_PARTITION_ID(slot1_partition)

static const struct flash_area *fa;
static uint32_t sector_size;
static uint32_t align_size;

static uint32_t erase_max_us, prog_max_us, erase_n, prog_n;

/* k_cycle_get_32() is the 1 MHz OS timer here (SYS_CLOCK_HW_CYCLES_PER_SEC), so
 * a raw cycle delta already is microseconds -- but go through the kernel helper
 * rather than assuming, so a board with a different counter still reports us. */
static inline uint32_t us_since(uint32_t c0)
{
	return (uint32_t) k_cyc_to_us_floor32(k_cycle_get_32() - c0);
}

int box_ota_flash_open(void)
{
	if (fa) {
		return 0;
	}

	int rc = flash_area_open(OTA_SLOT_ID, &fa);
	if (rc != 0) {
		fa = NULL;
		return rc;
	}

	/* Erase granularity comes from the page layout, not from an assumption
	 * that it is 4 kB: get it from the device for the slot's own base. */
	struct flash_pages_info info;
	info.size = 0;
	rc = flash_get_page_info_by_offs(flash_area_get_device(fa),
					 fa->fa_off, &info);
	if (rc != 0) {
		flash_area_close(fa);
		fa = NULL;
		return rc;
	}
	sector_size = info.size;
	align_size  = flash_area_align(fa);
	return 0;
}

void box_ota_flash_close(void)
{
	if (fa) {
		flash_area_close(fa);
		fa = NULL;
	}
}

uint32_t box_ota_flash_size(void)   { return fa ? (uint32_t) fa->fa_size : 0u; }
uint32_t box_ota_flash_sector(void) { return sector_size; }
uint32_t box_ota_flash_align(void)  { return align_size; }

int box_ota_flash_erase(uint32_t off)
{
	if (!fa) {
		return -ENODEV;
	}
	if (!sector_size || off >= fa->fa_size) {
		return -EINVAL;
	}

	uint32_t base = off - (off % sector_size);
	if (base + sector_size > fa->fa_size) {
		return -EINVAL;
	}

	uint32_t c0 = k_cycle_get_32();
	int rc = flash_area_erase(fa, base, sector_size);
	uint32_t d = us_since(c0);

	if (d > erase_max_us) {
		erase_max_us = d;
	}
	erase_n++;
	return rc;
}

int box_ota_flash_program(uint32_t off, const uint8_t *data, uint32_t len)
{
	if (!fa) {
		return -ENODEV;
	}
	if (!data || !len || off + len > fa->fa_size) {
		return -EINVAL;
	}

	uint32_t c0 = k_cycle_get_32();
	int rc = flash_area_write(fa, off, data, len);
	uint32_t d = us_since(c0);

	if (d > prog_max_us) {
		prog_max_us = d;
	}
	prog_n++;
	return rc;
}

int box_ota_flash_read(uint32_t off, uint8_t *buf, uint32_t len)
{
	if (!fa) {
		return -ENODEV;
	}
	if (!buf || !len || off + len > fa->fa_size) {
		return -EINVAL;
	}
	return flash_area_read(fa, off, buf, len);
}

void box_ota_flash_stats(uint32_t *e_max, uint32_t *p_max,
			 uint32_t *e_n, uint32_t *p_n)
{
	if (e_max) *e_max = erase_max_us;
	if (p_max) *p_max = prog_max_us;
	if (e_n)   *e_n   = erase_n;
	if (p_n)   *p_n   = prog_n;
}

void box_ota_flash_stats_reset(void)
{
	erase_max_us = prog_max_us = erase_n = prog_n = 0;
}
