/*
 * box_net_eth.c -- RW612/Zephyr Ethernet transport over BSD-style sockets.
 */
#include "box_net_eth.h"

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/socket.h>
#include <string.h>
#include <errno.h>

static struct net_if *iface;
static int sock = -1;
static int connecting;          /* 1 = a non-blocking connect() is in flight */

/* ---- config server: dserv connects BACK here with our subscribed datapoints ----
 * Two sockets in opposite directions is dserv's subscription model, not a choice:
 * %reg names an ip:port and dserv opens a connection to it. See the header. */
static int srv_listen = -1;     /* listening socket (BOX_ETH_CFG_PORT)          */
static int srv_conn   = -1;     /* dserv's accepted connect-back                */
static int srv_fresh;           /* one-shot: report BOX_NET_RESET after accept  */

static void srv_drop(void)
{
	if (srv_conn >= 0) {
		zsock_close(srv_conn);
		srv_conn = -1;
	}
}

int box_net_eth_init(void)
{
	iface = net_if_get_default();
	if (!iface) {
		return -1;
	}
	net_dhcpv4_start(iface);
	return 0;
}

int box_net_eth_link(void)
{
	return (iface && net_if_is_up(iface)) ? 1 : 0;
}

int box_net_eth_connected(void)
{
	if (sock < 0) {
		return 0;
	}
	if (!connecting) {
		return 1;                       /* established */
	}
	/* poll the in-flight connect without blocking */
	struct zsock_pollfd pfd = { .fd = sock, .events = ZSOCK_POLLOUT };
	if (zsock_poll(&pfd, 1, 0) <= 0) {
		return 0;                       /* still connecting */
	}
	int err = 0;
	socklen_t len = sizeof err;
	zsock_getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
	if (err == 0) {
		connecting = 0;
		return 1;                       /* connect succeeded */
	}
	zsock_close(sock);                      /* connect failed -- reset for a retry */
	sock = -1;
	connecting = 0;
	return 0;
}

int box_net_eth_get_ip(uint8_t out[4])
{
	if (!iface) {
		return 0;
	}
	struct in_addr *a = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
	if (!a) {
		return 0;
	}
	out[0] = a->s4_addr[0]; out[1] = a->s4_addr[1];
	out[2] = a->s4_addr[2]; out[3] = a->s4_addr[3];
	return 1;
}

int box_net_eth_wait_ip(uint8_t out[4], int timeout_ms)
{
	int waited = 0;
	while (waited <= timeout_ms) {
		struct in_addr *a = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
		if (a) {
			out[0] = a->s4_addr[0];
			out[1] = a->s4_addr[1];
			out[2] = a->s4_addr[2];
			out[3] = a->s4_addr[3];
			return 1;
		}
		k_msleep(100);
		waited += 100;
	}
	return 0;
}

/* NON-BLOCKING connect: a blocking connect() would stall the single-threaded
 * service loop for the full TCP timeout whenever the dserv target is
 * unreachable (seen live on a Teensy 4.1 when the demo dserv IP was off-subnet).
 * Start the connect and let box_net_eth_connected() poll it to completion. */
int box_net_eth_connect(const uint8_t dserv_ip[4], uint16_t port)
{
	if (sock >= 0) {
		return 0;                       /* one already in flight / established */
	}
	struct sockaddr_in addr = { 0 };
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	memcpy(&addr.sin_addr, dserv_ip, 4);

	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		return -1;
	}
	zsock_fcntl(sock, ZVFS_F_SETFL, ZVFS_O_NONBLOCK);

	int r = zsock_connect(sock, (struct sockaddr *)&addr, sizeof addr);
	if (r == 0) {
		connecting = 0;                 /* connected immediately (rare) */
		return 0;
	}
	if (errno == EINPROGRESS) {
		connecting = 1;                 /* in flight -- poll in connected() */
		return 0;
	}
	zsock_close(sock);
	sock = -1;
	return -1;
}

void box_net_eth_server_service(void)
{
	if (srv_listen < 0) {
		/* Rate-limit the setup retry. This runs on the service loop, which is
		 * event-driven and therefore fast, and with no link the socket/bind
		 * fails EVERY pass -- an unbounded retry would put a syscall pair in
		 * the DI publish path forever on a cable-less box. */
		static int64_t next_try;
		int64_t now = k_uptime_get();
		if (now < next_try) {
			return;
		}
		next_try = now + 1000;

		srv_listen = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (srv_listen < 0) {
			return;
		}
		int one = 1;
		/* dserv's connect-back may land while a previous accepted socket is
		 * still in TIME_WAIT; without SO_REUSEADDR the rebind fails and the
		 * box goes permanently deaf after one dserv restart. */
		zsock_setsockopt(srv_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

		struct sockaddr_in a = { 0 };
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = INADDR_ANY;
		a.sin_port = htons(BOX_ETH_CFG_PORT);
		if (zsock_bind(srv_listen, (struct sockaddr *) &a, sizeof a) < 0 ||
		    zsock_listen(srv_listen, 1) < 0) {
			zsock_close(srv_listen);
			srv_listen = -1;
			return;
		}
		zsock_fcntl(srv_listen, ZVFS_F_SETFL, ZVFS_O_NONBLOCK);
	}

	if (srv_conn >= 0) {
		return;                      /* one connect-back is all dserv makes */
	}
	int c = zsock_accept(srv_listen, NULL, NULL);
	if (c < 0) {
		return;                      /* EAGAIN: nobody waiting */
	}
	zsock_fcntl(c, ZVFS_F_SETFL, ZVFS_O_NONBLOCK);
	srv_conn = c;
	srv_fresh = 1;                       /* -> BOX_NET_RESET -> announce burst */
}

int box_net_eth_server_up(void)
{
	return srv_conn >= 0;
}

int box_net_eth_poll(uint8_t *buf, int max)
{
	/* A fresh connect-back means dserv is listening NOW: reset the framer and
	 * let main() describe the box. This is the Ethernet analogue of USB's
	 * "host opened the pipe" and is deliberately reported on CONNECT.
	 * (Reporting it only on close -- the original behaviour -- fired the
	 * announce burst into an already-dead socket, so a box on Ethernet never
	 * published its manifest at all.) */
	if (srv_fresh) {
		srv_fresh = 0;
		return BOX_NET_RESET;
	}

	/* ONE poll covers both sockets, and in the common case (nothing pending) that
	 * is the ONLY syscall this function makes.
	 *
	 * Two sockets have to be watched for different reasons: the config link
	 * carries dserv's pushes, and the client socket carries nothing inbound but
	 * still needs EOF detection -- box_net_eth_connected() only knows "sock >= 0",
	 * so without it a dserv restart parks the client socket in CLOSE_WAIT forever,
	 * the arbiter never reconnects, and the box goes silently write-only (the
	 * very failure this file exists to fix, reached from the other direction).
	 *
	 * Doing that as two separate non-blocking recv()s costs an extra syscall on
	 * EVERY service-loop pass, and it is not free: measured at ~70-90 us of added
	 * DI delivery latency with the FLOOR moving too (systematic, not jitter). The
	 * service loop is this box's scarcest resource -- it is what gates how fast an
	 * edge reaches dserv -- so per-pass work in here is worth real care. */
	struct zsock_pollfd pf[2];
	int nfd = 0, ci = -1, si = -1;

	if (sock >= 0)     { ci = nfd; pf[nfd].fd = sock;     pf[nfd].events = ZSOCK_POLLIN; nfd++; }
	if (srv_conn >= 0) { si = nfd; pf[nfd].fd = srv_conn; pf[nfd].events = ZSOCK_POLLIN; nfd++; }
	if (nfd == 0 || zsock_poll(pf, nfd, 0) <= 0) {
		return 0;
	}

	/* Client socket: readable or hung up can only mean EOF -- dserv never sends
	 * anything down this direction, so there is no data case to handle. */
	if (ci >= 0 && (pf[ci].revents & (ZSOCK_POLLIN | ZSOCK_POLLHUP | ZSOCK_POLLERR))) {
		zsock_close(sock);
		sock = -1;
		connecting = 0;
	}

	if (si >= 0 && (pf[si].revents & (ZSOCK_POLLIN | ZSOCK_POLLHUP | ZSOCK_POLLERR))) {
		if (max <= 0) {
			return 0;
		}
		int n = zsock_recv(srv_conn, buf, (size_t) max, ZSOCK_MSG_DONTWAIT);
		if (n > 0) {
			return n;
		}
		if (n == 0) {                    /* dserv closed the config link */
			srv_drop();
			return BOX_NET_RESET;        /* framer reset; the burst is a no-op */
		}
	}
	return 0;
}

int box_net_eth_send(const uint8_t *buf, int len)
{
	if (sock < 0) {
		return -1;
	}
	int n = zsock_send(sock, buf, (size_t) len, 0);
	return (n == len) ? 0 : -1;
}

/* One %-command per connection, with a bounded non-blocking connect.
 *
 * ONE COMMAND PER CONNECTION is not tidiness: dserv's '%' reader is greedy and
 * will swallow a second line sent on the same connection, so a batched %reg +
 * %match sequence silently loses everything after the first.
 *
 * The source port ROTATES 55000-55007 so back-to-back commands never reuse a
 * 4-tuple the peer kernel may still be aging out in TIME_WAIT. The bind is
 * best-effort: if it fails we fall through to an ephemeral port rather than
 * failing the registration.
 *
 * The connect is NON-BLOCKING + polled even though this runs off the service
 * loop -- a blocking connect against an unreachable dserv stalls for the full
 * TCP timeout, which would wedge the registration thread and stop the retry
 * watchdog from ever running again. (Same lesson as box_net_eth_connect.) */
int box_net_eth_send_command(const uint8_t dserv_ip[4], uint16_t port,
			     const char *cmd)
{
	static uint16_t rr;
	int rc = -1;

	int s = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s < 0) {
		return -1;
	}

	struct sockaddr_in me = { 0 };
	me.sin_family = AF_INET;
	me.sin_addr.s_addr = INADDR_ANY;
	me.sin_port = htons((uint16_t) (55000 + (rr++ & 7)));
	int one = 1;
	zsock_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	(void) zsock_bind(s, (struct sockaddr *) &me, sizeof me);   /* best effort */

	zsock_fcntl(s, ZVFS_F_SETFL, ZVFS_O_NONBLOCK);

	struct sockaddr_in a = { 0 };
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	memcpy(&a.sin_addr, dserv_ip, 4);

	if (zsock_connect(s, (struct sockaddr *) &a, sizeof a) < 0 &&
	    errno != EINPROGRESS && errno != EALREADY) {
		goto out;
	}
	struct zsock_pollfd pfd = { .fd = s, .events = ZSOCK_POLLOUT };
	if (zsock_poll(&pfd, 1, 300) <= 0) {           /* 300 ms to establish */
		goto out;
	}
	int err = 0;
	socklen_t elen = sizeof err;
	zsock_getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen);
	if (err != 0) {
		goto out;
	}

	size_t len = strlen(cmd);
	if (zsock_send(s, cmd, len, 0) != (int) len) {
		goto out;
	}

	/* dserv answers every '%' command with "<rc> ...\n"; rc 1 = accepted. Waiting
	 * for it is what makes the return value truthful -- without it we could not
	 * distinguish "registered" from "wrote into a socket nobody parsed", which is
	 * exactly the failure that leaves a box publishing but deaf. */
	pfd.events = ZSOCK_POLLIN;
	if (zsock_poll(&pfd, 1, 300) <= 0) {
		goto out;
	}
	char rep[16] = { 0 };
	int n = zsock_recv(s, rep, sizeof rep - 1, 0);
	if (n > 0 && rep[0] == '1') {
		rc = 0;
	}
out:
	zsock_close(s);
	return rc;
}
