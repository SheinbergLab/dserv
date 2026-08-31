/*
 * box_ble_periph.h -- the BLE PERIPHERAL end of the frozen d5e7000x pipe.
 *
 * The mirror of box_ble.c. Where that file is a hub holding many links, this is
 * one battery device holding one: it advertises, serves the two characteristics
 * (dserv_ble.h), and presents the link to the rest of the firmware as an
 * ordinary uplink -- so config dispatch, the announce/manifest, DI, groups, ain
 * groups, box_cli and box_persist all work unchanged above it. That seam is why
 * the peripheral is one file rather than a port.
 *
 * TWO THINGS THIS END OWES THE OTHER, and both are timing rather than plumbing:
 *
 *   1. THE ECHO REFLEX. The receiver measures the link by round trip and splits
 *      the difference (`r0 + rtt/2`). Any latency this end adds between
 *      receiving a request and answering it lands in one leg only, so it does
 *      not average out -- it becomes systematic BIAS in every timestamp the
 *      receiver maps. So the reply is built and sent from the ATT write
 *      callback itself: never a work queue, never the service loop. Measured on
 *      the Pico pair, that asymmetry is worth ~5 ms at a 15 ms connection
 *      interval and ~1 ms at 7.5 ms.
 *
 *   2. RAW SOURCE TIME. Events are stamped on THIS box's clock and sent
 *      untranslated; the receiver rewrites the ts field once, at the radio
 *      boundary. A peripheral must never map to dserv time itself -- it is
 *      never anchored, so box_clock_stamp() returns 0 and dserv would
 *      arrival-stamp away the source stamp. See CONFIG_BOX_BLE_PERIPHERAL.
 */
#ifndef BOX_BLE_PERIPH_H
#define BOX_BLE_PERIPH_H

#include <stdint.h>
#include "dserv_config.h"

/* Bring the radio up and start advertising as `extio-<cfg name>`. Idempotent.
 * 0 on success. */
int box_ble_periph_init(const box_config_t *cfg);

/* 1 iff the radio is up. Says NOTHING about whether anyone can see this box --
 * bt_enable() succeeding and an advertiser running are different facts, and a
 * powered non-advertising peripheral is invisible, which looks exactly like one
 * that is switched off. Ask box_ble_periph_advertising() for that. */
int box_ble_periph_active(void);

/* 1 iff an advertiser is actually running. 0 while connected (a legacy
 * advertiser stops on connect) and 0 on failure -- box_ble_periph_adv_err()
 * then holds the errno, which is the difference between "connected, so quiet"
 * and "never started". */
int box_ble_periph_advertising(void);
int box_ble_periph_adv_err(void);

/* 1 iff a receiver is connected AND has subscribed to the TX characteristic --
 * i.e. frames sent now will actually go somewhere. The two are separate states
 * and the gap between them is real: a central connects, then discovers, then
 * writes the CCC, and anything published in that window is dropped. */
int box_ble_periph_ready(void);

/* Re-advertise under a new name after a rename (CFG_NAME). Advertising data is
 * fixed at start, so the name only changes if we stop and restart. */
void box_ble_periph_rename(const box_config_t *cfg);

/* Telemetry for the console/datapoints. Any pointer may be NULL.
 *   mtu       negotiated ATT MTU (0 = not connected). BELOW DSERV_BLE_MTU_MIN
 *             the whole-frame contract is broken and frames are refused rather
 *             than fragmented -- loudly, because a silently halved pipe would
 *             look like a flaky radio.
 *   echo_rx   echo requests answered -- the receiver's clock depends on this
 *             advancing, so a frozen count with a live link is the fingerprint
 *             of a peripheral whose timestamps are about to be meaningless.
 *   tx_ok / tx_drop  frames notified / dropped because no one was subscribed. */
void box_ble_periph_stats(uint16_t *mtu, uint32_t *echo_rx,
			  uint32_t *tx_ok, uint32_t *tx_drop);

/* ---- the uplink face (box_uplink.c wraps these in a box_uplink_if) ----
 * Same contract as box_net_usb's equivalents, so the arbiter above needs no
 * special case: send() returns the bytes taken (0 = busy, retry), send_stream()
 * the frame-aligned bytes accepted, poll() the bytes read (0 = nothing). */
int box_ble_periph_send(const uint8_t *buf, int len);
int box_ble_periph_send_stream(const uint8_t *buf, int len);
int box_ble_periph_poll(uint8_t *buf, int max);

#endif /* BOX_BLE_PERIPH_H */
