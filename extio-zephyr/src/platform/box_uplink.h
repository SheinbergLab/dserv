/*
 * box_uplink.h -- the box<->dserv uplink arbiter (TRANSPORT.md).
 *
 * Exactly one authoritative uplink at a time, chosen by policy:
 *   - a physical mode strap (if wired) forces a transport, else
 *   - the persisted transport_mode: XMODE_ETH / XMODE_USB pin one, XMODE_AUTO
 *     picks Ethernet when the PHY reports carrier (debounced) else USB.
 * Producers publish through box_pub (box_pub.h), whose thread is the one
 * writer of box_uplink_send/send_stream; the arbiter routes each call to
 * whichever uplink is active, under a lock that also covers poll and the
 * service pass that can swap transports. This is the RW612 health-arbitrated
 * descendant of the Pico's boot-selected box_net_iface.
 */
#ifndef BOX_UPLINK_H
#define BOX_UPLINK_H

#include "dserv_config.h"
#include <stdint.h>

#ifndef BOX_NET_RESET
#define BOX_NET_RESET (-1)
#endif

/* What box_uplink_poll2 delivered (+29). BYTES is the historical contract
 * (a raw run for the caller's framer -- USB CDC); FRAME is one complete
 * 128-byte frame that the eth reader thread already received, stamped, and
 * fast-path-screened -- the caller dispatches it directly and must NOT
 * re-run the fast path or re-count it. */
#define BOX_UPLINK_RX_NONE   0
#define BOX_UPLINK_RX_RESET  1     /* transport (re)opened: reset framer, announce */
#define BOX_UPLINK_RX_BYTES  2     /* raw byte run in buf, *len set               */
#define BOX_UPLINK_RX_FRAME  3     /* one DSERV_MSG_LEN frame in buf, *arr_us set;
                                    * reader already counted + fast-screened it   */
#define BOX_UPLINK_RX_FRAME_RAW 4  /* +32: frame DEFERRED by the reader's order
                                    * fence -- not counted, not screened; the
                                    * dispatcher runs the full path, in order    */

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
	/* Optional (+29): kind-based inbound for transports with a reader thread.
	 * Absent -> box_uplink_poll2 adapts the legacy poll. */
	int (*poll2)(uint8_t *buf, int max, int *len, uint64_t *arr_us);
} box_uplink_if;

/* Init every transport and select+connect the initial active uplink. 0 ok. */
int box_uplink_init(const box_config_t *cfg);

/* Re-evaluate selection (carrier/strap/mode), fail over, (re)connect + register.
 * Call once per service pass. */
void box_uplink_service(const box_config_t *cfg);

/* Inbound bytes from the active uplink (BOX_NET_RESET on a fresh session). */
int box_uplink_poll(uint8_t *buf, int max);

/* Kind-based inbound (+29): returns a BOX_UPLINK_RX_* kind. BYTES fills buf
 * and *len; FRAME fills buf with one whole frame and *arr_us with its
 * arrival stamp (0 = transport could not stamp). */
int box_uplink_poll2(uint8_t *buf, int max, int *len, uint64_t *arr_us);

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

#if defined(CONFIG_NETWORKING)
/* Registration health, for the discovery beacon. Any pointer may be NULL.
 *
 *   up       1 iff dserv's connect-back is live RIGHT NOW (read live, so it
 *            cannot go stale when eth is not the active uplink)
 *   down_ms  how long it has been down; 0 while up. NOT the retry-backoff
 *            timer -- that resets every attempt and could never say "down for
 *            six minutes", which is the only thing worth reporting
 *   tries    re-registration attempts since it went down (0 while up)
 *   ever_up  1 iff a registration has EVER completed -- separates "never
 *            reached this dserv" (wrong target, wrong subnet) from "was
 *            working and stopped", which want different responses from a host
 */
void box_uplink_reg_health(int *up, uint32_t *down_ms, uint16_t *tries,
			   int *ever_up);
#endif

#endif /* BOX_UPLINK_H */
