/*
 * box_ble.c -- multi-peripheral BLE central over the frozen d5e7000x pipe.
 * Zephyr Bluetooth (bt_* / GATT client), up to CONFIG_BT_MAX_CONN peers.
 */
#include "box_ble.h"

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

/* Received whole frames, filled on the BT RX thread, drained by the main loop. */
K_MSGQ_DEFINE(rx_q, FRAME_LEN, 16, 4);

/* Per-connected-peer discovery/subscription state. */
struct peer {
	struct bt_conn                  *conn;
	uint16_t                         svc_end;
	struct bt_gatt_discover_params   disc;
	struct bt_gatt_subscribe_params  sub;
	struct bt_gatt_exchange_params   mtu;
};
static struct peer peers[CONFIG_BT_MAX_CONN];
static atomic_t connected_count;
static bool scanning;

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
			peers[i].conn = c;
			return &peers[i];
		}
	}
	return NULL;
}

static void start_scan(void);

/* ---- notifications: one whole 128-byte frame per PDU -> the queue ---- */
static uint8_t on_notify(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
			 const void *data, uint16_t length)
{
	ARG_UNUSED(conn);
	if (!data) {                                   /* peer unsubscribed */
		params->value_handle = 0;
		return BT_GATT_ITER_STOP;
	}
	if (length == FRAME_LEN) {                     /* whole-frame contract */
		(void) k_msgq_put(&rx_q, data, K_NO_WAIT); /* drop if full (best-effort) */
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
		p->svc_end = sv->end_handle;
		p->disc.uuid = &tx_uuid.uuid;
		p->disc.start_handle = attr->handle + 1;
		p->disc.end_handle = p->svc_end;
		p->disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
		(void) bt_gatt_discover(conn, &p->disc);
		return BT_GATT_ITER_STOP;
	}

	if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
		p->sub.value_handle = bt_gatt_attr_value_handle(attr);
		p->disc.uuid = BT_UUID_GATT_CCC;
		p->disc.start_handle = attr->handle + 2;
		p->disc.end_handle = p->svc_end;
		p->disc.type = BT_GATT_DISCOVER_DESCRIPTOR;
		(void) bt_gatt_discover(conn, &p->disc);
		return BT_GATT_ITER_STOP;
	}

	/* CCC descriptor -> subscribe to notifications */
	p->sub.notify = on_notify;
	p->sub.value = BT_GATT_CCC_NOTIFY;
	p->sub.ccc_handle = attr->handle;
	if (bt_gatt_subscribe(conn, &p->sub) == 0) {
		LOG_INF("subscribed to a peer's frame pipe");
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
		struct bt_conn *c = conn;
		bt_conn_unref(c);
		start_scan();                          /* try again */
		return;
	}
	struct peer *p = peer_alloc(conn);
	if (!p) {
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}
	atomic_inc(&connected_count);

	p->mtu.func = mtu_cb;
	if (bt_gatt_exchange_mtu(conn, &p->mtu) != 0) {
		mtu_cb(conn, 0, &p->mtu);              /* skip straight to discovery */
	}
	start_scan();                                  /* keep collecting the fleet */
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);
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
	bt_data_parse(buf, ad_has_service, &match);
	if (!match) {
		return;
	}

	if (bt_le_scan_stop() == 0) {
		scanning = false;
	}
	struct bt_conn *conn = NULL;
	int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
				    BT_LE_CONN_PARAM_DEFAULT, &conn);
	if (err) {
		LOG_WRN("create conn failed (%d)", err);
		start_scan();
	} else if (conn) {
		bt_conn_unref(conn);                   /* connected cb re-refs via callback */
	}
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

int box_ble_scanning(void)
{
	return scanning ? 1 : 0;
}

int box_ble_scan_err(void)
{
	return scan_err;
}
