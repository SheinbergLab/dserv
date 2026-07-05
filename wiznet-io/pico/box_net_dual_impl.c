/*
 * box_net_dual_impl.c -- boot-time transport selection for the DUAL image.
 * Transport-agnostic (no wizchip/tusb here); it only flips box_net_active between
 * the two backend vtables. Compiled only for -DBOX_NET_DUAL.
 */
#include "box_net_dual.h"

/* Safe default: a box_net_* call before box_net_select() never NULL-derefs. */
const box_net_vtable_t *box_net_active = &box_net_usb_vt;

int box_net_is_usb(void) { return box_net_active == &box_net_usb_vt; }

/*
 * Choose the transport at boot from the persisted flash setting (cfg->transport_mode).
 * Deterministic: XPORT_ETH -> W6300 (the box was explicitly configured `mode eth`);
 * anything else (including the zero/factory default) -> USB. No boot-time cable
 * auto-detect -- the W6300 PHY status registers proved unreliable at cold boot, so we
 * trade zero-config cable sensing for a one-time `mode eth` per wired box. (A future
 * enclosure switch would slot in here: read a strap GPIO and override `mode` in main
 * before calling this.)
 */
void box_net_select(int mode, const pico_config_t *cfg)
{
    (void) cfg;
    box_net_active = (mode == XPORT_ETH) ? &box_net_w6300_vt : &box_net_usb_vt;
}
