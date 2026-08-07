/*
 * box_beacon.h -- LAN discovery beacon (UDP broadcast, port 5011).
 *
 * WHAT IT IS FOR. A box that has an address but no dserv target is invisible:
 * it never registers, so no dserv knows it exists, so nothing can tell it where
 * to connect. Until now the only way out of that state was a serial console --
 * which is why the MCXN947 had to be told an IP over a USB cable during
 * bring-up. The beacon closes it: the box announces itself on the LAN, a host
 * lists what it hears, and a human adopts the one they meant.
 *
 * The deeper argument is addressing, not convenience. A dserv host is often
 * multi-homed (the rig's is), and a NAME can always resolve to the wrong
 * interface -- a box configured from that answer points somewhere it cannot
 * route while looking healthy. An advert observed on the interface dserv
 * listens on carries the right address BY CONSTRUCTION.
 *
 * THE WIRE CONTRACT IS NOT OURS. It was set by the RP2350 boxes
 * (wiznet-io/pico/wizchip_dserv_config.c beacon_send) and is already consumed
 * by tools/extio-setup/discover.go. This port keeps every v1 field byte-for-byte
 * so one listener serves both fleets; the health fields are ADDITIVE and marked
 * by "v":2. A v1 box simply omits them.
 *
 *   {"t":"extio","v":2,"name":"box","ip":"192.168.88.54",
 *    "fw":"0.4.0+47","board":"frdm_mcxn947",
 *    "build":"frdm_mcxn947_mcxn947_cpu0",
 *    "target":"192.168.88.40:4620",
 *    "link":"up","down_ms":0,"tries":0,"ever":1}
 *
 * `target` "0.0.0.0:<port>" is the unconfigured marker -- it predates this file
 * and is the one field an adoption scheme needs most.
 *
 * WHY THE HEALTH FIELDS EXIST. `target` alone cannot distinguish a box that is
 * happily talking to 192.168.88.40 from one that has been shouting at a dserv
 * that moved away three days ago. Both advertise the same target. `link`,
 * `down_ms`, `tries` and `ever` are what separate them, and `ever` in
 * particular separates "never reached this dserv" (wrong address, wrong subnet,
 * never worked) from "was working and stopped" -- different problems that want
 * different responses.
 *
 * THE BEACON IS UNCONDITIONAL, and deliberately so. It does not wait for a
 * failure, and it is not gated on being unconfigured. A signal that only
 * appears when something is broken is a signal nobody can test while things
 * work, and it would be discovered to be broken exactly when it was needed.
 * One datagram per 1.5 s is not a cost worth optimising against that.
 *
 * ADVERTISING IS NOT CONSENTING. This announces; it never adopts. Nothing here
 * changes the box's target -- a host must connect to the config port and say so
 * explicitly, with a human behind it. There is more than one dserv on more than
 * one subnet here, and a box that attached to the first one that answered would
 * be the juicer-by-highest-ttyACM bug wearing a different hat.
 *
 * Ethernet only: with no local IPv4 address there is nothing to advertise and
 * every call is a cheap no-op, which is also what makes it safe to call
 * unconditionally from the service loop on a USB box.
 */
#ifndef BOX_BEACON_H
#define BOX_BEACON_H

#include "dserv_config.h"

#if defined(CONFIG_NETWORKING)

/* Rate-gated internally; call it every service pass. Cheap no-op when there is
 * no local IPv4 address yet (USB transport, or DHCP has not answered). */
void box_beacon_service(const box_config_t *cfg);

#else
static inline void box_beacon_service(const box_config_t *cfg) { (void) cfg; }
#endif

#endif /* BOX_BEACON_H */
