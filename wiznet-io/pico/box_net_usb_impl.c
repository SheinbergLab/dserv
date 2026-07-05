/*
 * box_net_usb_impl.c -- USB-CDC backend vtable + always-on USB helpers for the DUAL
 * image. #includes the existing (untouched) box_net_usb.h and exports its
 * static-inline box_net_* functions as one vtable. Compiled only for -DBOX_NET_DUAL.
 *
 * The dual image OWNS TinyUSB in BOTH transport modes (the CDC0 console must work
 * even when the W6300 transport is active), so TinyUSB init + tud_task servicing are
 * exposed here as mode-independent helpers, not tied to the USB transport being the
 * selected one.
 */
#include "box_net_iface.h"
#include "box_net_usb.h"

const box_net_vtable_t box_net_usb_vt = {
    .init           = box_net_init,
    .poll           = box_net_poll,
    .server_poll    = box_net_server_poll,
    .client_service = box_net_client_service,
    .client_send    = box_net_client_send,
    .local_ip       = box_net_local_ip,
    .send_command   = box_net_send_command,
    .name           = box_net_backend_name,
};

void box_net_dual_usb_init(void)         { if (!tusb_inited()) tusb_init(); }
void box_net_dual_usb_console_init(void) { box_net_usb_console_init(); }
void box_net_dual_usb_task(void)         { tud_task(); }
