/*
 * box_net_eth.c -- RW612/Zephyr Ethernet transport over BSD-style sockets.
 */
#include "box_net_eth.h"
#include "box_event.h"
#include "box_gpio.h"                   /* box_gpio_now_us: arrival stamps (+29) */
#include "box_uplink.h"                 /* BOX_UPLINK_RX_* kinds (+29)           */
#include "box_fast.h"                   /* the schedule fast path (+29)          */
#include "dserv_msg.h"                  /* DSERV_MSG_LEN: the wire record size */

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
/* Doom flag for srv_conn: 0 = live, 1 = dead (EOF/RST seen, awaiting close),
 * 2 = a reaper has claimed the close and is mid-teardown. atomic_t because the
 * claim is a CAS -- two reapers exist (the RX-wake thread and, if that thread
 * is itself declared dead, the watchdog below), and a CAS is what guarantees a
 * doomed fd is closed EXACTLY once no matter which of them gets there. Why the
 * service loop no longer closes it directly is the subject of the RX-wake
 * section's comment. */
static atomic_t srv_doom;
/* Unsent tail of a frame the socket half-took -- partial-frame custody; the
 * mechanism and its rationale live with txp_flush() in the send section. */
static uint8_t txp_buf[DSERV_MSG_LEN];
static int     txp_len;

/* ---- config server: dserv connects BACK here with our subscribed datapoints ----
 * Two sockets in opposite directions is dserv's subscription model, not a choice:
 * %reg names an ip:port and dserv opens a connection to it. See the header. */
static int srv_listen = -1;     /* listening socket (BOX_ETH_CFG_PORT)          */
/* volatile: sampled by the RX-wake thread every loop pass. The compiler almost
 * certainly cannot hoist the load anyway (exported functions in this file write
 * it, so any opaque call clobbers it) -- but box_ain.c's `running` shows how
 * close this codebase already came to that trap, and "almost certainly" is not
 * a property to build a wake path on. Disassembly of v0.4.0+11 was checked and
 * the load was NOT hoisted, so this is insurance, not the bug fix. */
static volatile int srv_conn = -1;   /* dserv's accepted connect-back           */
static int srv_fresh;           /* one-shot: report BOX_NET_RESET after accept  */
/* ---- inbound instrumentation ----
 * The split changed meaning at +29, when the reader thread took over recv:
 *   wake_us  queue residence -- frame enqueued at arrival -> service loop
 *            popped it (ordinary frames only; the schedule classes are
 *            handled AT arrival and never ride the queue)
 *   recv_us  cost of the reader's zsock_recv that returned the bytes
 * wake_us is exactly the latency the fast path removed from arming: watch it
 * grow with fat service passes while at/at_abs stay unaffected. The 1 Hz
 * stats window with no pops reports UINT32_MAX (-1 on the wire) -- "nothing
 * queued", distinct from "queue was fast". (The pre-29 signal/sequence
 * machinery this replaced lived and died by the same distinction; see git
 * for its story.) */
static uint32_t wake_window;                 /* pops since last stats read */
static uint32_t wake_last_us, wake_max_us;
static uint32_t recv_last_us, recv_max_us;

void box_net_eth_rx_stats(uint32_t *wake_us, uint32_t *wake_max,
			  uint32_t *recv_us, uint32_t *recv_max)
{
	/* Caller (the 1 Hz status block) and the consumer (box_net_eth_poll2) are
	 * the same thread, so read-then-clear needs no lock. recv_* is written on
	 * the reader thread now: a torn read of a diagnostic uint32 is harmless. */
	if (wake_us)   *wake_us   = wake_window ? wake_last_us : UINT32_MAX;
	wake_window = 0;
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

/* Close a DOOMED connect-back, exactly once. The CAS is the claim: whichever
 * reaper wins moves 1 -> 2, does the teardown, and only then re-arms the flag
 * to 0. The loser sees 2 (or 0) and walks away. srv_conn goes to -1 BEFORE the
 * flag clears so the accept gate in server_service() can never see "no doom,
 * old fd still present" and double-book the slot. */
static void rx_wake_watchdog(void);     /* defined with the RX-wake section */

static void srv_reap(void)
{
	if (!atomic_cas(&srv_doom, 1, 2)) {
		return;
	}
	int fd = srv_conn;

	srv_conn = -1;
	if (fd >= 0) {
		zsock_close(fd);
	}
	atomic_set(&srv_doom, 0);
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
	txp_len = 0;                    /* no half-sent frame carries into a new session */
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
	/* Liveness first: this runs every service pass on every eth-capable
	 * board, which is exactly the guarantee the RX-wake self-heal needs
	 * (it rate-limits itself to 1 Hz inside). */
	rx_wake_watchdog();

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
	/* A doomed connect-back is DOWN even though the fd still exists: the reg
	 * watchdog keys re-registration off this, and "fd present but dead" is
	 * exactly the lie that kept boxes deaf before (see box_net_eth_send's
	 * header comment for the uplink-side version of the same lesson). */
	return srv_conn >= 0 && atomic_get(&srv_doom) == 0;
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
/* ---- how this thread dies, and why it now can't stay dead ----
 *
 * Twice this thread has been found silently dead: dbg/wake_us frozen while
 * dbg/recv_us kept updating, every dserv->box command +~1 ms (the loop's
 * box_event_wait(K_MSEC(1)) fallback doing its job). The first time was blamed
 * on stack overflow and the stack raised 1024 -> 2048. That diagnosis is now
 * DISPROVEN: every board runs hardware stack protection (PSPLIM on the M33s,
 * MPU guards on the RT10xx), the fatal handler is the Zephyr default which
 * HALTS THE WHOLE SYSTEM, and the failed boots kept publishing -- so no fault
 * of any kind fired, and 2048 was never the cure (v0.4.0+11 died the same way
 * on 2 of 4 boots of box02, 2026-08-01, with 2048 in place).
 *
 * What the evidence leaves: the loop body was checked in the shipped binary
 * (srv_conn IS reloaded every pass -- no hoisting), k_msleep and k_poll are
 * timeout-bounded and cannot pend forever, no fault fired, nothing aborts the
 * thread. The one unbounded wait in its path is zsock_poll's per-fd fdtable
 * mutex, taken K_FOREVER -- and Zephyr's zvfs_reserve_fd() re-inits that mutex
 * (k_mutex_init) whenever a freed fd SLOT is recycled. A thread pended on the
 * old mutex at that instant is orphaned: the wait queue is wiped under it, it
 * holds no timeout, and nothing will ever ready it again. Silent, permanent,
 * and invisible to every datapoint except the wake path it was providing.
 *
 * Boot is exactly when that window exists: each repeated %reg makes dserv tear
 * down the old connect-back and open a new one (Dataserver.cpp
 * add_new_send_client: "shut down any existing registration under this key"),
 * so the service loop used to close srv_conn -- WHILE THIS THREAD POLLS IT --
 * and the very next zsock_socket()/accept() recycles the slot. Whether the
 * kernel-level interleaving lands on the fatal cell is timing; ~50%/boot says
 * it lands often. After registration settles the churn stops, which is why a
 * box that boots healthy stays healthy.
 *
 * Three layers of fix, because the wedge is a KERNEL property we can only
 * stay out of, not repair:
 *   1. DEFERRED CLOSE. The service loop never closes srv_conn any more; it
 *      sets srv_doom and this thread -- the poller -- does the close between
 *      its own polls (srv_reap). The fd it watches can no longer be freed and
 *      recycled under its zsock_poll, which removes the exposure rather than
 *      shrinking it.
 *   2. HEARTBEAT + RESPAWN. rx_beat ticks every pass (<= ~221 ms apart by
 *      construction: 20 ms nap / 200 ms poll / 1 ms yield). The 1 Hz watchdog
 *      (rx_wake_watchdog, run off server_service like the listener's own
 *      retry) treats >= 5 s of frozen beat as death and starts the SPARE
 *      thread on its own stack. The wedged one is left pended forever, NOT
 *      k_thread_abort()ed: it provably holds nothing (every pend site in its
 *      loop is lock-free by then), and aborting a thread off a wait queue
 *      that was re-initialised dlist-unlinks through stale pointers -- the
 *      cure would be the corruption. One spare, not a pool: this covers the
 *      boot window; a second death in one uptime means something new and
 *      should stay visible (dbg/ethrx_age_ms grows, RTT +1 ms, box still
 *      works).
 *   3. If BOTH incarnations are dead the watchdog reaps doomed fds itself so
 *      the box cannot go deaf waiting for a closer that will never come --
 *      degraded to the 1 ms timeout, never dead.
 * dbg/ethrx_age_ms / dbg/ethrx_respawns / dbg/ethrx_stack_free are the 1 Hz
 * proof of which layer is doing work; the stack number is there so the next
 * "raise it" has a measurement instead of a guess. */
static K_THREAD_STACK_DEFINE(eth_rx_stack, 3072);
static K_THREAD_STACK_DEFINE(eth_rx_stack2, 3072);   /* the spare's own stack --
					* the wedged thread's stack stays its
					* grave and is never reused */
static struct k_thread eth_rx_thread, eth_rx_thread2;
static volatile uint32_t rx_beat;    /* liveness: bumped every loop pass */
static uint32_t rx_respawns;         /* 0 or 1; published as dbg/ethrx_respawns */
static uint8_t  rx_started;
/* Watchdog bookkeeping (rx_wake_watchdog below). last_change doubles as the
 * basis of dbg/ethrx_age_ms, so it is stamped at spawn, not first check. */
static uint32_t rx_wd_last_beat;
static int64_t  rx_wd_last_change;
static uint8_t  rx_wd_gave_up;

/* ---- +29: this thread is a READER now, not a doorbell. ----
 *
 * Until +28 the config link was drained by the SERVICE LOOP: this thread only
 * poked it awake. That put every inbound command behind whatever pass the
 * loop happened to be in -- measured as a suspiciously constant ~3.7 ms
 * loop_max_us -- and a 2 ms-spaced `at` burst landing inside such a pass was
 * armed after its first targets had already passed: past-due slots fire
 * ASAP back-to-back and two pulses merge into one. The soaks put that at a
 * few percent of sessions, with the sched ledger reading acc==fired and
 * NOTHING dropped anywhere: pure processing latency, which no RX-buffer
 * geometry could touch (+28 killed the MAC-ring underrun class; this is the
 * other half).
 *
 * Now the frames are received HERE, at priority 2 -- the +28 lattice seat:
 * arrival is creation-adjacent, below the sampling path, above service --
 * stamped on recv, framed locally, and offered to box_main_fast_frame(),
 * which arms the schedule classes (at / at_abs / the obs epoch) inline,
 * microseconds after the wire. Everything else is queued, stamp attached,
 * for the service loop to dispatch exactly as before. Arming latency is now
 * independent of the loop's pass length by construction.
 *
 * Ownership moves with the work: THIS thread is the only reader of srv_conn
 * and the only setter of srv_doom (recv EOF/hard-error dooms; the reap was
 * already here) -- the close-under-poll hazard shrinks to one thread. */
typedef struct {
	uint64_t arr_us;
	uint8_t  f[DSERV_MSG_LEN];
} eth_inq_item_t;
K_MSGQ_DEFINE(eth_inq, sizeof(eth_inq_item_t), 64, 8);
static uint32_t inq_drop;            /* full-queue drops (dbg/ethin_q_drop) */
static uint32_t inq_max;             /* depth watermark  (dbg/ethin_q_max)  */
static dserv_framer_t reader_framer;
static uint64_t reader_arr_us;       /* stamp for the frames of one recv    */

static void reader_on_frame(const uint8_t *frame, void *ud)
{
	ARG_UNUSED(ud);
	if (box_main_fast_frame(frame, reader_arr_us)) {
		return;                        /* consumed at arrival */
	}
	eth_inq_item_t it;

	it.arr_us = reader_arr_us;
	memcpy(it.f, frame, DSERV_MSG_LEN);
	if (k_msgq_put(&eth_inq, &it, K_NO_WAIT) != 0) {
		/* 64 deep and the loop still hasn't drained: count it loudly.
		 * These are HOST COMMANDS; a nonzero counter is a real loss. */
		inq_drop++;
	} else {
		uint32_t u = (uint32_t) k_msgq_num_used_get(&eth_inq);

		if (u > inq_max) {
			inq_max = u;
		}
	}
	box_event_signal();
}

static void eth_rx_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	int last_fd = -1;

	for (;;) {
		rx_beat++;
		/* Reap first: a doomed fd must never be polled again, and closing
		 * it HERE -- between this thread's own zsock_poll calls -- is the
		 * whole deferred-close design. */
		if (atomic_get(&srv_doom) != 0) {
			srv_reap();
			continue;
		}
		int fd = srv_conn;               /* sampled; -1 while disconnected */
		if (fd < 0) {
			k_msleep(20);            /* nothing to watch yet */
			continue;
		}
		if (fd != last_fd) {
			/* new session: never carry a half frame across sockets */
			dserv_framer_reset(&reader_framer);
			last_fd = fd;
		}
		struct zsock_pollfd pfd = { .fd = fd, .events = ZSOCK_POLLIN };
		/* Bounded wait so a socket swap (reconnect) is picked up promptly.
		 * The bound is also what makes rx_beat a real liveness signal. */
		if (zsock_poll(&pfd, 1, 200) <= 0) {
			continue;
		}
		for (;;) {
			uint8_t rb[512];
			uint32_t r0 = k_cycle_get_32();
			int n = zsock_recv(fd, rb, sizeof rb, ZSOCK_MSG_DONTWAIT);

			if (n > 0) {
				uint32_t d = k_cyc_to_us_floor32(k_cycle_get_32() - r0);

				recv_last_us = d;
				if (d > recv_max_us) {
					recv_max_us = d;
				}
				reader_arr_us = box_gpio_now_us();
				dserv_framer_feed(&reader_framer, rb, (uint32_t) n,
						  reader_on_frame, NULL);
				continue;        /* drain until EAGAIN */
			}
			if (n == 0 ||
			    (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
				/* dserv closed (or RST) the config link. DOOM it --
				 * the reap above closes it between OUR OWN polls,
				 * same single-owner discipline as before, now with
				 * both the setter and the reaper on this thread. */
				atomic_set(&srv_doom, 1);
			}
			break;
		}
	}
}

/* Priority 2 (+29): the arrival seat. Was 8 when this thread only rang the
 * doorbell; a reader that ARMs schedules must preempt the service loop, or
 * the fast path would wait behind exactly the pass it exists to bypass. */
static void rx_wake_spawn(struct k_thread *t, k_thread_stack_t *stk,
			  size_t sz, const char *name)
{
	k_thread_create(t, stk, sz, eth_rx_thread_fn, NULL, NULL, NULL,
			2, 0, K_NO_WAIT);
	k_thread_name_set(t, name);
}

void box_net_eth_rx_wake_start(void)
{
	rx_wd_last_change = k_uptime_get();   /* so ethrx_age_ms starts honest */
	rx_wake_spawn(&eth_rx_thread, eth_rx_stack,
		      K_THREAD_STACK_SIZEOF(eth_rx_stack), "extio_ethrx");
	rx_started = 1;
}

/* 1 Hz liveness check, called from box_net_eth_server_service() so it runs on
 * every eth-capable board without main.c plumbing (the reg watchdog precedent:
 * detection and repair live next to the thing they guard). last_change is
 * uptime-of-last-observed-beat-CHANGE, so ethrx_age_ms reads 0..~1000 healthy
 * and simply grows once the thread stops -- the number that was missing on
 * 2026-08-01 when wake_us's plausible garbage sent the hunt sideways. */
#define RX_WAKE_STALL_MS 5000    /* >= 22x the worst healthy beat interval; an
				  * OTA flash blast stalls the WHOLE box (SL
				  * included, wd_skipped counts it), so a gap
				  * only this thread can hit means it is gone */

static void rx_wake_watchdog(void)
{
	static int64_t next_check;
	int64_t now = k_uptime_get();

	if (!rx_started || now < next_check) {
		return;
	}
	next_check = now + 1000;

	uint32_t b = rx_beat;

	if (b != rx_wd_last_beat) {
		rx_wd_last_beat = b;
		rx_wd_last_change = now;
		return;
	}
	if (now - rx_wd_last_change < RX_WAKE_STALL_MS) {
		return;
	}
	if (rx_respawns == 0) {
		rx_respawns = 1;
		rx_wd_last_change = now;         /* give the spare its own 5 s */
		printk("ethrx: wake thread frozen %d ms -- spawning spare "
		       "(wedged one left pended; commands ride the 1 ms timeout meanwhile)\n",
		       RX_WAKE_STALL_MS);
		rx_wake_spawn(&eth_rx_thread2, eth_rx_stack2,
			      K_THREAD_STACK_SIZEOF(eth_rx_stack2), "extio_ethrx2");
		return;
	}
	/* Both incarnations dead. Do the one job that cannot wait for them --
	 * closing doomed fds -- so the box degrades to +1 ms commands instead of
	 * going deaf. Racing a poller is safe here BECAUSE they are wedged: a
	 * thread pended forever is, by definition, not inside a poll window. */
	srv_reap();
	if (!rx_wd_gave_up) {
		rx_wd_gave_up = 1;
		printk("ethrx: spare wake thread ALSO frozen -- no more respawns; "
		       "running on the 1 ms timeout fallback (dbg/ethrx_age_ms grows)\n");
	}
}

void box_net_eth_rx_health(uint32_t *age_ms, uint32_t *respawns, uint32_t *stack_free)
{
	if (age_ms) {
		*age_ms = rx_started ?
			(uint32_t) (k_uptime_get() - rx_wd_last_change) : 0;
	}
	if (respawns) {
		*respawns = rx_respawns;
	}
	if (stack_free) {
		/* Real headroom, not a guess: needs CONFIG_INIT_STACKS (prj.conf)
		 * for the watermark fill. Reports the ACTIVE incarnation. */
		size_t unused = 0;
		struct k_thread *t = rx_respawns ? &eth_rx_thread2 : &eth_rx_thread;

		*stack_free = (rx_started &&
			       k_thread_stack_space_get(t, &unused) == 0) ?
			(uint32_t) unused : 0;
	}
}

int box_net_eth_poll2(uint8_t *buf, int max, int *len, uint64_t *arr_us)
{
	*len = 0;
	*arr_us = 0;
	/* A fresh connect-back means dserv is listening NOW: reset the framer and
	 * let main() describe the box. This is the Ethernet analogue of USB's
	 * "host opened the pipe" and is deliberately reported on CONNECT.
	 * (Reporting it only on close -- the original behaviour -- fired the
	 * announce burst into an already-dead socket, so a box on Ethernet never
	 * published its manifest at all.) */
	if (srv_fresh) {
		srv_fresh = 0;
		return BOX_UPLINK_RX_RESET;
	}

	/* +29: inbound frames come from the reader thread's queue -- receive,
	 * framing, arrival stamping and the schedule fast path all happened at
	 * priority 2 the moment the bytes landed. What is left here is delivery
	 * of the ordinary frames to the dispatch path, plus the one job the
	 * reader cannot do: EOF detection on the CLIENT socket (it carries
	 * nothing inbound, but a dserv restart parks it in CLOSE_WAIT forever
	 * and the arbiter would never reconnect -- the box goes silently
	 * write-only, the very failure this file exists to fix). */
	eth_inq_item_t it;

	if (max >= (int) DSERV_MSG_LEN &&
	    k_msgq_get(&eth_inq, &it, K_NO_WAIT) == 0) {
		memcpy(buf, it.f, DSERV_MSG_LEN);
		*len = (int) DSERV_MSG_LEN;
		*arr_us = it.arr_us;
		/* dbg/wake_us, reinterpreted (+29): queue residence time --
		 * enqueue at arrival to pop here. Small when the loop is
		 * keeping up; grows with exactly the pass length that used to
		 * delay ARMING. The schedule classes no longer ride it. */
		uint64_t now = box_gpio_now_us();

		if (it.arr_us && now > it.arr_us) {
			uint32_t w = (uint32_t) (now - it.arr_us);

			wake_last_us = w;
			if (w > wake_max_us) {
				wake_max_us = w;
			}
			wake_window++;
		}
		return BOX_UPLINK_RX_FRAME;
	}

	if (sock >= 0) {
		struct zsock_pollfd pf = { .fd = sock, .events = ZSOCK_POLLIN };

		if (zsock_poll(&pf, 1, 0) > 0 &&
		    (pf.revents & (ZSOCK_POLLIN | ZSOCK_POLLHUP | ZSOCK_POLLERR))) {
			/* readable or hung up can only mean EOF: dserv never
			 * sends anything down the client socket */
			zsock_close(sock);
			sock = -1;
			connecting = 0;
		}
	}
	return BOX_UPLINK_RX_NONE;
}

void box_net_eth_inq_stats(uint32_t *drop, uint32_t *max_depth)
{
	if (drop) {
		*drop = inq_drop;
	}
	if (max_depth) {
		*max_depth = inq_max;
	}
}

/* Legacy byte-run poll: dead on Ethernet since +29 (the service loop uses
 * poll2; frames never surface as raw bytes here). Kept because the uplink
 * vtable requires .poll and honesty requires it not to invent data. */
int box_net_eth_poll(uint8_t *buf, int max)
{
	ARG_UNUSED(buf); ARG_UNUSED(max);
	if (srv_fresh) {
		srv_fresh = 0;
		return BOX_NET_RESET;
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

/* ---- partial-frame custody (txp_*) ----
 *
 * The wire format is fixed 128-byte records with NO framing bytes to resync on
 * (dserv's '>' reader counts, it does not scan), so a frame that goes out in
 * two pieces MUST NOT have another writer's frame spliced between the pieces
 * -- that desyncs every record that follows. And two writers now exist: the
 * publisher thread (gathered sends, box_pub.c) and the service loop (bypass
 * mode, plus the stage-1 direct-send sites).
 *
 * So: the moment the socket takes PART of a frame, the transport takes custody
 * of that frame's unsent tail. Every send path flushes the tail before writing
 * anything new, and the partially-taken frame is reported to its caller as
 * accepted -- it is no longer the caller's to retry or drop. The wire stays
 * record-aligned no matter which thread shows up next (box_uplink's lock
 * serializes the calls themselves).
 *
 * This replaces the old short-write policy -- close and reconnect, "a partial
 * one is unusable anyway" -- which was right only while no caller could
 * resume. Custody resumes. (txp_buf/txp_len are declared with the session
 * state at the top of the file; connect() clears them for a new session.) */
static void uplink_failed(const char *why);

/* 0 = clear, -1 = tail still pending (buffer full), -2 = session died. */
static int txp_flush(void)
{
	while (txp_len > 0) {
		int n = zsock_send(sock, txp_buf, (size_t) txp_len, 0);

		if (n < 0) {
			int err = errno;

			if (err == EAGAIN || err == EWOULDBLOCK) {
				return -1;
			}
			uplink_failed("send error (pending tail)");
			return -2;
		}
		if (n == 0) {
			return -1;
		}
		txp_len -= n;
		if (txp_len > 0) {
			memmove(txp_buf, txp_buf + n, (size_t) txp_len);
		}
	}
	return 0;
}

static void txp_stash(const uint8_t *tail, int len)
{
	memcpy(txp_buf, tail, (size_t) len);
	txp_len = len;
}

static void uplink_failed(const char *why)
{
	printk("uplink: %s -- closing socket so it can be re-established\n", why);
	if (sock >= 0) {
		zsock_close(sock);
	}
	sock = -1;
	connecting = 0;
	uplink_eagain = 0;
	txp_len = 0;                    /* the half-sent frame died with the session */
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
	int pf = txp_flush();

	if (pf == -2) {
		uplink_drops++;
		return -1;
	}
	if (pf == -1) {
		/* The socket still owes a prior frame's tail: nothing new may go out
		 * ahead of it. Same accounting and strike logic as a full buffer. */
		uplink_drops++;
		if (++uplink_eagain >= UPLINK_EAGAIN_LIMIT) {
			uplink_failed("peer not draining");
		}
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
	if (n > 0) {
		/* Short write: the socket took part of the frame, so the frame is no
		 * longer droppable -- take custody of the tail (txp_*) and report it
		 * sent. The old policy closed the connection here. */
		txp_stash(buf + n, len - n);
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
	if (n == 0) {
		/* send() taking zero bytes without an error is a full buffer in
		 * different clothes. */
		if (++uplink_eagain >= UPLINK_EAGAIN_LIMIT) {
			uplink_failed("peer not draining");
		}
		return -1;
	}
	uplink_failed("send error");
	return -1;
}

/* Byte-granular send for GATHERED frames (box_pub.c). `len` must be a multiple
 * of DSERV_MSG_LEN. Returns bytes accepted -- ALWAYS frame-aligned, because a
 * partially-taken frame is stashed (txp_*) and reported as accepted -- or 0
 * when the buffer is full (caller retries after a pause), or -1 when there is
 * no usable session (caller drops the remainder, exactly what every frame got
 * when the loop sent into a dead socket). */
int box_net_eth_send_stream(const uint8_t *buf, int len)
{
	if (sock < 0 || connecting) {
		return -1;
	}
	int pf = txp_flush();

	if (pf == -2) {
		return -1;
	}
	if (pf == -1) {
		if (++uplink_eagain >= UPLINK_EAGAIN_LIMIT) {
			uplink_failed("peer not draining");
			return -1;
		}
		return 0;
	}
	uint32_t t0 = k_cycle_get_32();
	int n = zsock_send(sock, buf, (size_t) len, 0);
	int err = errno;
	uint32_t dt = k_cyc_to_us_floor32(k_cycle_get_32() - t0);

	send_last_us = dt;
	if (dt > send_max_us) {
		send_max_us = dt;
	}

	if (n < 0) {
		if (err == EAGAIN || err == EWOULDBLOCK ||
		    err == ENOTCONN || err == EINPROGRESS || err == EALREADY) {
			if (++uplink_eagain >= UPLINK_EAGAIN_LIMIT) {
				uplink_failed("peer not draining");
				return -1;
			}
			return 0;
		}
		uplink_failed("send error");
		return -1;
	}
	if (n == 0) {
		if (++uplink_eagain >= UPLINK_EAGAIN_LIMIT) {
			uplink_failed("peer not draining");
			return -1;
		}
		return 0;
	}
	uplink_eagain = 0;

	int rem = n % DSERV_MSG_LEN;

	if (rem) {
		txp_stash(buf + n, DSERV_MSG_LEN - rem);
		n += DSERV_MSG_LEN - rem;        /* that frame is ours now: count it sent */
	}
	return n;
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
