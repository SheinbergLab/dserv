/*
 * box_ptp.h -- access to the RW612 ENET IEEE-1588 hardware clock.
 *
 * The whole reason Ethernet is interesting on this silicon (vs the W6300's
 * software echo sync): the MAC timestamps frames in hardware off this clock, so
 * the box<->dserv sync tier can reach sub-µs instead of the ~98 µs the software
 * estimator gives. Block #5 exposes the clock; wiring hardware TX/RX packet
 * timestamps into the sync anchor is the follow-on (BENCH_NXP D3).
 */
#ifndef BOX_PTP_H
#define BOX_PTP_H

#include <stdint.h>
#include <stdbool.h>

/* Is the ENET PTP clock device present and ready? */
bool box_ptp_ready(void);

/* Current PTP hardware time in nanoseconds, or 0 if unavailable. */
uint64_t box_ptp_now_ns(void);

/* Has the 1588 counter actually been SET to master time, as opposed to merely
 * running?
 *
 * Tested against the EPOCH, which is the one PTP fact on this box that cannot
 * lie. Every other indicator is a proxy that reads healthy on an unsynced box:
 * `ready` means the device exists, `sync/source=ptp` means a paired read
 * succeeded, `anchored` means ptpconf pushed us an offset. On 2026-07-29 all
 * three said yes while the counter was 56 years wrong. A free-running counter
 * reads seconds-since-boot; a disciplined one reads ~1.78e9 s. Nothing between
 * those is reachable, so the threshold needs no tuning. */
bool box_ptp_synced(void);

#endif /* BOX_PTP_H */
