/*
 * box_uplink.h -- the box<->dserv uplink arbiter (TRANSPORT.md).
 *
 * Exactly one authoritative uplink at a time, chosen by policy:
 *   - a physical mode strap (if wired) forces a transport, else
 *   - the persisted transport_mode: XMODE_ETH / XMODE_USB pin one, XMODE_AUTO
 *     picks Ethernet when the PHY reports carrier (debounced) else USB.
 * All producers -- local GPIO/analog and BLE ingress (block #6) -- publish
 * through box_uplink_send(), which routes to whichever uplink is active. This is
 * the RW612 health-arbitrated descendant of the Pico's boot-selected box_net_iface.
 */
#ifndef BOX_UPLINK_H
#define BOX_UPLINK_H

#include "dserv_config.h"
#include <stdint.h>

#ifndef BOX_NET_RESET
#define BOX_NET_RESET (-1)
#endif

/* One uplink transport's operations. Each wraps a box_net_* backend. */
typedef struct {
	const char *name;
	int (*init)(const box_config_t *cfg);        /* bring the link up            */
	int (*available)(void);                      /* physically usable now        */
	int (*connect)(const box_config_t *cfg);     /* establish the dserv session  */
	int (*connected)(void);                      /* session up                   */
	int (*poll)(uint8_t *buf, int max);          /* inbound; BOX_NET_RESET on connect */
	int (*send)(const uint8_t *buf, int len);    /* one 128-byte frame out       */
	int (*send_stream)(const uint8_t *buf, int len); /* k*128 gathered; bytes taken */
	int (*self_register)(const box_config_t *cfg); /* announce to dserv on connect */
} box_uplink_if;

/* Init every transport and select+connect the initial active uplink. 0 ok. */
int box_uplink_init(const box_config_t *cfg);

/* Re-evaluate selection (carrier/strap/mode), fail over, (re)connect + register.
 * Call once per service pass. */
void box_uplink_service(const box_config_t *cfg);

/* Inbound bytes from the active uplink (BOX_NET_RESET on a fresh session). */
int box_uplink_poll(uint8_t *buf, int max);

/* Send one frame out the active uplink; 0 ok, <0 if none active / send failed. */
int box_uplink_send(const uint8_t *buf, int len);

/* Gathered send for the publisher thread (box_pub.c): len = k * 128. Returns
 * frame-aligned bytes accepted, 0 = transport full (retry after a pause), -1 =
 * no active/usable uplink (drop the remainder). */
int box_uplink_send_stream(const uint8_t *buf, int len);

/* 1 iff an uplink is active and its session is up -- the publisher's "is it
 * worth dequeuing" gate. Frames enqueued while this is 0 simply wait. */
int box_uplink_ready(void);

/* Name of the active uplink ("eth"/"usb"), or "none". */
const char *box_uplink_active_name(void);


/* Routine registration chatter (`reg: matches refreshed ...`). Off by default;
 * a full registration or any failure always prints. Runtime only, not saved. */
void box_uplink_set_verbose(int on);
int  box_uplink_verbose(void);

#endif /* BOX_UPLINK_H */
