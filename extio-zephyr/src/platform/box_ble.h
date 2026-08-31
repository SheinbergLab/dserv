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
#include "dserv_config.h"   /* ble_latency: the manager reads it live */

/* Enable the controller, register callbacks, and start scanning. 0 on success.
 * IDEMPOTENT -- safe to call again to bring the radio up at runtime. */
int box_ble_init(void);

/* 1 iff the radio has been brought up. */
int box_ble_active(void);

/* Dequeue one received frame (128 bytes into out). Returns 1 if a frame was
 * filled, 0 if the queue is empty. Drain in a loop each service pass. */
int box_ble_poll(uint8_t *out);

/* ---- the DOWNLINK: host -> peripheral ----
 *
 * Forward one whole frame to whichever connected peripheral owns `name`
 * (`<class>/<box>/...`, e.g. `extio/hh1/cmd/...`). Returns 1 if a peer took it,
 * 0 if no connected peer answers to that name (the caller then treats the frame
 * as it always did), <0 if the GATT write failed.
 *
 * This half did not exist until 2026-08-31, and its absence was invisible
 * because the pipe LOOKED complete: hh1's events flowed, its manifest appeared
 * in dserv, and nothing reported an error. What was missing is everything that
 * travels the other way -- `config/` and `cmd/` pushes ("configure
 * peripherals from home base", which the host already speaks per-name), and,
 * more consequentially, the `'E'` echo-sync frames. Without a write path the
 * receiver cannot run the RTT estimator, cannot learn a peripheral's clock
 * offset, and therefore cannot rewrite the ts field at the radio boundary --
 * so source-stamped events arrive in the PERIPHERAL's clock domain and are
 * unusable for timing (measured: hh1 edges landed carrying its ~1000 s uptime).
 *
 * The peer is matched on the name it PUBLISHES, learned from its own frames,
 * not on the advertised GAP name -- see box_ble.c. */
int box_ble_forward(const char *name, uint16_t namelen, const uint8_t *frame);

/* ---- echo-sync: run the clock estimator ----
 *
 * Call once per service pass. Sends one echo REQUEST per connected peer every
 * ECHO_INTERVAL_MS; replies are consumed on the BT thread and never relayed.
 *
 * What this buys, and why box_ble_poll() is useless for timing without it: a
 * peripheral stamps its events at GPIO IRQ on ITS OWN clock, and nothing else
 * in the system knows what that clock reads. Until the receiver has measured
 * the offset it cannot translate, so it publishes 0 and dserv arrival-stamps
 * (accurate to radio latency, ~10-50 ms). Once synced, box_ble_poll() hands
 * back frames already mapped into THIS box's clock, and main.c's usual
 * box->dserv sync finishes the job.
 *
 * Also runs the ADAPTIVE PERIPHERAL LATENCY manager (BLE.md "Power"), which is
 * why it needs the config. At connect the pipe runs latency 0 -- every event
 * listened for, best for echo convergence, worst for the peripheral's battery.
 * Once the estimator has settled we raise latency to cfg->ble_latency so the
 * peripheral may SKIP idle events, then drop back to 0 periodically for a short
 * SYNC BURST so the clock stays disciplined against drift.
 *
 * THE PROPERTY THAT MAKES THIS SAFE: peripheral latency lets the peripheral
 * skip LISTENING, but it still wakes to TRANSMIT on any event -- so button and
 * joystick edges are delivered just as fast at latency 4 as at latency 0. Only
 * receiver->peripheral writes and the echo RTT coarsen, and both are
 * latency-tolerant. cfg->ble_latency == 0 (the factory default) keeps the
 * always-listen behaviour every measurement so far was taken against. */
void box_ble_service(const box_config_t *cfg);

/* Per-peer telemetry for the console/datapoints. A struct rather than a dozen
 * out-params, which is where this was heading. */
typedef struct {
	const char *name;         /* published prefix, or "(unnamed)"            */
	uint32_t    min_rtt_us;   /* running echo floor; 0 = none yet            */
	uint32_t    echo_tx;
	uint32_t    echo_rx;
	uint16_t    conn_int;     /* NEGOTIATED interval, 1.25 ms units (0 = ?)  */
	uint16_t    lat_applied;  /* peripheral latency currently on the link    */
	uint8_t     synced;       /* 0 none, 1 snapped, 2 windowed/rate-teaching */
} box_ble_peer_info_t;

/* Fills `out` for peer `idx`; returns 0 when that slot is not a connected peer,
 * so callers can just walk 0..CONFIG_BT_MAX_CONN-1.
 *
 * min_rtt_us bounds how good the mapping can possibly be, and conn_int is what
 * makes it interpretable -- an echo round trip cannot beat one interval, so
 * "is 25 ms bad?" has no answer without it. The two together also predict the
 * midpoint BIAS, which is the error the interval owns: measured +5.39 ms at a
 * 15 ms interval and +1.12 ms at 7.5 ms, against a hardware ground truth. */
int box_ble_peer_info(int idx, box_ble_peer_info_t *out);

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

/* Advertisements seen at all, and those carrying our service UUID. Splits the
 * two causes of a scanning central with no peers: seen=0 means nothing is
 * advertising in range (or we are not really scanning); seen>0 with matched=0
 * means something IS and we are rejecting it. */
void box_ble_scan_counts(uint32_t *seen, uint32_t *matched);

/* Why a matched advertisement did not become a peer. tries counts create
 * attempts; create_err the controller refusing outright; est_err a connection
 * that failed to establish (last_err then holds the NEGATED HCI reason);
 * dropped a link that came up and went away again. */
void box_ble_conn_counts(uint32_t *tries, uint32_t *create_err,
			 uint32_t *est_err, uint32_t *dropped, int *last_err);

#endif /* BOX_BLE_H */
