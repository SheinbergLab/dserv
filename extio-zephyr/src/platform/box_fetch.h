/*
 * box_fetch -- OTA image pull: '<' binary-get of a dserv datapoint, streamed
 * into a caller-supplied sink from a dedicated low-priority worker thread.
 *
 * Ethernet only: the pull rides a second, transient TCP socket to dserv's
 * command port, which the USB carrier has no equivalent of. Wire-compatible
 * with the rig-proven RP2350 original (wiznet-io/net/box_net_w6300.h,
 * box_net_get_binary) -- same request, same reply layout, same "the image
 * never sits in RAM" property. The Zephyr difference is WHERE it runs: the
 * RP2350 blocks its RT loop for the duration; here a worker thread blocks
 * instead, so telemetry, PTP, and the service loop keep breathing while the
 * stream drains -- and TCP backpressure (the worker simply not reading while
 * a sector erases) is what paces dserv, replacing every tuned window/drip
 * constant in the host's chunk path.
 */
#ifndef BOX_FETCH_H
#define BOX_FETCH_H

#include <stdint.h>

/* Feed one span of value bytes. Return 0 to keep going, <0 to abort. */
typedef int (*box_fetch_sink_t)(void *ud, const uint8_t *data, uint32_t len);

/* Terminal callback, on the worker thread: rc = total value bytes streamed
 * (>= 0, the sink saw them all), or < 0 on any failure (connect, protocol,
 * timeout, sink abort) -- the stream may have delivered a prefix first. */
typedef void (*box_fetch_done_t)(void *ud, int rc);

/* Queue one fetch on the worker. 0 = accepted (done() WILL fire, once);
 * -1 = refused (bad args, or a fetch is already running). */
int box_fetch_start(const uint8_t dserv_ip[4], uint16_t port, const char *key,
		    box_fetch_sink_t sink, box_fetch_done_t done, void *ud);

/* 1 while a fetch owns the sink (from start-accept until done() returns). */
int box_fetch_busy(void);

#endif /* BOX_FETCH_H */
