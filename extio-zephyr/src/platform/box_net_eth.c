/*
 * box_net_eth.c -- RW612/Zephyr Ethernet transport over BSD-style sockets.
 */
#include "box_net_eth.h"
#include "box_event.h"

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
/* ---- inbound instrumentation ----
 * With the stack in ITCM the OUTBOUND cost collapsed (275 -> 61 us) and the
 * remaining loopback time is the command leg. These split the box's share of it:
 *   wake_us  RX thread saw the socket readable -> service loop reached recv
 *   recv_us  cost of the zsock_recv that returned the frame
 * If both are small, the time is spent BEFORE the socket became readable --
 * dserv's send path, transit, or the box's net RX thread -- and not in our loop. */
static volatile uint32_t rx_signal_cyc;
static uint32_t wake_last_us, wake_max_us;
static uint32_t recv_last_us, recv_max_us;

void box_net_eth_rx_stats(uint32_t *wake_us, uint32_t *wake_max,
			  uint32_t *recv_us, uint32_t *recv_max)
{
	if (wake_us)   *wake_us   = wake_last_us;
	if (wake_max)  *wake_max  = wake_max_us;
	if (recv_us)   *recv_us   = recv_last_us;
	if (recv_max)  *recv_max  = recv_max_us;
}

/* Small frames carrying single events are the ENTIRE traffic pattern here, so
 * Nagle is pure harm: it holds a 128-byte publish until a prior segment is
 * ACKed, adding an RTT (or a delayed-ACK interval) to an event whose whole
 * value is its timeliness. dserv disables it on its own send socket
 * (Dataserver.cpp open_send_sock) and the Pico's W6300 path opens with
 * SF_TCP_NODELAY -- this port simply never did, which is an oversight rather
 * than a choice. */
static void tcp_nodelay(int fd)
{
	int one = 1;
	(void) zsock_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

static void srv_drop(void)
{
	if (srv_conn >= 0) {
		zsock_close(srv_conn);
		srv_conn = -1;
	}
}

int box_net_eth_init(const box_config_t *cfg)
{
	iface = net_if_get_default();
	if (!iface) {
		return -1;
	}

	/* Bring the interface ADMIN UP explicitly. Zephyr's net_config subsystem
	 * (CONFIG_NET_CONFIG_SETTINGS/AUTO_INIT) normally does this, and we do not
	 * enable it -- we own our own addressing. Without it the iface stays down,
	 * the ENET driver never starts the PHY, autonegotiation never runs, and
	 * BOTH ends report no carrier. That presents as a dead cable: the box says
	 * link=0, the host says NO-CARRIER, and swapping cables/ports/hosts teaches
	 * you nothing. Verified 2026-07-25 -- stock samples/net/ptp linked on the
	 * same board+cable+host purely because it enables net_config.
	 * Idempotent: net_if_up() on an already-up iface returns 0. */
	if (!net_if_is_admin_up(iface)) {
		int rc = net_if_up(iface);
		if (rc && rc != -EALREADY) {
			return rc;
		}
	}

	if (cfg && cfg->net_mode == NET_MODE_STATIC) {
		struct in_addr ip, nm, gw;

		memcpy(&ip, cfg->net_ip, 4);
		if (!net_if_ipv4_addr_add(iface, &ip, NET_ADDR_MANUAL, 0)) {
			return -1;
		}
		memcpy(&nm, cfg->net_sn, 4);
		if (nm.s_addr == 0) {
			nm.s_addr = htonl(0xffffff00);    /* zero mask => /24 */
		}
		net_if_ipv4_set_netmask_by_addr(iface, &ip, &nm);
		memcpy(&gw, cfg->net_gw, 4);
		if (gw.s_addr != 0) {                     /* zero gw => none */
			net_if_ipv4_set_gw(iface, &gw);
		}
		return 0;
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
	tcp_nodelay(sock);                       /* publishes are small and latency-critical */
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
	tcp_nodelay(c);                          /* inbound cmd/config, same argument */
	zsock_fcntl(c, ZVFS_F_SETFL, ZVFS_O_NONBLOCK);
	srv_conn = c;
	srv_fresh = 1;                       /* -> BOX_NET_RESET -> announce burst */
}

int box_net_eth_server_up(void)
{
	return srv_conn >= 0;
}

/* ---- wake the service loop on inbound packets ----
 *
 * Every other inbound path signals box_event and gets an immediate wake: the CDC
 * RX ISR (box_net_usb), the console, the GPIO edge ISR. Ethernet had NOTHING, so
 * a command sat in the socket until box_event_wait(K_MSEC(1)) timed out -- ~0.5 ms
 * of pure structural delay on every dserv->box command, and up to 1 ms worst case.
 * That is invisible on a throughput test and dominant on a latency one.
 *
 * Zephyr's socket API exposes no ISR hook, so the idiomatic equivalent is a thread
 * blocked in zsock_poll() that signals and goes straight back to waiting. It never
 * touches the socket data -- the service loop still owns every read, so there is no
 * second consumer and no locking. Priority is below the service loop: this only
 * needs to make the loop runnable, not to run before it. */
/* 2048: zsock_poll + fdtable machinery overflowed the original 1024 -- the
 * suspected cause of the wake thread dying silently (dbg/wake_us frozen). */
static K_THREAD_STACK_DEFINE(eth_rx_stack, 2048);
static struct k_thread eth_rx_thread;

static void eth_rx_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		int fd = srv_conn;               /* sampled; -1 while disconnected */
		if (fd < 0) {
			k_msleep(20);            /* nothing to watch yet */
			continue;
		}
		struct zsock_pollfd pfd = { .fd = fd, .events = ZSOCK_POLLIN };
		/* Bounded wait so a socket swap (reconnect) is picked up promptly and a
		 * closed fd can never wedge this thread. */
		if (zsock_poll(&pfd, 1, 200) > 0) {
			rx_signal_cyc = k_cycle_get_32();
			box_event_signal();
			/* The loop drains it. Yield so we re-poll after that has happened
			 * rather than spinning on a still-readable socket. */
			k_msleep(1);
		}
	}
}

void box_net_eth_rx_wake_start(void)
{
	k_thread_create(&eth_rx_thread, eth_rx_stack, K_THREAD_STACK_SIZEOF(eth_rx_stack),
			eth_rx_thread_fn, NULL, NULL, NULL, 8, 0, K_NO_WAIT);
	k_thread_name_set(&eth_rx_thread, "extio_ethrx");
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
		uint32_t r0 = k_cycle_get_32();
		int n = zsock_recv(srv_conn, buf, (size_t) max, ZSOCK_MSG_DONTWAIT);
		if (n > 0) {
			uint32_t d = k_cyc_to_us_floor32(k_cycle_get_32() - r0);
			recv_last_us = d;
			if (d > recv_max_us) recv_max_us = d;

			uint32_t w = k_cyc_to_us_floor32(r0 - rx_signal_cyc);
			if (w < 1000000) {          /* ignore a stale/never-signalled stamp */
				wake_last_us = w;
				if (w > wake_max_us) wake_max_us = w;
			}
			return n;
		}
		if (n == 0) {                    /* dserv closed the config link */
			srv_drop();
			return BOX_NET_RESET;        /* framer reset; the burst is a no-op */
		}
	}
	return 0;
}

/* ---- send-cost instrumentation ----
 * Three host-side hypotheses for the ~650 us between two frames the box emits
 * microseconds apart (syscall count, Nagle + RX wake, TX traffic class) were all
 * wrong. This measures the thing directly instead of narrowing it again: if
 * zsock_send itself burns ~650 us, the cost is the stack traversal; if it
 * returns fast, the delay is elsewhere and the send was never the culprit. */
static uint32_t send_last_us, send_max_us;


void box_net_eth_send_stats(uint32_t *last_us, uint32_t *max_us)
{
	if (last_us) *last_us = send_last_us;
	if (max_us)  *max_us  = send_max_us;
}

/* UPLINK FAILURE DETECTION. Without this the box publishes into a dead socket
 * forever and looks dead from dserv while running perfectly.
 *
 * The socket is non-blocking, so a failed send DROPS the frame and returns -1 --
 * and every caller ignored that (box_uplink_send passes it up, nothing checks).
 * box_net_eth_connected() then reported "established" purely because the fd still
 * existed; it never tested whether the peer was there. So once dserv dropped its
 * end for ANY reason, the box kept an fd that would never work again, never
 * reconnected, and silently discarded every publish. From dserv it presented as a
 * watchdog frozen at the last value it managed to send -- indistinguishable from a
 * crashed box, but it needed a POWER CYCLE to recover because nothing in the
 * firmware could rebuild the socket. Observed repeatedly on 2026-07-30: box2
 * frozen at up=45 for 6+ minutes while healthy.
 *
 * Two failure classes, and conflating them is what to avoid:
 *
 *   EAGAIN/EWOULDBLOCK -- the send buffer is full and the peer has not drained it.
 *     TRANSIENT and expected: a 1 Hz status burst is ~30 frames, and the peer may
 *     legitimately be busy (dserv relaying an OTA blast, say). Tolerate a long run
 *     of these before concluding anything.
 *   anything else (ECONNRESET, EPIPE, ENOTCONN, ...) -- the connection is gone.
 *     Give up at once; retrying cannot help and every attempt is a dropped publish.
 *
 * On giving up, CLOSE the socket. That is what makes connected() tell the truth,
 * which lets the existing re-registration path (box_uplink) rebuild the uplink on
 * its own. The recovery machinery already existed -- it was simply never reached,
 * because nothing ever admitted the connection had failed. */
#define UPLINK_EAGAIN_LIMIT 200   /* ~consecutive full-buffer sends before giving up */

static unsigned uplink_eagain;
static unsigned uplink_drops;      /* lifetime, for state/dbg */

static void uplink_failed(const char *why)
{
	printk("uplink: %s -- closing socket so it can be re-established\n", why);
	if (sock >= 0) {
		zsock_close(sock);
	}
	sock = -1;
	connecting = 0;
	uplink_eagain = 0;
}

unsigned box_net_eth_uplink_drops(void) { return uplink_drops; }

int box_net_eth_send(const uint8_t *buf, int len)
{
	if (sock < 0) {
		return -1;
	}
	/* DO NOT SEND ON A CONNECT STILL IN FLIGHT. box_net_eth_connect() is
	 * deliberately non-blocking, so for a while after it returns the fd exists but
	 * the handshake has not completed. A send then fails with ENOTCONN -- which
	 * looks exactly like a hard error, and treating it as one produced a tight
	 * close/reconnect/send-too-early THRASH LOOP, printing "send error" several
	 * times a second and never recovering. That was the first version of this fix;
	 * the box went from silently dead to noisily dead. Drop the frame and let
	 * box_net_eth_connected() finish the handshake first. */
	if (connecting) {
		uplink_drops++;
		return -1;
	}
	uint32_t t0 = k_cycle_get_32();
	int n = zsock_send(sock, buf, (size_t) len, 0);
	int err = errno;
	uint32_t dt = k_cyc_to_us_floor32(k_cycle_get_32() - t0);

	send_last_us = dt;
	if (dt > send_max_us) {
		send_max_us = dt;
	}

	if (n == len) {
		uplink_eagain = 0;
		return 0;
	}

	uplink_drops++;
	if (n < 0 && (err == EAGAIN || err == EWOULDBLOCK ||
		      err == ENOTCONN || err == EINPROGRESS || err == EALREADY)) {
		/* ENOTCONN/EINPROGRESS/EALREADY belong here, not with the hard errors:
		 * they mean "not ready yet", the same as a full buffer. Classifying them
		 * as fatal is what created the reconnect thrash loop described above. */
		if (++uplink_eagain >= UPLINK_EAGAIN_LIMIT) {
			uplink_failed("peer not draining");
		}
		return -1;
	}
	/* Hard error, or a short write we cannot resume from mid-frame (the wire
	 * format is fixed-length records, so a partial one is unusable anyway). */
	uplink_failed(n < 0 ? "send error" : "short write");
	return -1;
}

/* ---- in-stack residence time ----
 * Deltas of the stack's own rx_time/tx_time accumulators (sum + count, us),
 * fetched through the public net_mgmt stats request. Everything here is
 * per-interval: subtract the previous snapshot so a 1 Hz reader sees "mean of
 * the frames moved since last second", not a boot-lifetime average that a
 * bench run can no longer move. */
#if defined(CONFIG_NET_STATISTICS_USER_API) && \
	(defined(CONFIG_NET_PKT_RXTIME_STATS) || defined(CONFIG_NET_PKT_TXTIME_STATS))

#include <zephyr/net/net_stats.h>
#include <zephyr/net/net_mgmt.h>

int box_net_eth_stack_stats(box_eth_stack_stats_t *out)
{
	static struct net_stats prev;
	static int primed;
	struct net_stats cur;

	memset(out, 0, sizeof *out);
	if (!iface ||
	    net_mgmt(NET_REQUEST_STATS_GET_ALL, iface, &cur, sizeof cur) != 0) {
		return -1;
	}

#if defined(CONFIG_NET_PKT_RXTIME_STATS)
	{
		uint64_t s = cur.rx_time.sum - (primed ? prev.rx_time.sum : 0);
		uint32_t n = cur.rx_time.count - (primed ? prev.rx_time.count : 0);

		out->rx_avg_us = n ? (uint32_t) (s / n) : 0;
		out->rx_count = n;
	}
#if defined(CONFIG_NET_PKT_RXTIME_STATS_DETAIL)
	out->rx_detail_n = MIN((int) ARRAY_SIZE(cur.rx_time_detail),
			       (int) ARRAY_SIZE(out->rx_detail_us));
	for (int i = 0; i < out->rx_detail_n; i++) {
		uint64_t s = cur.rx_time_detail[i].sum -
			     (primed ? prev.rx_time_detail[i].sum : 0);
		uint32_t n = cur.rx_time_detail[i].count -
			     (primed ? prev.rx_time_detail[i].count : 0);

		out->rx_detail_us[i] = n ? (uint32_t) (s / n) : 0;
	}
#endif
#endif

#if defined(CONFIG_NET_PKT_TXTIME_STATS)
	{
		uint64_t s = cur.tx_time.sum - (primed ? prev.tx_time.sum : 0);
		uint32_t n = cur.tx_time.count - (primed ? prev.tx_time.count : 0);

		out->tx_avg_us = n ? (uint32_t) (s / n) : 0;
		out->tx_count = n;
	}
#if defined(CONFIG_NET_PKT_TXTIME_STATS_DETAIL)
	out->tx_detail_n = MIN((int) ARRAY_SIZE(cur.tx_time_detail),
			       (int) ARRAY_SIZE(out->tx_detail_us));
	for (int i = 0; i < out->tx_detail_n; i++) {
		uint64_t s = cur.tx_time_detail[i].sum -
			     (primed ? prev.tx_time_detail[i].sum : 0);
		uint32_t n = cur.tx_time_detail[i].count -
			     (primed ? prev.tx_time_detail[i].count : 0);

		out->tx_detail_us[i] = n ? (uint32_t) (s / n) : 0;
	}
#endif
#endif

	prev = cur;
	primed = 1;
	return 0;
}

#else /* stats not in this build */

int box_net_eth_stack_stats(box_eth_stack_stats_t *out)
{
	memset(out, 0, sizeof *out);
	return -1;
}

#endif

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
/* Send SEVERAL %reg/%match lines over ONE connection.
 *
 * Registration used to open a separate short-lived TCP connection per line.
 * That races dserv's per-client teardown: measured 2026-07-26 on an RW612,
 * `%getmatch <box> 5010` showed dserv holding only ONE of the three matches --
 * which one depended purely on timing (the first without inter-line delays, the
 * last with them). The box is then left PUBLISHING BUT DEAF: state/* keeps
 * flowing, cmd/* never arrives, and it looks like a perfectly healthy box.
 *
 * Sending them down a single connection lands all of them, verified by hand
 * against the same dserv. Returns the number of lines that were NOT accepted.
 */
int box_net_eth_send_commands(const uint8_t dserv_ip[4], uint16_t port,
			      const char *const *cmds, int ncmds)
{
	int fails = ncmds;
	int s = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (s < 0) {
		return fails;
	}
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
	if (zsock_poll(&pfd, 1, 300) <= 0) {
		goto out;
	}
	int err = 0;
	socklen_t elen = sizeof err;
	zsock_getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen);
	if (err != 0) {
		goto out;
	}

	fails = 0;
	for (int i = 0; i < ncmds; i++) {
		size_t len = strlen(cmds[i]);

		if (zsock_send(s, cmds[i], len, 0) != (int) len) {
			fails++;
			continue;
		}
		pfd.events = ZSOCK_POLLIN;
		if (zsock_poll(&pfd, 1, 300) <= 0) {
			fails++;
			continue;
		}
		char rep[16] = { 0 };
		int n = zsock_recv(s, rep, sizeof rep - 1, 0);

		if (!(n > 0 && rep[0] == '1')) {
			fails++;
		}
	}
out:
	zsock_close(s);
	return fails;
}

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
