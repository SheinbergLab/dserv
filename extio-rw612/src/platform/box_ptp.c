/*
 * box_ptp.c -- read the RW612 ENET IEEE-1588 hardware clock (enet_ptp_clock).
 */
#include "box_ptp.h"

#include <zephyr/device.h>
#include <zephyr/drivers/ptp_clock.h>
#include <zephyr/net/ptp_time.h>
#include <zephyr/net/ethernet.h>

/* The PTP clock is registered by the ENET driver, not a standalone DT device, so
 * fetch it at runtime (cached) rather than via DEVICE_DT_GET. */
static const struct device *ptp_dev(void)
{
	static const struct device *d;
	if (!d) {
		d = net_eth_get_ptp_clock_by_index(0);
	}
	return d;
}

bool box_ptp_ready(void)
{
	const struct device *d = ptp_dev();
	return d && device_is_ready(d);
}

uint64_t box_ptp_now_ns(void)
{
	const struct device *d = ptp_dev();
	struct net_ptp_time t;

	if (!d || ptp_clock_get(d, &t) != 0) {
		return 0;
	}
	return (uint64_t) t.second * 1000000000ULL + t.nanosecond;
}
