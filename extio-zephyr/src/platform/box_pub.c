/*
 * box_pub.c -- see box_pub.h for the why; this file is the how.
 *
 * Two k_msgqs of {enqueue-time, frame} items, one binary-semaphore wake (the
 * box_event idiom), one thread. The drain order is the contract: the event
 * queue is emptied before every bulk gather, and re-checked after each one, so
 * the most an event waits behind bulk is ONE in-flight send.
 */
#include "box_pub.h"
#include "box_uplink.h"
#include "dserv_msg.h"

#include <string.h>

/* Depths. EVENT is sized for the worst legitimate burst (a chord storm across
 * 30 pins plus scheduled fires plus replies) with the wire briefly stalled;
 * BULK must hold one full announce burst so a manifest never self-drops --
 * the old pubq was 40 for the ~28-frame status burst, announce is bigger.
 * If dbg/pub_bulk_hwm ever grazes the depth, grow it; RAM is 132 B/slot. */
#define PUB_EVQ_DEPTH   64
#define PUB_BULKQ_DEPTH 96

struct pub_item {
	uint32_t t_cyc;                  /* k_cycle_get_32() at enqueue, for wait_us */
	uint8_t  f[DSERV_MSG_LEN];
};

K_MSGQ_DEFINE(pub_evq,   sizeof(struct pub_item), PUB_EVQ_DEPTH,   4);
K_MSGQ_DEFINE(pub_bulkq, sizeof(struct pub_item), PUB_BULKQ_DEPTH, 4);

/* Binary, not counting: the thread drains until empty on every wake, so one
 * pending give covers any number of enqueues (box_event.c, same reasoning). */
static K_SEM_DEFINE(pub_kick, 0, 1);

static box_pub_stats_t st;
static uint8_t pub_bypass_on;
static uint8_t pub_gather_n = BOX_PUB_GATHER_MAX;
static volatile uint8_t pub_busy;    /* mid-gather flag, read by box_pub_flush */

/* ---- producers ---- */

static int enqueue(struct k_msgq *q, const uint8_t *f)
{
	struct pub_item it;

	it.t_cyc = k_cycle_get_32();
	memcpy(it.f, f, DSERV_MSG_LEN);
	if (k_msgq_put(q, &it, K_NO_WAIT) != 0) {
		return -1;
	}
	uint16_t used = (uint16_t) k_msgq_num_used_get(q);

	if (q == &pub_evq)   { if (used > st.ev_hwm)   st.ev_hwm = used; }
	else                 { if (used > st.bulk_hwm) st.bulk_hwm = used; }
	k_sem_give(&pub_kick);
	return 0;
}

int box_pub_event(const uint8_t *f)
{
	if (pub_bypass_on) {
		return box_uplink_send(f, DSERV_MSG_LEN);
	}
	if (enqueue(&pub_evq, f) != 0) {
		/* Full means >64 frames already waiting on a stalled wire, where the
		 * transport would be dropping these too (box_net_eth.c EAGAIN path).
		 * Drop-NEWEST and count -- blocking the producer here would hand the
		 * stall right back to the loop the queue exists to protect. */
		st.ev_dropped++;
		return -1;
	}
	return 0;
}

int box_pub_bulk(const uint8_t *f)
{
	if (pub_bypass_on) {
		return box_uplink_send(f, DSERV_MSG_LEN);
	}
	if (enqueue(&pub_bulkq, f) != 0) {
		/* Bulk is last-value-wins by definition, so the OLDEST frame is the
		 * least valuable one in the queue -- displace it (the old pubq
		 * policy). The get can race the publisher's own get; the worst case
		 * is one extra frame dropped, and it is counted either way. */
		struct pub_item scratch;

		(void) k_msgq_get(&pub_bulkq, &scratch, K_NO_WAIT);
		st.bulk_dropped++;
		if (enqueue(&pub_bulkq, f) != 0) {
			return -1;
		}
	}
	return 0;
}

/* ---- knobs / stats ----
 *
 * What the knobs are for, and what is already known (carried over from the
 * loop-drained pubq this module replaced):
 *
 * SETTLED 2026-07-27: drain granularity does not move two-box skew. Three arms
 * (inline / 2 per pass / 8 per pass), 5 interleaved reps each, 100 shots per
 * run: skew medians 15.2 / 14.3 / 15.6 us, p90 37.8 / 36.2 / 37.0 us --
 * indistinguishable, within-arm spread far exceeding any between-arm
 * difference. (An earlier 5-pair run showing 2/pass ~4 us worse on p90 did NOT
 * replicate; p90 from 100 shots carries ~10 samples of information.) Stamps are
 * ISR-side, so HOW frames drain cannot touch them; what draining changes is
 * LOOP STALL, i.e. inbound command latency. That is the metric bypass/gather
 * exist to measure -- dbg/pub_wait_max_us and cmd RTT under an announce storm,
 * interleaved arms, same method. */

void box_pub_set_bypass(int on) { pub_bypass_on = (on != 0); }
int  box_pub_bypass(void)       { return pub_bypass_on; }

void box_pub_set_gather(int n)
{
	if (n < 1)                  n = 1;
	if (n > BOX_PUB_GATHER_MAX) n = BOX_PUB_GATHER_MAX;
	pub_gather_n = (uint8_t) n;
}
int box_pub_gather(void) { return pub_gather_n; }

void box_pub_get_stats(box_pub_stats_t *out) { *out = st; }

int box_pub_flush(k_timeout_t timeout)
{
	k_timepoint_t end = sys_timepoint_calc(timeout);

	for (;;) {
		if (k_msgq_num_used_get(&pub_evq) == 0 &&
		    k_msgq_num_used_get(&pub_bulkq) == 0 && !pub_busy) {
			return 0;
		}
		if (sys_timepoint_expired(end)) {
			return -1;
		}
		k_sem_give(&pub_kick);           /* make sure the drain is awake */
		k_msleep(1);
	}
}

/* ---- the publisher thread ---- */

static uint8_t gbuf[BOX_PUB_GATHER_MAX * DSERV_MSG_LEN];

/* Pull up to `max` frames from q into gbuf; returns the frame count and the
 * oldest frame's wait in *wait_us (32-bit cycle delta: wraps at ~28 s, which
 * only misreads frames that sat through a dead uplink -- a state the drop
 * counters already make visible). */
static int gather_from(struct k_msgq *q, int max, uint32_t *wait_us)
{
	struct pub_item it;
	int n = 0;

	while (n < max && k_msgq_get(q, &it, K_NO_WAIT) == 0) {
		if (n == 0) {
			*wait_us = k_cyc_to_us_floor32(k_cycle_get_32() - it.t_cyc);
		}
		memcpy(gbuf + n * DSERV_MSG_LEN, it.f, DSERV_MSG_LEN);
		n++;
	}
	return n;
}

/* How many consecutive no-progress retries before a gather gives up, at 1 ms
 * each. Generous against ordinary backpressure -- a host that pauses for a
 * quarter second still loses nothing -- and short enough that a transport which
 * has genuinely stopped draining cannot hold this thread indefinitely. */
#define PUB_STALL_RETRIES 250

static void send_gather(int nframes)
{
	int glen = nframes * DSERV_MSG_LEN;
	int sent = 0;
	int stuck = 0;

	while (sent < glen) {
		int n = box_uplink_send_stream(gbuf + sent, glen - sent);

		if (n < 0) {
			/* Uplink down or hard-failed: the remaining frames go the way
			 * every frame went when the loop sent them into a dead socket --
			 * dropped and counted, never blocked on. */
			st.wire_dropped += (uint32_t) ((glen - sent) / DSERV_MSG_LEN);
			return;
		}
		sent += n;
		if (n > 0) {
			stuck = 0;              /* the budget is per stall, not per gather */
		}
		if (sent < glen) {
			/* Socket/ring full mid-gather. The transport owns any partial
			 * frame (box_net_eth.c txp_*), and progress is frame-aligned.
			 * 1 ms is one TCP drain opportunity, not a tuning knob.
			 *
			 * THIS USED TO BE UNBOUNDED, on the strength of a comment saying
			 * "its strike counter bounds how long a dead peer keeps us here".
			 * That is true of box_net_eth.c, which calls uplink_strike() at
			 * three sites and eventually returns -1. box_net_usb.c contains
			 * no strike logic at all: a full TX ring is a plain `return 0`,
			 * for ever, so on USB the only exit above was one that transport
			 * never takes. A wedged pipe therefore pinned this thread in a
			 * 1 kHz retry with no counter moving anywhere -- silent, because
			 * every drop counter here is on a path it was not taking.
			 *
			 * Give up like the hard-failure branch does, and count it the
			 * same way. Telemetry is the right thing to lose when a
			 * transport stops draining; the thread is not. */
			if (++stuck > PUB_STALL_RETRIES) {
				st.wire_dropped += (uint32_t) ((glen - sent) / DSERV_MSG_LEN);
				st.stalled_out++;
				return;
			}
			k_msleep(1);
		}
	}
	st.gathers++;
	st.gather_frames += (uint32_t) nframes;
}

static void pub_run(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	for (;;) {
		/* The timeout is the re-check cadence for an uplink coming back or a
		 * bypass toggle -- NOT a polling loop for frames, which always kick. */
		(void) k_sem_take(&pub_kick, K_MSEC(50));

		if (pub_bypass_on) {
			continue;                /* producers are sending inline */
		}
		if (!box_uplink_ready()) {
			continue;                /* frames wait; bulk drop-oldest bounds it */
		}

		for (;;) {
			uint32_t w = 0;
			int n = gather_from(&pub_evq, pub_gather_n, &w);

			if (n == 0) {
				n = gather_from(&pub_bulkq, pub_gather_n, &w);
				if (n == 0) {
					break;           /* both empty: back to the semaphore */
				}
			}
			st.wait_last_us = w;
			if (w > st.wait_max_us) {
				st.wait_max_us = w;
			}
			pub_busy = 1;
			send_gather(n);
			pub_busy = 0;
			/* Loop re-checks the event queue before any more bulk. */
		}
	}
}

/* Priority 3: below main (0), below the RW612's pinned mcp320x acquisition
 * thread (1, boards/frdm_rw612.conf) and the analog sampler (2, box_ain.c) --
 * so a long bulk drain can never delay a sweep -- and above reg (7) / ethrx
 * (8). See box_pub.h for why below-main is the load-bearing choice. Slotting
 * UNDER ain rather than renumbering ain keeps box_ain.c's deliberate ordering
 * against the SPI driver thread exactly as reasoned there.
 * Stack sized for a full zsock_send stack traversal (TCP/IP/driver inline). */
/* 4: below the service loop (now 3) -- shipping yields to everything that
 * creates or reacts to data. See the priority-lattice note in prj.conf. */
#define PUB_THREAD_PRIO  4
#define PUB_THREAD_STACK 4096

K_THREAD_STACK_DEFINE(pub_stack, PUB_THREAD_STACK);
static struct k_thread pub_thread;

void box_pub_init(void)
{
	k_thread_create(&pub_thread, pub_stack, K_THREAD_STACK_SIZEOF(pub_stack),
			pub_run, NULL, NULL, NULL, PUB_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&pub_thread, "extio_pub");
}
