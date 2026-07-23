/*
 * box_ptp.c -- read the ENET IEEE-1588 hardware clock.
 *
 * The clock is a devicetree node (nxp,enet-ptp-clock) that the PTP driver
 * instantiates once PTP_CLOCK_NXP_ENET is enabled, so we fetch it directly by
 * label. (An earlier version used net_eth_get_ptp_clock_by_index(), which can
 * return NULL if the clock isn't yet associated with the interface.) Falls back
 * to the interface lookup if a board leaves the node without a device.
 *
 * NOTE: this reads the LOCAL free-running counter -- it needs no PTP partner.
 * Disciplining it to a grandmaster (the sub-us sync story) is a separate thing
 * that does need a peer; that is validated against the i.MX95 host, not here.
 */
#include "box_ptp.h"

#include <zephyr/device.h>
#include <zephyr/drivers/ptp_clock.h>
#include <zephyr/net/ptp_time.h>
#include <zephyr/net/ethernet.h>

static const struct device *ptp_dev(void)
{
	static const struct device *d;
	if (d) {
		return d;
	}
#if DT_NODE_HAS_STATUS(DT_NODELABEL(enet_ptp_clock), okay)
	d = DEVICE_DT_GET(DT_NODELABEL(enet_ptp_clock));
	if (!device_is_ready(d)) {
		d = net_eth_get_ptp_clock_by_index(0);   /* fallback */
	}
#else
	d = net_eth_get_ptp_clock_by_index(0);
#endif
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

	if (!d || !device_is_ready(d) || ptp_clock_get(d, &t) != 0) {
		return 0;
	}
	return (uint64_t) t.second * 1000000000ULL + t.nanosecond;
}
