/*
 * box_ble.c -- multi-peripheral BLE central over the frozen d5e7000x pipe.
 * Zephyr Bluetooth (bt_* / GATT client), up to CONFIG_BT_MAX_CONN peers.
 */
#include "box_ble.h"
#include "dserv_msg.h"   /* peers are identified by the frames they publish */
#include "dserv_ble.h"   /* the frozen echo-sync side-channel layout          */
#include "box_clock.h"   /* the same offset+rate estimator the box uses       */
#include "box_gpio.h"    /* box_gpio_now_us(): the receiver's own clock       */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(box_ble, LOG_LEVEL_INF);

#define FRAME_LEN 128

/* Frozen contract (BLE.md / wiznet-io/pico/extio_pipe.gatt): the service, the
 * peripheral->central NOTIFY characteristic, and the whole-frame MTU floor. */
static const struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xd5e70001, 0x8f2c, 0x4b6a, 0x9ae5, 0x3c7a10a5b2c1));
static const struct bt_uuid_128 tx_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xd5e70002, 0x8f2c, 0x4b6a, 0x9ae5, 0x3c7a10a5b2c1));
/* RX: central -> peripheral, WRITE_WITHOUT_RESPONSE. Same frozen contract as
 * the other two; this end simply never used it until 2026-08-31. */
static const struct bt_uuid_128 rx_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xd5e70003, 0x8f2c, 0x4b6a, 0x9ae5, 0x3c7a10a5b2c1));

/* Received whole frames, filled on the BT RX thread, drained by the main loop. */
K_MSGQ_DEFINE(rx_q, FRAME_LEN, 16, 4);

/* How far along the discovery chain a peer is. The chain walks
 * primary service -> TX characteristic -> its CCC (subscribe) -> RX
 * characteristic, and every step lands in the SAME callback, so the stage has
 * to be explicit: TX and RX both arrive as BT_GATT_DISCOVER_CHARACTERISTIC and
 * are otherwise indistinguishable there. */
enum disc_stage {
	DISC_TX_CHAR = 1,   /* looking for d5e70002 */
	DISC_TX_CCC,        /* looking for its CCC, then subscribing */
	DISC_RX_CHAR,       /* looking for d5e70003 */
	DISC_DONE,
};

/* The longest `<class>/<box>` we will remember for a peer. BOX_NAME_MAX is 24
 * in dserv_config.h and the class is "extio", so 40 leaves room without
 * pulling that header in for one constant. A longer prefix is dropped rather
 * than truncated -- a TRUNCATED prefix would match the wrong box, which is
 * worse than not routing at all. */
#define PEER_PFX_MAX 40

/* ---- echo-sync cadence and filtering (BLE.md "Time") ----
 *
 * 300 ms matches the Pico receiver: ~3 probes/s, dense enough to converge in a
 * couple of seconds and cheap enough to leave running forever.
 *
 * MINIMUM-RTT, NOT AVERAGING, and the reason is specific to this radio: BLE
 * round trips are quantised by the connection interval, so the distribution is
 * a floor with a long upper tail rather than noise about a mean. Averaging
 * walks INTO the tail; the lowest-RTT sample in a window is the one whose
 * midpoint assumption (that the request and the reply each took rtt/2) is least
 * wrong. Same NTP reasoning the Pico's estimator uses.
 *
 * A window of 8 at 300 ms commits a rate-teaching anchor about every 2.4 s. */
#define ECHO_INTERVAL_MS 300
#define ECHO_WIN         8

/* Connection parameters, PINNED rather than defaulted -- 15 ms interval,
 * peripheral latency 0, 4 s supervision. Units are 1.25 ms (interval) and
 * 10 ms (timeout).
 *
 * Zephyr's BT_LE_CONN_PARAM_DEFAULT is 30-50 ms, and that is not a detail
 * here: an echo round trip cannot beat the connection interval, `rtt/2` is
 * the midpoint uncertainty, and the tier's target is ~1 ms. The Pico receiver
 * pinned 15 ms for the same reason ("btstack's default too coarse, the offset
 * noise swamped the ~1 ms target").
 *
 * 7.5 ms -- the spec floor -- rather than the Pico's 15, and this is MEASURED
 * rather than reasoned. Against hh1 on the Phase 2 rig, the systematic bias in
 * the mapped timestamps was:
 *
 *     15.00 ms interval   median +5.39 ms   IQR 0.51 ms
 *      7.50 ms interval   median +1.12 ms   IQR 0.55 ms
 *
 * The interval owns the BIAS and not the jitter, which is what the midpoint
 * model predicts: the central transmits promptly while the peripheral's reflex
 * reply waits for the next connection event, so the error is (leg2-leg1)/2 and
 * leg2 is interval-quantised. Only at 7.5 ms does the tier meet its ~1 ms
 * target against a hardware ground truth.
 *
 * The cost is the peripheral's battery, and the answer to that is peripheral
 * LATENCY (see latency_service) -- never a longer interval, which would buy
 * power by giving the bias straight back. */
#define CONN_INT_UNITS 6            /* 6 = 7.5 ms (spec floor); 12 = 15 ms */
static const struct bt_le_conn_param conn_param =
	BT_LE_CONN_PARAM_INIT(CONN_INT_UNITS, CONN_INT_UNITS, 0, 400);

/* Per-connected-peer discovery/subscription state. */
struct peer {
	struct bt_conn                  *conn;
	uint16_t                         svc_start;
	uint16_t                         svc_end;
	uint16_t                         rx_handle;   /* d5e70003 value handle, 0 = none */
	char                             pfx[PEER_PFX_MAX];  /* "extio/hh1", learned */
	uint8_t                          named;
	uint8_t                          stage;
	struct bt_gatt_discover_params   disc;
	struct bt_gatt_subscribe_params  sub;
	struct bt_gatt_exchange_params   mtu;

	/* Echo-sync: maps THIS PEER's clock -> the receiver's own clock. Written
	 * and read only on the BT RX thread (on_notify); the console reads it
	 * through box_clock_snap's seqlock. box_ble_service() sends requests from
	 * the service loop but never touches this. */
	box_clock_t                      hh_clock;
	uint32_t                         last_echo_ms;
	uint32_t                         win_rtt;     /* best RTT in the open window */
	uint64_t                         win_rmid;    /* ...and its receiver midpoint */
	uint64_t                         win_h;       /* ...and the peer time it maps  */
	uint8_t                          win_n;
	uint32_t                         min_rtt_us;  /* running floor, telemetry      */
	uint32_t                         echo_tx, echo_rx;
	/* 0 none, 1 first snap (offset only), 2 a windowed min-RTT anchor has
	 * committed. The latency manager gates on 2: raising latency before the
	 * estimator has a filtered anchor would coarsen the very samples it still
	 * needs to converge. */
	uint8_t                          sync_level;

	/* Adaptive peripheral latency state (service-loop thread only). */
	uint16_t                         lat_applied;
	uint32_t                         lat_phase_ms;
	uint32_t                         lat_try_ms;
	uint8_t                          lat_idle;
};
static struct peer peers[CONFIG_BT_MAX_CONN];
static atomic_t connected_count;
static bool scanning;

/* Advertisement ledger. Without it, "conns 0/8 scanning" has two completely
 * different causes that look identical from the console: nothing is
 * advertising within range, or something is and we are rejecting it (wrong
 * service UUID, wrong adv type, fleet full). One counter each turns a guess
 * into a reading. */
static uint32_t adv_seen, adv_matched;

/* Connection ledger, for the same reason as the advertisement one. "matched but
 * never connected" has at least three causes -- the controller refusing the
 * create, the establishment failing, or the link coming up and dropping again --
 * and on a board whose LOG_DEFAULT_LEVEL is ERR, none of them says anything.
 * Counting is cheaper than raising log levels on a box that is misbehaving. */
static uint32_t conn_try, conn_create_err, conn_est_err, conn_dropped;
static int      conn_last_err;

static struct peer *peer_for(struct bt_conn *c)
{
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
		if (peers[i].conn == c) {
			return &peers[i];
		}
	}
	return NULL;
}

static struct peer *peer_alloc(struct bt_conn *c)
{
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
		if (!peers[i].conn) {
			memset(&peers[i], 0, sizeof peers[i]);
			/* A zeroed running MINIMUM would never be beaten, so every
			 * echo sample would fail `rtt < min` and the filter would
			 * silently never narrow. Start it at the ceiling. */
			peers[i].min_rtt_us = 0xFFFFFFFFu;
			peers[i].conn = c;
			return &peers[i];
		}
	}
	return NULL;
}

static void start_scan(void);

/* Learn which box a peer IS, from the frames it publishes.
 *
 * WHY FROM THE TRAFFIC AND NOT FROM THE ADVERTISEMENT. The peripheral
 * advertises `extio-<name>` (BLE.md), so the GAP name is available at connect
 * time and would be the obvious source. It is the wrong one: the host addresses
 * boxes by the datapoint prefix they PUBLISH, and those two strings are related
 * by a firmware convention rather than by anything enforced. Routing a
 * `config/` write by the advertised name would send it to a box whose
 * published identity might differ -- silently, and only for the peer that had
 * been renamed. The frames are the authority, so take the name from them.
 *
 * Costs one thing worth stating: a peer is unroutable until it has published
 * something. In practice that is its announce burst, immediately after connect. */
static void learn_name(struct peer *p, const uint8_t *frame)
{
	dserv_msg_t m;
	int i = 0, slash = 0, plen;

	if (dserv_msg_parse(frame, &m) != 0) {
		return;
	}
	/* "<class>/<box>/..." -- keep the first two segments verbatim. */
	while (i < (int) m.namelen && slash < 2) {
		if (m.name[i] == '/') {
			slash++;
		}
		i++;
	}
	if (slash < 2) {
		return;                                /* no leaf yet -- not an addressable key */
	}
	plen = i - 1;                                  /* drop the second '/' */
	if (plen <= 0 || plen >= PEER_PFX_MAX) {
		return;                                /* see PEER_PFX_MAX: never truncate */
	}
	/* RE-LEARN ON CHANGE, not once. A peer that is renamed keeps the same
	 * connection and simply starts publishing under a new prefix -- and the
	 * downlink routes on this string, so a stale copy would leave
	 * `extio/<newname>/config/...` matching nothing while the box itself is
	 * connected and healthy. Cheap: a memcmp of ~10 bytes against a prefix
	 * that almost never changes. */
	if (p->named && (int) strlen(p->pfx) == plen &&
	    memcmp(p->pfx, m.name, (size_t) plen) == 0) {
		return;                                /* unchanged, the usual case */
	}
	if (p->named) {
		LOG_INF("peer renamed: %s -> %.*s", p->pfx, plen, m.name);
	} else {
		LOG_INF("peer identified as %.*s", plen, m.name);
	}
	memcpy(p->pfx, m.name, (size_t) plen);
	p->pfx[plen] = '\0';
	p->named = 1;
}

/* One echo REPLY: turn it into an offset sample, min-RTT filter, feed the clock.
 *
 * The receiver stays STATELESS across the round trip -- r0 comes back in the
 * reply rather than being remembered here -- so a dropped request costs nothing
 * and there is no outstanding-request bookkeeping to get wrong.
 *
 * rtt/2 assumes the two legs are symmetric, which is exactly the assumption the
 * min-RTT filter protects: the fastest round trip observed is the one least
 * inflated by a connection-interval wait on either side. */
static void echo_reply(struct peer *p, const uint8_t *v)
{
	uint64_t r1 = box_gpio_now_us();
	uint64_t r0 = 0, h_recv = 0;
	uint32_t rtt;

	memcpy(&r0,     v + DSERV_ECHO_OFF_R0,    sizeof r0);
	memcpy(&h_recv, v + DSERV_ECHO_OFF_HRECV, sizeof h_recv);

	/* A reply that claims to predate its own request, or that carries no peer
	 * stamp, is garbage -- from a reboot mid-flight, or a peripheral whose
	 * clock is not running. Dropping it is right: a bad pair here would be
	 * indistinguishable from a real one later. */
	if (r1 <= r0 || h_recv == 0) {
		return;
	}
	rtt = (uint32_t) (r1 - r0);
	p->echo_rx++;
	if (rtt < p->min_rtt_us) {
		p->min_rtt_us = rtt;
	}

	if (p->win_n == 0 || rtt < p->win_rtt) {
		p->win_rtt  = rtt;
		p->win_rmid = r0 + rtt / 2;
		p->win_h    = h_recv;
	}
	p->win_n++;

	/* Snap the offset from the very first sample so relayed frames start
	 * carrying real time within ~300 ms instead of waiting out a full window
	 * -- but NOT as a rate-teaching anchor (trusted=0): one unfiltered sample
	 * has no business setting a slope. */
	if (!p->hh_clock.synced) {
		box_clock_sync(&p->hh_clock, p->win_rmid, p->win_h, 0);
		p->sync_level = 1;
		LOG_INF("echo: %s synced, rtt %u us", p->named ? p->pfx : "peer", rtt);
	}

	if (p->win_n >= ECHO_WIN) {
		box_clock_sync(&p->hh_clock, p->win_rmid, p->win_h, 1);
		p->win_n = 0;
		p->sync_level = 2;
	}
}

/* ---- notifications: one whole 128-byte frame per PDU -> the queue ---- */
static uint8_t on_notify(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
			 const void *data, uint16_t length)
{
	if (!data) {                                   /* peer unsubscribed */
		params->value_handle = 0;
		return BT_GATT_ITER_STOP;
	}
	if (length == FRAME_LEN) {                     /* whole-frame contract */
		const uint8_t *v = data;
		struct peer *p = peer_for(conn);
		uint8_t f[FRAME_LEN];
		dserv_msg_t m;

		/* Echo replies are handled ENTIRELY here and never relayed: they are
		 * a radio-boundary side-channel, not datapoints (dserv_ble.h). */
		if (v[0] == DSERV_ECHO_CHAR && v[1] == DSERV_ECHO_REPLY) {
			if (p) {
				echo_reply(p, v);
			}
			return BT_GATT_ITER_CONTINUE;
		}

		memcpy(f, v, FRAME_LEN);
		if (p) {
			learn_name(p, f);

			/* THE REWRITE. BLE.md: "translate exactly once, at the radio
			 * boundary." The peer stamped this at its own GPIO IRQ on its
			 * own clock; box_clock_stamp maps that into OUR clock, and
			 * main.c then applies the box->dserv sync every local event
			 * already goes through. Two hops, each owned by the layer that
			 * has the evidence for it.
			 *
			 * Unsynced (or an unstamped frame) deliberately yields 0, which
			 * tells dserv to arrival-stamp. That is a real loss of accuracy
			 * and it is the HONEST one: before this existed the peer's raw
			 * uptime was relayed verbatim and landed ~56 years off, a
			 * plausible-looking number nothing downstream could catch. */
			if (dserv_msg_parse(f, &m) == 0 && m.timestamp) {
				dserv_msg_set_timestamp(
					f, box_clock_stamp(&p->hh_clock, m.timestamp));
			}
		}
		(void) k_msgq_put(&rx_q, f, K_NO_WAIT);    /* drop if full (best-effort) */
	}
	return BT_GATT_ITER_CONTINUE;
}

/* ---- discovery chain: primary service -> notify char -> CCC -> subscribe ---- */
static uint8_t discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   struct bt_gatt_discover_params *params)
{
	struct peer *p = peer_for(conn);
	if (!p || !attr) {
		return BT_GATT_ITER_STOP;
	}

	if (params->type == BT_GATT_DISCOVER_PRIMARY) {
		const struct bt_gatt_service_val *sv = attr->user_data;
		p->svc_start = attr->handle + 1;
		p->svc_end = sv->end_handle;
		p->stage = DISC_TX_CHAR;
		p->disc.uuid = &tx_uuid.uuid;
		p->disc.start_handle = p->svc_start;
		p->disc.end_handle = p->svc_end;
		p->disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
		(void) bt_gatt_discover(conn, &p->disc);
		return BT_GATT_ITER_STOP;
	}

	if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
		if (p->stage == DISC_RX_CHAR) {
			/* The write end. Nothing follows it, so the chain ends here --
			 * and a peer without it stays receive-only rather than
			 * failing: an older peripheral that predates the RX
			 * characteristic should keep publishing, not drop off. */
			p->rx_handle = bt_gatt_attr_value_handle(attr);
			p->stage = DISC_DONE;
			LOG_INF("peer downlink ready (rx handle %u)", p->rx_handle);
			return BT_GATT_ITER_STOP;
		}
		p->sub.value_handle = bt_gatt_attr_value_handle(attr);
		p->stage = DISC_TX_CCC;
		p->disc.uuid = BT_UUID_GATT_CCC;
		p->disc.start_handle = attr->handle + 2;
		p->disc.end_handle = p->svc_end;
		p->disc.type = BT_GATT_DISCOVER_DESCRIPTOR;
		(void) bt_gatt_discover(conn, &p->disc);
		return BT_GATT_ITER_STOP;
	}

	/* CCC descriptor -> subscribe to notifications, then go find the RX
	 * characteristic. Subscribing first is deliberate: the uplink is what the
	 * peripheral needs in order to be useful at all, so a failure hunting the
	 * write end must never cost us the read end. */
	p->sub.notify = on_notify;
	p->sub.value = BT_GATT_CCC_NOTIFY;
	p->sub.ccc_handle = attr->handle;
	if (bt_gatt_subscribe(conn, &p->sub) == 0) {
		LOG_INF("subscribed to a peer's frame pipe");
	} else {
		LOG_WRN("subscribe FAILED -- this peer will publish nothing");
	}

	p->stage = DISC_RX_CHAR;
	p->disc.uuid = &rx_uuid.uuid;
	p->disc.start_handle = p->svc_start;
	p->disc.end_handle = p->svc_end;
	p->disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
	if (bt_gatt_discover(conn, &p->disc) != 0) {
		LOG_WRN("RX characteristic discovery did not start -- peer is receive-only");
		p->stage = DISC_DONE;
	}
	return BT_GATT_ITER_STOP;
}

static void mtu_cb(struct bt_conn *conn, uint8_t err,
		   struct bt_gatt_exchange_params *params)
{
	ARG_UNUSED(params);
	struct peer *p = peer_for(conn);
	if (!p) {
		return;
	}
	if (err) {
		LOG_WRN("MTU exchange failed (%u)", err);
	}
	/* start service discovery regardless (MTU may already satisfy the floor) */
	p->disc.uuid = &svc_uuid.uuid;
	p->disc.func = discover_cb;
	p->disc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	p->disc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	p->disc.type = BT_GATT_DISCOVER_PRIMARY;
	(void) bt_gatt_discover(conn, &p->disc);
}

/* ---- connection lifecycle ---- */
static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		conn_est_err++;
		conn_last_err = -(int) err;            /* HCI reason, negated to be distinct */
		bt_conn_unref(conn);                   /* the create reference; nothing took it */
		start_scan();                          /* try again */
		return;
	}
	struct peer *p = peer_alloc(conn);
	if (!p) {
		/* Fleet full. Drop the link AND the create reference -- disconnected()
		 * will not clean up after us, because there is no slot for peer_for()
		 * to find. */
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(conn);
		return;
	}
	/* p->conn now OWNS the reference bt_conn_le_create() returned. */
	atomic_inc(&connected_count);

	p->mtu.func = mtu_cb;
	if (bt_gatt_exchange_mtu(conn, &p->mtu) != 0) {
		mtu_cb(conn, 0, &p->mtu);              /* skip straight to discovery */
	}
	start_scan();                                  /* keep collecting the fleet */
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	conn_dropped++;
	conn_last_err = -(int) reason;
	struct peer *p = peer_for(conn);
	if (p) {
		bt_conn_unref(p->conn);
		p->conn = NULL;
		atomic_dec(&connected_count);
	}
	start_scan();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* ---- scanning: match the frozen service UUID in the advertisement ---- */
static bool ad_has_service(struct bt_data *data, void *user_data)
{
	bool *match = user_data;
	if (data->type != BT_DATA_UUID128_ALL && data->type != BT_DATA_UUID128_SOME) {
		return true;                           /* keep parsing */
	}
	for (int i = 0; i + 16 <= data->data_len; i += 16) {
		if (memcmp(&data->data[i], svc_uuid.val, 16) == 0) {
			*match = true;
			return false;                  /* stop parsing */
		}
	}
	return true;
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
		    struct net_buf_simple *buf)
{
	ARG_UNUSED(rssi);
	if (adv_type != BT_GAP_ADV_TYPE_ADV_IND &&
	    adv_type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		return;
	}
	if (atomic_get(&connected_count) >= CONFIG_BT_MAX_CONN) {
		return;                                /* fleet full */
	}

	bool match = false;

	adv_seen++;
	bt_data_parse(buf, ad_has_service, &match);
	if (!match) {
		return;
	}
	adv_matched++;

	if (bt_le_scan_stop() == 0) {
		scanning = false;
	}
	struct bt_conn *conn = NULL;
	int err;

	conn_try++;
	err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, &conn_param, &conn);
	if (err) {
		conn_create_err++;
		conn_last_err = err;
		LOG_WRN("create conn failed (%d)", err);
		start_scan();
	}
	/* THE CREATE REFERENCE IS DELIBERATELY NOT RELEASED HERE. It is handed to
	 * the peer slot in connected(), and released once in disconnected().
	 *
	 * This used to unref immediately, on the strength of a comment saying "the
	 * connected cb re-refs via callback" -- which it does not: a Zephyr conn
	 * callback receives a BORROWED reference and adds nothing. So peers[i].conn
	 * held a pointer the module did not own, and disconnected() then unref'd a
	 * reference it had never taken.
	 *
	 * MEASURED, and it is why this was invisible for a day: nothing at all goes
	 * wrong until the FIRST disconnect. hh1 stayed connected all afternoon and
	 * the pipe was flawless. The moment a peripheral went away (a UF2 reflash),
	 * the unbalanced unref freed the conn object early, the stack kept a stale
	 * entry for that peer, and every subsequent bt_conn_le_create() for it
	 * returned -EINVAL -- for ever. The ledger read
	 * `conn try=2431 create_err=2430 dropped=1 last=-22`: one disconnect, then
	 * nothing but refusals. */
}

/* Last bt_le_scan_start() failure; 0 = none. Kept because a central that is not
 * scanning is DEAF, and until 2026-08-31 that state was completely invisible:
 * this function tested the return value only to decide whether to set
 * `scanning`, and dropped the error on the floor. Meanwhile both callers that
 * announce the radio -- main.c's boot line and the CLI's `ble enable` -- printed
 * "scanning" on the strength of box_ble_init() returning 0, which only tests
 * bt_enable(). So a box whose scan never started said "radio up, scanning" and
 * then found nothing, forever, with no error anywhere.
 *
 * That is this repo's own "fields that report memory, not reality" (PORTING.md)
 * on the one message you would most want to trust while bringing up an unproven
 * central against a real peripheral -- where "it didn't connect" and "it was
 * never listening" are the two hypotheses you must be able to tell apart. */
static int scan_err;

static void start_scan(void)
{
	int err;

	if (scanning || atomic_get(&connected_count) >= CONFIG_BT_MAX_CONN) {
		return;                                /* already on, or fleet full */
	}
	err = bt_le_scan_start(BT_LE_SCAN_ACTIVE, scan_cb);
	if (err) {
		scan_err = err;
		LOG_ERR("scan start FAILED (%d) -- central is deaf, nothing will connect", err);
		return;
	}
	scan_err = 0;
	scanning = true;
}

/* ---- public API ---- */
static bool ble_up;

int box_ble_active(void) { return ble_up ? 1 : 0; }

int box_ble_init(void)
{
	int err;

	/* Idempotent: the boot path calls this once when cfg.ble_en is set, and
	 * `ble enable 1` calls it again to bring the radio up WITHOUT a reboot
	 * (which is what the CLI has always promised -- "live on radio builds").
	 * bt_enable() twice returns -EALREADY, so guard rather than rely on that. */
	if (ble_up) {
		return 0;
	}
	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}
	ble_up = true;
	start_scan();
	return 0;
}

int box_ble_poll(uint8_t *out)
{
	return k_msgq_get(&rx_q, out, K_NO_WAIT) == 0 ? 1 : 0;
}

int box_ble_conn_count(void)
{
	return (int) atomic_get(&connected_count);
}

int box_ble_forward(const char *name, uint16_t namelen, const uint8_t *frame)
{
	if (!ble_up || !name || !frame) {
		return 0;
	}
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
		struct peer *p = &peers[i];
		size_t n;
		int err;

		if (!p->conn || !p->named || !p->rx_handle) {
			continue;
		}
		n = strlen(p->pfx);
		/* Prefix match on a SEGMENT boundary. Without the '/' test,
		 * `extio/hh10` would be routed to `extio/hh1`. */
		if (namelen <= n || memcmp(name, p->pfx, n) != 0 || name[n] != '/') {
			continue;
		}
		/* Write-without-response, per the frozen contract: the peripheral
		 * owes no ATT ack, and for the echo-sync frames that will ride this
		 * path a response would add exactly the variable latency the
		 * estimator exists to remove. */
		err = bt_gatt_write_without_response(p->conn, p->rx_handle,
						     frame, FRAME_LEN, false);
		if (err) {
			LOG_WRN("forward to %s failed (%d)", p->pfx, err);
			return err;
		}
		return 1;
	}
	return 0;
}

/* ---- adaptive peripheral latency (BLE.md "Power") ----
 *
 * Ported from the Pico receiver's box_ble_latency_service, per peer rather than
 * per pipe. The windows are ITS first-cut values and remain UNTUNED here: they
 * are pending a real battery-life measurement, which no bench-powered board can
 * provide. Do not read them as validated.
 *
 * The supervision timeout must outlast a fully-skipped stretch. At 7.5 ms with
 * timeout 4 s, even ble_latency = 255 gives (1+255) * 7.5 * 2 = 3840 ms < 4000,
 * so a uint8_t latency can never ask for a link the timeout cannot survive.
 * That headroom is a property of the 7.5 ms interval -- re-check it if the
 * interval is ever raised. */
#define LAT_SYNC_MS   3000     /* dense latency-0 window: (re)converge echo-sync */
#define LAT_IDLE_MS  30000     /* low-power window between sync bursts           */
#define LAT_RETRY_MS   250     /* throttle: an update may already be in flight   */

static void latency_service(struct peer *p, const box_config_t *cfg, uint32_t now_ms)
{
	uint16_t target;

	if (cfg->ble_latency == 0) {
		/* Feature off -- the always-listen behaviour every timing
		 * measurement so far was taken against. */
		target = 0;
		p->lat_phase_ms = now_ms;
		p->lat_idle = 0;
	} else if (!p->lat_idle) {
		if (!p->lat_phase_ms) {
			p->lat_phase_ms = now_ms;
		}
		target = 0;
		if ((uint32_t) (now_ms - p->lat_phase_ms) >= LAT_SYNC_MS &&
		    p->sync_level >= 2) {
			p->lat_idle = 1;
			p->lat_phase_ms = now_ms;
		}
	} else {
		target = cfg->ble_latency;
		if ((uint32_t) (now_ms - p->lat_phase_ms) >= LAT_IDLE_MS) {
			p->lat_idle = 0;          /* back to a sync burst */
			p->lat_phase_ms = now_ms;
		}
	}

	if (target == p->lat_applied ||
	    (uint32_t) (now_ms - p->lat_try_ms) < LAT_RETRY_MS) {
		return;
	}
	p->lat_try_ms = now_ms;
	{
		/* Interval PINNED, only latency moves -- raising the interval would
		 * put the midpoint bias back (measured +5.39 ms at 15 ms vs
		 * +1.12 ms at 7.5), and that is accuracy, not power. */
		struct bt_le_conn_param np = {
			.interval_min = CONN_INT_UNITS,
			.interval_max = CONN_INT_UNITS,
			.latency      = target,
			.timeout      = 400,
		};

		if (bt_conn_le_param_update(p->conn, &np) == 0) {
			p->lat_applied = target;
			LOG_INF("%s peripheral latency -> %u (%s)",
				p->named ? p->pfx : "peer", target,
				target ? "idle power-save" : "sync burst");
		}
	}
}

void box_ble_service(const box_config_t *cfg)
{
	uint32_t now_ms;

	if (!ble_up || !cfg) {
		return;
	}
	now_ms = k_uptime_get_32();
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
		struct peer *p = &peers[i];
		uint8_t req[FRAME_LEN];
		uint64_t r0;

		if (!p->conn || !p->rx_handle) {
			continue;               /* receive-only peer: nothing to probe with */
		}
		latency_service(p, cfg, now_ms);

		if ((uint32_t) (now_ms - p->last_echo_ms) < ECHO_INTERVAL_MS) {
			continue;
		}
		p->last_echo_ms = now_ms;

		memset(req, 0, sizeof req);          /* zero-padded, whole-PDU rule */
		req[0] = DSERV_ECHO_CHAR;
		req[1] = DSERV_ECHO_REQ;
		/* Stamp r0 as late as possible -- everything between here and the
		 * write lands in the measured RTT as pure noise. */
		r0 = box_gpio_now_us();
		memcpy(req + DSERV_ECHO_OFF_R0, &r0, sizeof r0);

		if (bt_gatt_write_without_response(p->conn, p->rx_handle,
						   req, FRAME_LEN, false) == 0) {
			p->echo_tx++;
		}
		/* A refused write is ordinary backpressure (no ATT buffer this
		 * instant), not a fault: the next tick retries, and min-RTT
		 * filtering is built to tolerate gaps. */
	}
}

int box_ble_peer_info(int idx, box_ble_peer_info_t *out)
{
	struct peer *p;
	struct bt_conn_info info;

	if (idx < 0 || idx >= CONFIG_BT_MAX_CONN || !out) {
		return 0;
	}
	p = &peers[idx];
	if (!p->conn) {
		return 0;
	}
	memset(out, 0, sizeof *out);
	out->name        = p->named ? p->pfx : "(unnamed)";
	out->min_rtt_us  = (p->min_rtt_us == 0xFFFFFFFFu) ? 0 : p->min_rtt_us;
	out->echo_tx     = p->echo_tx;
	out->echo_rx     = p->echo_rx;
	out->synced      = p->sync_level;
	out->lat_applied = p->lat_applied;
	/* The NEGOTIATED interval, read live from the controller -- not the value
	 * we asked for in conn_param. Those differ whenever the peripheral has its
	 * own preferred parameters and requests an update after connect, which is
	 * invisible from this side otherwise. */
	out->conn_int = (bt_conn_get_info(p->conn, &info) == 0 &&
			 info.type == BT_CONN_TYPE_LE) ? info.le.interval : 0;
	return 1;
}

void box_ble_scan_counts(uint32_t *seen, uint32_t *matched)
{
	if (seen)    *seen = adv_seen;
	if (matched) *matched = adv_matched;
}

void box_ble_conn_counts(uint32_t *tries, uint32_t *create_err,
			 uint32_t *est_err, uint32_t *dropped, int *last_err)
{
	if (tries)      *tries = conn_try;
	if (create_err) *create_err = conn_create_err;
	if (est_err)    *est_err = conn_est_err;
	if (dropped)    *dropped = conn_dropped;
	if (last_err)   *last_err = conn_last_err;
}

int box_ble_scanning(void)
{
	return scanning ? 1 : 0;
}

int box_ble_scan_err(void)
{
	return scan_err;
}
