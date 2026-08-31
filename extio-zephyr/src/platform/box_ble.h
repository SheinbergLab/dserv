/*
 * box_ble.h -- multi-peripheral BLE central: the ingress side of block #6.
 *
 * The RW612 hub is BLE central to MANY nRF54-class clients at once (a fleet of
 * handhelds/buttons/levers). Each client speaks the frozen d5e7000x GATT pipe
 * (BLE.md): it NOTIFIES whole 128-byte dserv frames (MTU >= 131) already stamped
 * under its own source name (extio/<client>/...). This module scans, connects up
 * to CONFIG_BT_MAX_CONN peers, subscribes to each, and enqueues received frames.
 *
 * Threading: notifications land on the BT RX thread; box_ble_poll() hands frames
 * to the main loop, which is the SINGLE caller of box_uplink_send() -- so the
 * uplink never sees concurrent producers.
 */
#ifndef BOX_BLE_H
#define BOX_BLE_H

#include <stdint.h>

/* Enable the controller, register callbacks, and start scanning. 0 on success.
 * IDEMPOTENT -- safe to call again to bring the radio up at runtime. */
int box_ble_init(void);

/* 1 iff the radio has been brought up. */
int box_ble_active(void);

/* Dequeue one received frame (128 bytes into out). Returns 1 if a frame was
 * filled, 0 if the queue is empty. Drain in a loop each service pass. */
int box_ble_poll(uint8_t *out);

/* Number of peripherals currently connected (telemetry / fleet-ceiling check). */
int box_ble_conn_count(void);

/* 1 iff a scan is actually RUNNING right now -- which is not implied by
 * box_ble_active(). The radio can be enabled with no scan in progress: the scan
 * start can fail, and it is also stopped deliberately while a connection is
 * being established and whenever the fleet is full. A central that is up but
 * not scanning is deaf to new peripherals, so the two must be reported apart. */
int box_ble_scanning(void);

/* Last scan-start failure (0 = none). Distinguishes "not scanning because the
 * fleet is full or a connect is in flight" -- normal -- from "not scanning
 * because the controller refused", which is a fault. */
int box_ble_scan_err(void);

#endif /* BOX_BLE_H */
