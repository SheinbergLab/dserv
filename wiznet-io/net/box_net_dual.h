/*
 * box_net_dual.h -- BOX_NET_DUAL seam: one image, Ethernet OR USB, boot-selected.
 *
 * Included by box_net.h when -DBOX_NET_DUAL. Provides the runtime dispatchers so
 * wizchip_dserv_config.c's box_net_* calls are UNCHANGED, plus box_net_select()
 * (picks the active backend at boot) and the always-on USB helpers (the dual image
 * OWNS TinyUSB in both modes so the CDC0 console is available even when the W6300
 * transport is active). The backend impls live in box_net_{w6300,usb}_impl.c; the
 * selection logic in box_net_dual_impl.c. Nothing here is compiled into the
 * single-transport builds.
 */
#ifndef BOX_NET_DUAL_H
#define BOX_NET_DUAL_H

#include "box_net_iface.h"

extern const box_net_vtable_t *box_net_active;   /* set by box_net_select(); safe default before */

void box_net_select(int mode, const pico_config_t *cfg);   /* pick transport from cfg->transport_mode (XPORT_*) */
int  box_net_is_usb(void);                                 /* 1 if the USB transport is active */

/* dual owns TinyUSB in BOTH modes: bring it up + service it (both on the RT core;
 * the CDC0 console bytes are ferried to core 0 by box_console.h). */
void box_net_dual_usb_init(void);
void box_net_dual_usb_task(void);
void box_net_dual_usb_wait_mounted(int timeout_ms);   /* boot-time helper (currently unused: the
                                                       * rt loop's tud_task completes enumeration) */

/* seam dispatchers -- keep the main loop's box_net_* call sites unchanged */
static inline int  box_net_init(const pico_config_t *c)                    { return box_net_active->init(c); }
static inline void box_net_poll(void)                                      { box_net_active->poll(); }
static inline int  box_net_server_poll(uint16_t p, uint8_t *b, int m)      { return box_net_active->server_poll(p, b, m); }
static inline int  box_net_client_service(const uint8_t ip[4], uint16_t p) { return box_net_active->client_service(ip, p); }
static inline int  box_net_client_send(const uint8_t *b, int l)            { return box_net_active->client_send(b, l); }
static inline void box_net_local_ip(uint8_t out[4])                        { box_net_active->local_ip(out); }
static inline int  box_net_send_command(const uint8_t ip[4], uint16_t p, const char *c) { return box_net_active->send_command(ip, p, c); }
static inline int  box_net_send_command_start(const uint8_t ip[4], uint16_t p, const char *c) { return box_net_active->send_command_start(ip, p, c); }
static inline int  box_net_send_command_poll(void)                         { return box_net_active->send_command_poll(); }
static inline const char *box_net_backend_name(void)                       { return box_net_active->name(); }
static inline int  box_net_server_up(void)                                 { return box_net_active->server_up(); }
static inline int  box_net_client_reading(void)                            { return box_net_active->client_reading(); }

/* PHY link goes to the W6300 backend DIRECTLY, not through box_net_active: the
 * auto-transport probe (and the `phylink` CLI) senses the cable while USB is
 * still the active transport -- that's the whole point. The board is a
 * W6300-EVB, so the chip is always there to ask. */
static inline int  box_net_phy_link(void)                                   { return box_net_w6300_vt.phy_link(); }

#endif /* BOX_NET_DUAL_H */
