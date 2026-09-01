/*
 * box_boot.h -- why this box is running, and WHICH image it is running.
 *
 * OTA step 5 (rollback visibility). Steps 1-4 made a bad image roll back
 * automatically; this makes the rollback VISIBLE. A revert is silent by
 * construction: MCUboot swaps the old image back and it boots normally, so a
 * fleet page shows a healthy box -- running firmware nobody chose. The failure
 * mode is not a dead box, it is a box that quietly disagrees with the fleet.
 *
 * Three questions, answered once at boot and then never recomputed:
 *
 *   1. WHY did we reset?          hwinfo reset cause, latched and CLEARED.
 *   2. Are we ON TRIAL?           MCUboot image-confirmed flag.
 *   3. Did an armed update FAIL?  a persisted breadcrumb, below.
 *
 * WHY A PERSISTED BREADCRUMB AND NOT RAM. The revert you most want to see is
 * the one after a power cut during a trial, and that is exactly the case where
 * a RAM breadcrumb (noinit, retention, watchdog scratch -- the RP2350 approach)
 * is gone. NVS survives it. Cost is one small write per OTA transition, which
 * is nothing next to the ~10 s of slot writes that precede it.
 *
 * WHY NOT COMPARE VERSIONS to tell a revert from a successful update: because
 * dev images routinely share a version, and that is precisely how the RP2350
 * OTA looked stuck for a day when it had actually succeeded (OTA.md, 2026-07-14:
 * "base and trial shared a version, so state/fw never changed on commit"). The
 * breadcrumb instead records whether a trial boot was ever SEEN, which is
 * version-independent and distinguishes the two failures that look identical
 * from the outside:
 *
 *   armed, trial seen, now confirmed+old   -> REVERT   (image ran, nobody kept it)
 *   armed, trial never seen                -> REJECTED (MCUboot refused to swap:
 *                                                       bad signature, bad header,
 *                                                       image at the wrong offset)
 *
 * That second one is worth its own word. An image can transfer perfectly, verify
 * by read-back, arm without error, and still never run -- which is exactly the
 * bug OTA step 4 found (image written at slot offset 0 instead of the second
 * sector). "rejected" names it in one datapoint instead of a rig visit.
 */
#ifndef BOX_BOOT_H
#define BOX_BOOT_H

#include <stdint.h>

/* Latch everything. Call ONCE from main, after the persistent store is mounted
 * (it reads and may update the breadcrumb) and before the first announce. */
void box_boot_init(void);

/* Why we booted, latched at box_boot_init():
 *
 *   power | pin | software | watchdog | lockup | security | brownout | other
 *   trial     -- running an unconfirmed image; the next reset reverts it
 *   revert    -- an armed image ran and was NOT kept; we are the old one again
 *   rejected  -- an armed image never ran at all (MCUboot would not take it)
 *
 * LATCHED, not recomputed per call, because box_boot_init() CLEARS the hardware
 * cause register -- see the .c for why leaving it set makes the field lie. */
const char *box_boot_reason(void);

/* The raw hwinfo mask as read before clearing, for the boot banner. A named
 * reason collapses information ("software" hides that lockup was also set);
 * report the errno-equivalent alongside the word, as elsewhere in this tree. */
uint32_t box_boot_reset_cause(void);

/* Semver of the RUNNING image, from its MCUboot header ("0.0.2+0"), or NULL on
 * a build with no bootloader. NULL doubles as "none of the OTA fields below
 * mean anything here", so callers need no CONFIG_ ifdefs.
 *
 * This is NOT state/fw: that is a compile-time string (BOX_FW_VERSION, still
 * "dev" in this tree) and is identical in every image we build, so it cannot
 * show an OTA taking effect. The header version is per-image and signed. */
const char *box_boot_img_ver(void);

/* 1 while running an image that has not been confirmed. */
int box_boot_on_trial(void);

/* Lifetime counters from the breadcrumb; -1 if there is no store. */
int box_boot_reverts(void);
int box_boot_rejects(void);
int box_boot_updates(void);

/* The image version we last ARMED, or "none". After a revert this is the answer
 * to "what was it trying to become?", which the box itself no longer runs. */
const char *box_boot_last_arm_ver(void);

/* Breadcrumb transitions, called by the OTA commands. Both return 0 or a
 * negative errno; both are best-effort in the sense that a failed write costs
 * visibility, never correctness -- MCUboot's own trailer is the authority on
 * what actually boots. */
int box_boot_note_arm(const char *staged_ver);
int box_boot_note_confirm(void);

/* Read an MCUboot image header version into buf ("0.0.2+0").
 * slot 0 = the running image, slot 1 = the staged one. Returns 0, or a negative
 * errno; -EIO means "no valid image header there", which is the one check that
 * catches an image written to the wrong place. */
int box_boot_image_ver(int slot, char *buf, uint32_t len);

/* Make the NEXT boot stop in the board's UF2 bootloader instead of running us,
 * and reset into it. DOES NOT RETURN on a board that supports it; returns 0 on
 * one that does not, so the caller can say so rather than appear to hang.
 *
 * Lives here because "what the next boot does" is exactly this file's subject.
 *
 * The point is reach rather than convenience. These boards are reflashed by
 * dropping a .uf2 on a mass-storage volume, and the only documented way to
 * expose that volume is PHYSICAL -- double-tap reset, or hold the button while
 * plugging in. The MDK dongle is the second kind AND ships sealed in a case, in
 * a USB port behind a machine. Neither software escape people reach for works:
 * a plain reset just restarts the app, and the 1200-baud touch is an
 * application behaviour Arduino and Adafruit cores implement and Zephyr does
 * not (both measured on that dongle, 2026-09-01). The bootloader's real trigger
 * is a retained register that only the running firmware can write -- so if the
 * firmware does not offer this, nothing can.
 *
 * Callers: cmd/bootsel (main.c) and the `bootsel` console verb. Flush anything
 * that must be SEEN before calling -- this does not come back. */
int box_boot_enter_uf2(void);

#endif /* BOX_BOOT_H */
