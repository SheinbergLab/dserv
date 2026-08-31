/*
 * box_ble_periph.c -- BLE peripheral over the frozen d5e7000x pipe (see the .h).
 */
#include "box_ble_periph.h"
#include "dserv_msg.h"
#include "dserv_ble.h"
#include "box_gpio.h"    /* box_gpio_now_us(): this box's own clock, for h_recv */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(box_ble_periph, LOG_LEVEL_INF);

#define FRAME_LEN DSERV_MSG_LEN

static const struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xd5e70001, 0x8f2c, 0x4b6a, 0x9ae5, 0x3c7a10a5b2c1));
static const struct bt_uuid_128 tx_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xd5e70002, 0x8f2c, 0x4b6a, 0x9ae5, 0x3c7a10a5b2c1));
static const struct bt_uuid_128 rx_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xd5e70003, 0x8f2c, 0x4b6a, 0x9ae5, 0x3c7a10a5b2c1));

/* Inbound config/cmd frames, filled on the BT RX thread, drained by the service
 * loop through the uplink's poll(). Echo requests never reach this queue -- they
 * are answered in the write callback and consumed there. */
K_MSGQ_DEFINE(rx_q, FRAME_LEN, 8, 4);

static struct bt_conn *peer;          /* the receiver, or NULL                */
static uint8_t  radio_up;
/* Whether ADVERTISING is actually running, and why not if it is not.
 *
 * Separate from radio_up because bt_enable() succeeding says nothing about the
 * advertiser -- and a peripheral that is powered but not advertising is
 * INVISIBLE, which from the receiver's side is indistinguishable from being
 * switched off or out of range. The central had exactly this bug this morning
 * (a boot line claiming "scanning" on the strength of bt_enable alone); there
 * is no excuse for reintroducing it at the other end of the same pipe. */
static uint8_t  advertising;
static int      adv_err;
static uint8_t  subscribed;           /* CCC written: notifications wanted    */
static uint16_t att_mtu;
static uint32_t st_echo_rx, st_tx_ok, st_tx_drop;
static char     adv_name[4 + BOX_NAME_MAX + 8];   /* "extio-" + name          */

/* The TX characteristic's VALUE attribute, resolved once at init.
 *
 * NOT an offset from whatever attribute a callback happens to receive: from the
 * RX value attribute the TX value is three back, and writing that as attr[-3]
 * puts a silent dependency on the service table's ORDER into a function that
 * has no reason to know it. Reorder the table and the peripheral would notify
 * the CCC instead -- which fails quietly, in the one direction that carries
 * every event. */
static const struct bt_gatt_attr *tx_attr;

/* ---- the GATT service ----
 *
 * TX carries no read/write handler at all: it is NOTIFY-only, so the value is
 * pushed with bt_gatt_notify() and never fetched. The CCC that follows it is
 * what a central writes to subscribe, and its callback is the only honest
 * signal that frames sent now will go anywhere.
 */
static void tx_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	subscribed = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("receiver %s the frame pipe", subscribed ? "subscribed to" : "unsubscribed from");
}

/* One inbound write on RX: either an echo request (answered here, immediately)
 * or a frame for the box (queued).
 *
 * THE ECHO PATH IS THE WHOLE REASON THIS FUNCTION IS SHAPED LIKE THIS. h_recv is
 * stamped in the first statement, before any parsing or branching, because
 * everything between arrival and that stamp is error the receiver cannot see and
 * cannot filter: it lands in one leg of the round trip, so it biases every
 * timestamp rather than averaging out. Same reason the reply is notified from
 * this callback rather than handed to a work queue.
 */
static ssize_t rx_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	uint64_t h_recv = box_gpio_now_us();
	const uint8_t *v = buf;

	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	/* The whole-frame contract: one 128-byte dserv_msg per PDU, no
	 * fragmentation layer. Anything else is not ours -- drop it rather than
	 * feed a framer that would then resync on the next '>' and silently eat
	 * a real frame with it. */
	if (len != FRAME_LEN) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	if (v[0] == DSERV_ECHO_CHAR && v[1] == DSERV_ECHO_REQ) {
		uint8_t reply[FRAME_LEN];

		/* r0 is echoed back UNTOUCHED so the receiver stays stateless
		 * across the round trip -- it has no outstanding-request table to
		 * keep, and a dropped request costs it nothing. */
		memcpy(reply, v, FRAME_LEN);
		reply[1] = DSERV_ECHO_REPLY;
		memcpy(reply + DSERV_ECHO_OFF_HRECV, &h_recv, sizeof h_recv);

		if (subscribed && tx_attr) {
			(void) bt_gatt_notify(conn, tx_attr, reply, FRAME_LEN);
			st_echo_rx++;
		}
		return len;                     /* consumed: never reaches the box */
	}

	if (k_msgq_put(&rx_q, v, K_NO_WAIT) != 0) {
		/* Inbound config/cmd is rare and the queue is 8 deep; full means
		 * the service loop is not running, and dropping is better than
		 * blocking the BT thread on a box that is already wedged. */
		LOG_WRN("inbound queue full -- dropped a frame");
	}
	return len;
}

/* Table order is load-bearing: PRIMARY_SERVICE, CHARACTERISTIC(tx), tx value,
 * CCC, CHARACTERISTIC(rx), rx value. tx_attr is taken from index 2 at init. */
BT_GATT_SERVICE_DEFINE(extio_svc,
	BT_GATT_PRIMARY_SERVICE(&svc_uuid),
	BT_GATT_CHARACTERISTIC(&tx_uuid.uuid, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(tx_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&rx_uuid.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, rx_write, NULL),
);

/* ---- advertising ----
 *
 * The SERVICE UUID goes in the advertisement itself and the NAME in the scan
 * response, which is the split dserv_ble.h froze: the receiver matches on the
 * UUID (a name is a convention, and matching on one would connect to anything a
 * user renamed), while the name is what a human reads in a scanner. A 128-bit
 * UUID plus flags is 19 of the 31 adv bytes, so the name would not fit beside
 * it anyway. */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_UUID128_ALL, svc_uuid.val, sizeof svc_uuid.val),
};
static struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, adv_name, 0),   /* len filled at start */
};

static int adv_start(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
				  sd, ARRAY_SIZE(sd));

	if (err && err != -EALREADY) {
		advertising = 0;
		adv_err = err;
		LOG_ERR("advertising failed to start (%d)", err);
		return err;
	}
	advertising = 1;
	adv_err = 0;
	LOG_INF("advertising as %s", adv_name);
	return 0;
}

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_WRN("connection failed (%u)", err);
		adv_start();
		return;
	}
	peer = bt_conn_ref(conn);
	advertising = 0;                        /* a legacy advertiser stops on connect */
	att_mtu = bt_gatt_get_mtu(conn);
	LOG_INF("receiver connected, mtu %u%s", att_mtu,
		att_mtu >= DSERV_BLE_MTU_MIN ? "" : " -- TOO SMALL for whole frames");
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	if (peer) {
		bt_conn_unref(peer);
		peer = NULL;
	}
	subscribed = 0;
	att_mtu = 0;
	LOG_INF("receiver disconnected (0x%02x) -- advertising again", reason);
	adv_start();
}

/* The MTU is negotiated AFTER connect, so the value latched in connected_cb is
 * the pre-exchange one (23 on most stacks). Without this the box would report a
 * broken pipe on a link that is fine -- and, worse, refuse to send on it. */
static void mtu_updated_cb(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	ARG_UNUSED(conn);
	att_mtu = (tx < rx) ? tx : rx;
	LOG_INF("mtu now %u%s", att_mtu,
		att_mtu >= DSERV_BLE_MTU_MIN ? "" : " -- TOO SMALL for whole frames");
}

BT_CONN_CB_DEFINE(periph_conn_cbs) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
};
static struct bt_gatt_cb gatt_cbs = { .att_mtu_updated = mtu_updated_cb };

/* ---- public API ---- */
int box_ble_periph_init(const box_config_t *cfg)
{
	int err;

	if (radio_up) {
		return 0;
	}
	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}
	bt_gatt_cb_register(&gatt_cbs);
	tx_attr = &extio_svc.attrs[2];          /* the TX characteristic value */
	radio_up = 1;

	/* dserv_cfg_name(), NOT cfg->name: the "box" default for an unnamed box
	 * lives in that accessor, and the raw field is EMPTY on a fresh config.
	 * Using it directly advertised `extio-` while the same box published its
	 * datapoints under `extio/box` -- the advertised and published identities
	 * disagreeing, which is the exact mismatch box_ble.c's learn_name() comment
	 * warns about at the other end of this pipe. */
	snprintf(adv_name, sizeof adv_name, "%s%s", DSERV_BLE_NAME_PFX,
		 dserv_cfg_name(cfg));
	sd[0].data_len = (uint8_t) strlen(adv_name);
	sd[0].data = (const uint8_t *) adv_name;

	return adv_start();
}

int box_ble_periph_active(void) { return radio_up ? 1 : 0; }

int box_ble_periph_advertising(void) { return advertising ? 1 : 0; }
int box_ble_periph_adv_err(void)     { return adv_err; }

int box_ble_periph_ready(void)
{
	/* Subscribed AND big enough for a whole frame. Reporting ready on a
	 * 23-byte MTU would hand the publisher a pipe that refuses every frame,
	 * which reads downstream as a dead box rather than a small MTU. */
	return (peer && subscribed && att_mtu >= DSERV_BLE_MTU_MIN) ? 1 : 0;
}

void box_ble_periph_rename(const box_config_t *cfg)
{
	if (!radio_up) {
		return;
	}
	/* dserv_cfg_name(), NOT cfg->name: the "box" default for an unnamed box
	 * lives in that accessor, and the raw field is EMPTY on a fresh config.
	 * Using it directly advertised `extio-` while the same box published its
	 * datapoints under `extio/box` -- the advertised and published identities
	 * disagreeing, which is the exact mismatch box_ble.c's learn_name() comment
	 * warns about at the other end of this pipe. */
	snprintf(adv_name, sizeof adv_name, "%s%s", DSERV_BLE_NAME_PFX,
		 dserv_cfg_name(cfg));
	sd[0].data_len = (uint8_t) strlen(adv_name);
	sd[0].data = (const uint8_t *) adv_name;

	/* Advertising data is captured at start, so a rename only reaches the
	 * air by stopping and restarting. Harmless while connected: bt_le_adv_stop
	 * on a non-advertising set returns -EALREADY and the new name takes
	 * effect at the next disconnect. */
	(void) bt_le_adv_stop();
	adv_start();
}

void box_ble_periph_stats(uint16_t *mtu, uint32_t *echo_rx,
			  uint32_t *tx_ok, uint32_t *tx_drop)
{
	if (mtu)     *mtu = att_mtu;
	if (echo_rx) *echo_rx = st_echo_rx;
	if (tx_ok)   *tx_ok = st_tx_ok;
	if (tx_drop) *tx_drop = st_tx_drop;
}

/* ---- the uplink face (box_uplink.c calls these through its vtable) ---- */
int box_ble_periph_send(const uint8_t *buf, int len)
{
	if (!box_ble_periph_ready()) {
		st_tx_drop++;
		return -1;
	}
	if (len != FRAME_LEN) {
		return -1;
	}
	if (!tx_attr || bt_gatt_notify(peer, tx_attr, buf, FRAME_LEN) != 0) {
		/* No ATT buffer this instant -- ordinary backpressure. Report it
		 * as "took nothing" so box_pub retries rather than counting a
		 * drop; the publisher's queue is what absorbs a busy radio. */
		return 0;
	}
	st_tx_ok++;
	return len;
}

int box_ble_periph_send_stream(const uint8_t *buf, int len)
{
	int sent = 0;

	/* Frame-aligned, one notification per frame: the contract is one whole
	 * dserv_msg per ATT PDU, so a "stream" here is just a loop. Stops at the
	 * first frame the radio will not take and reports the bytes accepted, so
	 * the publisher keeps the remainder. */
	while (len - sent >= FRAME_LEN) {
		int r = box_ble_periph_send(buf + sent, FRAME_LEN);

		if (r != FRAME_LEN) {
			break;
		}
		sent += FRAME_LEN;
	}
	return sent;
}

int box_ble_periph_poll(uint8_t *buf, int max)
{
	if (max < FRAME_LEN) {
		return 0;
	}
	return (k_msgq_get(&rx_q, buf, K_NO_WAIT) == 0) ? FRAME_LEN : 0;
}
