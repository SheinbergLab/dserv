/*
 * box_net_iface.h -- runtime transport interface for the DUAL (Ethernet+USB) box.
 *
 * The single-transport builds pick a backend at COMPILE time (box_net.h #if/#elif).
 * The dual build instead compiles BOTH backends and dispatches through this vtable,
 * chosen at boot (box_net_dual.h / box_net_select). Each backend translation unit
 * (box_net_{w6300,usb}_impl.c) #includes its existing backend header untouched and
 * exports one vtable; because they are separate TUs their identically-named
 * `static inline box_net_*` functions never collide.
 *
 * BOX_NET_RESET lives here (not box_net.h) so the backend headers still see it when
 * an impl TU includes them directly, without pulling in box_net.h's dispatchers.
 */
#ifndef BOX_NET_IFACE_H
#define BOX_NET_IFACE_H

#include "dserv_config.h"
#include <stdint.h>

#define BOX_NET_RESET (-1)   /* box_net_server_poll: reset framer on new conn */

/* Sink for a streamed binary pull (box_net_get_binary): called with successive
 * spans of a datapoint's raw VALUE, never the whole thing buffered. 0 = keep
 * going, <0 = abort. Home of the typedef; the single-transport w6300 build sees
 * it because box_net.h includes this header before box_net_w6300.h. Guarded so a
 * TU that reaches it twice (iface + a backend header that once redeclared it)
 * doesn't double-define. */
#ifndef BOX_NET_BIN_SINK_DEFINED
#define BOX_NET_BIN_SINK_DEFINED
typedef int (*box_net_bin_sink)(void *ud, const uint8_t *data, uint32_t len);
#endif

typedef struct {
    int         (*init)(const pico_config_t *cfg);
    void        (*poll)(void);
    int         (*server_poll)(uint16_t port, uint8_t *buf, int max);
    int         (*client_service)(const uint8_t dserv_ip[4], uint16_t port);
    int         (*client_send)(const uint8_t *buf, int len);
    void        (*local_ip)(uint8_t out[4]);
    int         (*send_command)(const uint8_t dserv_ip[4], uint16_t port, const char *cmd);
    const char *(*name)(void);
    int         (*phy_link)(void);     /* 1 up, 0 down, -1 chip error, -2 no PHY on this transport.
                                        * w6300: lazily brings the chip up (sans the vendored PHY-wait),
                                        * so the auto probe + `phylink` CLI work while USB is active. */
    int         (*server_up)(void);    /* 1 iff the dserv->box config path is alive (w6300: config
                                        * server socket ESTABLISHED; usb: enumerated). Feeds the
                                        * registration self-heal watchdog. */
    int         (*client_reading)(void); /* 1 iff the FAR END is actually draining our client stream:
                                          * usb = host opened the DATA CDC (DTR), NOT mere enumeration;
                                          * socket = connected peer always reads (return 1). Gates the
                                          * connect burst so it isn't fired into a not-yet-open USB tty. */
    /* Async variant of send_command for the RT loop: start one command, then
     * poll each pass (us-bounded: ~1-2 SPI reads) until it resolves. w6300 runs
     * a per-pass socket state machine; usb resolves immediately (CDC write).
     *   start: 0 accepted, -1 can't start (previous command still in flight)
     *   poll:  0 in progress, 1 done + dserv accepted, -1 done + failed        */
    int         (*send_command_start)(const uint8_t dserv_ip[4], uint16_t port, const char *cmd);
    int         (*send_command_poll)(void);
    /* Pull one datapoint's raw binary value, streamed to `sink` (OTA image fetch;
     * blocks the caller, bounded). Returns datalen (>=0) or -1 on error. Only the
     * w6300 backend implements it (transient socket + raw '<' get); usb/lwip stub
     * to -1 (Stage-0 pull is Ethernet-only -- see OTA.md). */
    int         (*get_binary)(const uint8_t dserv_ip[4], uint16_t port,
                              const char *key, box_net_bin_sink sink, void *ud);
} box_net_vtable_t;

extern const box_net_vtable_t box_net_w6300_vt;
extern const box_net_vtable_t box_net_usb_vt;

#endif /* BOX_NET_IFACE_H */
