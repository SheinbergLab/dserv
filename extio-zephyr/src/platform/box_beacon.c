/*
 * box_beacon.c -- the UDP discovery beacon. See box_beacon.h for what it is for
 * and why the wire contract is not ours to change.
 *
 * Zephyr-native rather than a transliteration of the RP2350's W6300 socket
 * code: one UDP socket, opened lazily, non-blocking sendto. The PAYLOAD is what
 * must match the Pico byte for byte -- not the plumbing, and copying BSD
 * plumbing on faith is exactly what broke the first cut (see beacon_socket).
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
#include <errno.h>

#ifndef BOX_FW_VERSION
#define BOX_FW_VERSION   "dev"
#endif
#define BOX_BOARD_ID     CONFIG_BOARD

#define BOX_BEACON_PORT  5011      /* what extio-setup listens on */
#define BEACON_MS        1500      /* the RP2350's cadence; extio-setup ages
                                    * entries out at 12 s, so this must stay
                                    * comfortably under that TTL */

static int      sock = -1;
static int64_t  next_ms;
static uint32_t st_sent, st_fail;
static int      st_errno;      /* errno of the most recent failure */

/* Open once, lazily. Deliberately NOT at init: on a DHCP box there is no
 * address for seconds after boot, and a socket held open through that window
 * buys nothing. Failure is retried on the next pass -- discovery is an aid, and
 * a box must never fail to do its actual job because it could not advertise
 * itself -- but it is COUNTED (dbg/beacon_*), because a silent best-effort path
 * is indistinguishable from one that never ran. */
static int beacon_socket(void)
{
	if (sock >= 0) {
		return sock;
	}
	int s = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (s < 0) {
		st_fail++;
		st_errno = errno;
		return -1;
	}

	/* SO_BROADCAST is a BSD requirement that Zephyr does NOT share: the
	 * constant exists in socket.h but no handler does, so setsockopt falls
	 * through to ENOPROTOOPT and fails. Zephyr's IPv4 path does not gate
	 * broadcast on it either, so the correct action is to ask and not care.
	 *
	 * Treating the failure as fatal is not a hypothetical: it is what the
	 * first cut of this file did, and it closed the socket on every single
	 * call, so the beacon never emitted one byte while looking entirely
	 * healthy from the outside. Left in place (rather than deleted) so the
	 * call is already correct if a later Zephyr implements it. */
	int one = 1;

	(void) zsock_setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);

	sock = s;
	return sock;
}

void box_beacon_stats(uint32_t *sent, uint32_t *fail, int *last_errno)
{
	if (sent)       { *sent = st_sent; }
	if (fail)       { *fail = st_fail; }
	if (last_errno) { *last_errno = st_errno; }
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

	/* Best-effort: no retry -- a dropped beacon is corrected 1.5 s later by
	 * the next one -- but counted, so "no boxes found" can be told apart from
	 * "this box never transmitted". */
	if (zsock_sendto(s, body, n, ZSOCK_MSG_DONTWAIT,
			 (struct sockaddr *) &to, sizeof to) < 0) {
		st_fail++;
		st_errno = errno;
		/* A send failure usually means the interface went away underneath
		 * us; drop the socket so the next pass opens a fresh one against
		 * whatever the stack has now. */
		zsock_close(s);
		sock = -1;
		return;
	}
	st_sent++;
}

#endif /* CONFIG_NETWORKING */
