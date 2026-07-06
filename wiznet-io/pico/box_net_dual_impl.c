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
 * Flip box_net_active to the backend for `mode` (XPORT_ETH -> W6300, else USB).
 * `mode` is decided by the GP28 hardware strap, read in main() at boot -- NOT from a
 * persisted flash setting. (A stale `mode eth` in flash on a box with no cable used to
 * hang W6300 init forever; a strap can't go stale. There is deliberately no boot-time
 * PHY cable auto-detect -- the W6300 link registers proved unreliable at cold boot.)
 */
void box_net_select(int mode, const pico_config_t *cfg)
{
    (void) cfg;
    box_net_active = (mode == XPORT_ETH) ? &box_net_w6300_vt : &box_net_usb_vt;
}
