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
 * Choose the transport at boot. An explicit BOX_MODE_ETH/USB is honored directly
 * (Stage 3 feeds cfg->transport_mode in as that override). BOX_MODE_AUTO auto-detects:
 * a live Ethernet cable (W6300 present + PHY link up within ~2 s) wins -> Ethernet, the
 * independent/timing tier; otherwise USB. box_net_dual_usb_task is pumped during the
 * probe so the console keeps enumerating. -DBOX_DUAL_STAGE1_ETH forces ETH for bench
 * work without a cable.
 */
void box_net_select(int mode, const pico_config_t *cfg)
{
    (void) cfg;
    if      (mode == BOX_MODE_ETH) box_net_active = &box_net_w6300_vt;
    else if (mode == BOX_MODE_USB) box_net_active = &box_net_usb_vt;
    else {                                  /* BOX_MODE_AUTO */
#if defined(BOX_DUAL_STAGE1_ETH)
        box_net_active = &box_net_w6300_vt;                 /* forced ETH (bench, no cable) */
#else
        box_net_active = box_net_w6300_link_up(box_net_dual_usb_task)
                           ? &box_net_w6300_vt : &box_net_usb_vt;
#endif
    }
}
