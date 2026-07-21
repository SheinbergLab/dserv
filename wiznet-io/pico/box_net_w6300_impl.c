/*
 * box_net_w6300_impl.c -- W6300 backend vtable for the DUAL image.
 *
 * #includes the existing (untouched) box_net_w6300.h and exports its static-inline
 * box_net_* functions as one vtable. Its own file-scope statics (bn_netinfo, DHCP
 * state, ...) live in this TU. Compiled only for -DBOX_NET_DUAL.
 */
#include "box_net_iface.h"
#include "box_net_w6300.h"

const box_net_vtable_t box_net_w6300_vt = {
    .init           = box_net_init,
    .poll           = box_net_poll,
    .server_poll    = box_net_server_poll,
    .client_service = box_net_client_service,
    .client_send    = box_net_client_send,
    .local_ip       = box_net_local_ip,
    .send_command   = box_net_send_command,
    .name           = box_net_backend_name,
    .phy_link       = box_net_phy_link,     /* lazy chip bring-up, NO vendored PHY-wait */
    .server_up      = box_net_server_up,
    .client_reading = box_net_client_reading,   /* connected socket == read */
    .send_command_start = box_net_send_command_start,
    .send_command_poll  = box_net_send_command_poll,
    .get_binary         = box_net_get_binary,   /* raw '<' pull -> flash (OTA) */
    .beacon             = box_net_beacon,        /* UDP discovery broadcast */
};

/* (The 2026-07-05 boot-time cable auto-detect read PHYSR the instant the chip came
 * out of reset -- before autonegotiation (1-3s) could complete -- which is why it
 * proved "unreliable" and was dropped for the strap. phy_link above is the sound
 * replacement: the caller samples it debounced over a window, from the running loop.) */
