/*
 * box_boot.c -- see box_boot.h.
 */
#include "box_boot.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#endif

#if defined(BOX_HAVE_PERSIST)
#include "box_flash.h"
#endif

/* ---- the persisted breadcrumb ----
 *
 * Deliberately NOT part of the box_persist config blob. That blob is written by
 * `cmd/save`, i.e. by the operator, and carries whatever the live config happens
 * to be -- so arming an update would silently persist unsaved config edits, and
 * an operator's `save` could roll back OTA bookkeeping. Different lifetime,
 * different writer, different record. */
#define BOOT_REC_MAGIC   0x42544f31u   /* "BTO1" */
#define BOOT_REC_VER     1u

struct boot_rec {
	uint32_t magic;
	uint8_t  ver;
	uint8_t  armed;        /* an update was armed and has not resolved   */
	uint8_t  trial_seen;   /* ...and we have since booted it unconfirmed */
	uint8_t  pad;
	uint16_t reverts;
	uint16_t rejects;
	uint16_t updates;
	uint16_t pad2;
	char     arm_ver[16];  /* version we armed, for "what was it becoming?" */
};

static struct boot_rec rec;
static int  rec_valid;                 /* a store exists and was readable */

static const char *reason = "unknown";
static uint32_t    raw_cause;
static char        img_ver[16];
static int         on_trial;

static void rec_load(void)
{
#if defined(BOX_HAVE_PERSIST)
	uint8_t buf[sizeof rec];
	int n = box_flash_load_boot(buf, sizeof buf);

	if (n == (int) sizeof rec) {
		memcpy(&rec, buf, sizeof rec);
		if (rec.magic == BOOT_REC_MAGIC && rec.ver == BOOT_REC_VER) {
			rec_valid = 1;
			return;
		}
	}
	/* Absent or from an older layout: start clean rather than guess. Losing
	 * the lifetime counters once is a fair price for never misreporting a
	 * revert, which is the whole point of the record. */
	memset(&rec, 0, sizeof rec);
	rec.magic = BOOT_REC_MAGIC;
	rec.ver   = BOOT_REC_VER;
	rec_valid = 1;
#endif
}

static int rec_store(void)
{
#if defined(BOX_HAVE_PERSIST)
	if (!rec_valid) {
		return -ENODEV;
	}
	return box_flash_save_boot((const uint8_t *) &rec, sizeof rec);
#else
	return -ENOTSUP;
#endif
}

/* ---- reset cause ---- */

static const char *cause_str(uint32_t c)
{
	/* Order matters: the register ACCUMULATES within a power session, so a
	 * watchdog reset followed by a commanded reboot can present both bits.
	 * Report the most alarming one -- a wedge you have to hear about outranks
	 * a reboot you asked for. (Clearing, below, keeps this rare.) */
	if (c & RESET_WATCHDOG)   return "watchdog";
	if (c & RESET_CPU_LOCKUP) return "lockup";
	if (c & RESET_SECURITY)   return "security";
	if (c & RESET_BROWNOUT)   return "brownout";
	if (c & RESET_DEBUG)      return "debug";
	if (c & RESET_SOFTWARE)   return "software";
	if (c & RESET_PIN)        return "pin";
	if (c & RESET_POR)        return "power";
	/* The RW612 has no power-on bit at all (fsl_power.h power_reset_cause_t
	 * stops at ResetB) and reports RESET_HARDWARE for the external reset line,
	 * which a cold start also drives -- so "hardware" here is not a fault, it
	 * is the ordinary cold boot on that SoC. */
	if (c & RESET_HARDWARE)   return "hardware";
	return c ? "other" : "power";
}

/* ---- MCUboot image headers ---- */

int box_boot_image_ver(int slot, char *buf, uint32_t len)
{
#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
	struct mcuboot_img_header h;
	uint8_t area = (slot == 0) ? FIXED_PARTITION_ID(slot0_partition)
				   : FIXED_PARTITION_ID(slot1_partition);
	/* boot_read_bank_header() applies boot_get_image_start_offset() itself, so
	 * it looks in the second sector for slot1 under swap-using-offset -- the
	 * same rule box_ota_flash_image_base() encodes for writing. Two independent
	 * statements of the same offset, which is why a header read is a real check
	 * on where we put the image and not a tautology. */
	int rc = boot_read_bank_header(area, &h, sizeof h);

	if (rc != 0) {
		return rc;
	}
	if (h.mcuboot_version != 1U) {
		return -ENOTSUP;
	}
	snprintf(buf, len, "%u.%u.%u+%u",
		 h.h.v1.sem_ver.major, h.h.v1.sem_ver.minor,
		 h.h.v1.sem_ver.revision, (unsigned) h.h.v1.sem_ver.build_num);
	return 0;
#else
	ARG_UNUSED(slot);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	return -ENOTSUP;
#endif
}

/* ---- init ---- */

void box_boot_init(void)
{
	if (hwinfo_get_reset_cause(&raw_cause) != 0) {
		raw_cause = 0;
		reason = "unknown";
	} else {
		reason = cause_str(raw_cause);
	}

	/* CLEAR IT. The cause register is sticky: nothing in the SoC resets it
	 * between boots, so an uncleared register reports the union of every reset
	 * since power-on. One watchdog trip would make the box answer "watchdog"
	 * to every boot for the rest of its power cycle -- a field reporting
	 * memory instead of reality, which is the failure shape this tree keeps
	 * meeting (net.ip, sync/source, persist=FAILED). Read once, latch, clear;
	 * every later caller gets the latch. */
	(void) hwinfo_clear_reset_cause();

	rec_load();

#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
	if (box_boot_image_ver(0, img_ver, sizeof img_ver) != 0) {
		/* We are demonstrably running, so this cannot mean "no image" -- it
		 * means the header is not where the tooling thinks. Say so rather
		 * than inventing a version. */
		strcpy(img_ver, "unreadable");
	}
	on_trial = boot_is_img_confirmed() ? 0 : 1;

	if (rec.armed) {
		if (on_trial) {
			/* The armed image is running and unconfirmed: this is the trial.
			 * Record that we SAW it, so the next boot can tell a rollback
			 * apart from an image MCUboot never took. Persisted now, because
			 * the interesting failure from here is a power cut. */
			if (!rec.trial_seen) {
				rec.trial_seen = 1;
				(void) rec_store();
			}
			reason = "trial";
		} else if (rec.trial_seen) {
			/* Armed, ran, and we are back on a confirmed image: the trial was
			 * not kept. Whether that was a wedge, a power cut, or simply
			 * nobody sending cmd/ota/confirm, the outcome is the same and the
			 * fleet needs to see it. */
			rec.armed = 0;
			rec.trial_seen = 0;
			rec.reverts++;
			(void) rec_store();
			reason = "revert";
		} else {
			/* Armed, never ran. MCUboot declined the image: bad signature,
			 * wrong header, or an image written where the bootloader does not
			 * look (step 4's offset bug). Distinct from a revert and much
			 * cheaper to diagnose when it is named. */
			rec.armed = 0;
			rec.rejects++;
			(void) rec_store();
			reason = "rejected";
		}
	}
#endif
}

const char *box_boot_reason(void)       { return reason; }
uint32_t    box_boot_reset_cause(void)  { return raw_cause; }
int         box_boot_on_trial(void)     { return on_trial; }

const char *box_boot_img_ver(void)
{
#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
	return img_ver;
#else
	return NULL;      /* also means "the OTA fields below are meaningless here" */
#endif
}

int box_boot_reverts(void) { return rec_valid ? (int) rec.reverts : -1; }
int box_boot_rejects(void) { return rec_valid ? (int) rec.rejects : -1; }
int box_boot_updates(void) { return rec_valid ? (int) rec.updates : -1; }

const char *box_boot_last_arm_ver(void)
{
	return (rec_valid && rec.arm_ver[0]) ? rec.arm_ver : "none";
}

int box_boot_note_arm(const char *staged_ver)
{
	if (!rec_valid) {
		return -ENODEV;
	}
	rec.armed      = 1;
	rec.trial_seen = 0;
	if (staged_ver) {
		strncpy(rec.arm_ver, staged_ver, sizeof rec.arm_ver - 1);
		rec.arm_ver[sizeof rec.arm_ver - 1] = '\0';
	}
	return rec_store();
}

int box_boot_note_confirm(void)
{
	if (!rec_valid) {
		return -ENODEV;
	}
	/* Only counts as an update if there was something to keep. A confirm on an
	 * already-confirmed image is a no-op here, not a phantom update. */
	if (rec.armed) {
		rec.updates++;
	}
	rec.armed      = 0;
	rec.trial_seen = 0;
	on_trial       = 0;
	return rec_store();
}
