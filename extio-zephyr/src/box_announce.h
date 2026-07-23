/*
 * box_announce.h -- the box describing ITSELF to dserv.
 *
 * Everything a host needs to render or drive this box without hardcoding
 * anything: what it is (identity) and how it is configured (manifest). The Pico
 * publishes 39 state keys; this is the portable subset that applies here.
 *
 * House style, inherited verbatim from the Pico so hosts decode both alike:
 * ONE datapoint per item (each fits a 128-byte frame), not a blob. Consumers are
 * extioconf's decode, the ess joystick bit-map, and the fleet page.
 *
 * Ghost avoidance is a real part of the contract: dserv RETAINS datapoints
 * forever, so a value that simply stops being published still reads back stale.
 * Anything that can be turned OFF is therefore published as an explicit
 * "off" value rather than omitted -- obs/sync pin as -1, a cleared label as the
 * empty string, a pin dropped from pins/in.
 *
 * WHEN: the full burst fires on BOX_NET_RESET (the pass a host opens the pipe --
 * USB DTR rising or a fresh TCP session), because dserv only learns what it is
 * told while listening. The manifest half re-fires on any live label/desc/group
 * change, so an edit reaches consumers without a reconnect.
 */
#ifndef BOX_ANNOUNCE_H
#define BOX_ANNOUNCE_H

#include "dserv_config.h"
#include "box_group.h"

/* Config description only: desc, pins/in|out, obs_pin, sync_pin, feature flags,
 * per-pin labels, per-group pins/settle/quiet/idx. Cheap enough to re-fire on
 * every config edit. */
void box_announce_manifest(const box_config_t *c);

/* Full burst for a freshly connected host: identity (transport, board, build,
 * fw, boot cause, channel, ip) + the manifest + the CURRENT DI and chord levels,
 * so a UI shows live state immediately instead of waiting for the next edge. */
void box_announce_burst(const box_config_t *c, const group_rt_t *groups);

#endif /* BOX_ANNOUNCE_H */
