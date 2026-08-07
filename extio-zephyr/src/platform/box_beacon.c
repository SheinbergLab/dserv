/*
 * box_beacon.c -- the UDP discovery beacon. See box_beacon.h for what it is for
 * and why the wire contract is not ours to change.
 *
 * Zephyr-native rather than a transliteration of the RP2350's W6300 socket
 * code: one BSD UDP socket, opened lazily, SO_BROADCAST, non-blocking sendto.
 * The PAYLOAD is what must match the Pico byte for byte -- not the plumbing.
 */

#include "box_beacon.h"

#if defined(CONFIG_NETWORKING)

#include "box_net_eth.h"        /* box_net_eth_get_ip */
#include "box_uplink.h"         /* box_uplink_reg_health */
#include "box_announce.h"       /* box_build_key: the same shelf key we announce */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <stdio.h>
#include <string.h>

#ifndef BOX_FW_VERSION
#define BOX_FW_VERSION   "dev"
#endif
#define BOX_BOARD_ID     CONFIG_BOARD

#define BOX_BEACON_PORT  5011      /* what extio-setup listens on */
#define BEACON_MS        1500      /* the RP2350's cadence; extio-setup ages
                                    * entries out at 12 s, so this must stay
                                    * comfortably under that TTL */

static int     sock = -1;
static int64_t next_ms;

/* Open once, lazily. Deliberately NOT at init: on a DHCP box there is no
 * address for seconds after boot, and a socket held open through that window
 * buys nothing. Failure is silent and retried on the next pass -- discovery is
 * an aid, and a box must never fail to do its actual job because it could not
 * advertise itself. */
static int beacon_socket(void)
{
	if (sock >= 0) {
		return sock;
	}
	int s = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (s < 0) {
		return -1;
	}
	int one = 1;

	if (zsock_setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one) < 0) {
		zsock_close(s);
		return -1;
	}
	sock = s;
	return sock;
}

void box_beacon_service(const box_config_t *cfg)
{
	int64_t now = k_uptime_get();

	if (now < next_ms) {
		return;
	}
	next_ms = now + BEACON_MS;

	uint8_t ip[4] = { 0, 0, 0, 0 };

	box_net_eth_get_ip(ip);
	if (!(ip[0] || ip[1] || ip[2] || ip[3])) {
		return;             /* USB, or no lease yet -- nothing to advertise */
	}

	int s = beacon_socket();

	if (s < 0) {
		return;
	}

	int      up = 0, ever = 0;
	uint32_t down_ms = 0;
	uint16_t tries = 0;

	box_uplink_reg_health(&up, &down_ms, &tries, &ever);

	/* Static: this runs on the service loop, one thread, and a ~320-byte frame
	 * does not belong on that stack next to the rest of a service pass. */
	static char body[384];
	int n = snprintf(body, sizeof body,
		"{\"t\":\"extio\",\"v\":2,\"name\":\"%s\",\"ip\":\"%u.%u.%u.%u\","
		"\"fw\":\"%s\",\"board\":\"%s\",\"build\":\"%s\","
		"\"target\":\"%u.%u.%u.%u:%u\","
		"\"link\":\"%s\",\"down_ms\":%u,\"tries\":%u,\"ever\":%d}",
		dserv_cfg_name(cfg), ip[0], ip[1], ip[2], ip[3],
		BOX_FW_VERSION, BOX_BOARD_ID, box_build_key(),
		cfg->dserv_ip[0], cfg->dserv_ip[1], cfg->dserv_ip[2], cfg->dserv_ip[3],
		dserv_cfg_port(cfg),
		up ? "up" : "down", (unsigned) down_ms, (unsigned) tries, ever);

	/* Truncation would emit invalid JSON, which a listener drops anyway -- but
	 * silently, and it would look identical to a box that is simply absent.
	 * Drop it here instead, where the reason is knowable. */
	if (n < 0 || n >= (int) sizeof body) {
		return;
	}

	struct sockaddr_in to = { 0 };

	to.sin_family      = AF_INET;
	to.sin_port        = htons(BOX_BEACON_PORT);
	to.sin_addr.s_addr = htonl(INADDR_BROADCAST);   /* 255.255.255.255, as the
							 * RP2350 sends it */

	/* Best-effort by design: no retry, no error surfaced. A dropped beacon is
	 * corrected 1.5 s later by the next one. */
	(void) zsock_sendto(s, body, n, ZSOCK_MSG_DONTWAIT,
			    (struct sockaddr *) &to, sizeof to);
}

#endif /* CONFIG_NETWORKING */
