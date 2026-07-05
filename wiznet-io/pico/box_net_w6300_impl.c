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
};

/* (Boot-time PHY cable auto-detect was removed: the W6300 PHYSR link/cable bits proved
 * unreliable at cold boot and false-selected Ethernet with no cable. Transport is now
 * the persisted flash setting -- see box_net_select in box_net_dual_impl.c.) */
