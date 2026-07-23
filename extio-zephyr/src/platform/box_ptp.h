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

#endif /* BOX_PTP_H */
