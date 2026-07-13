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
#include "pico/time.h"

const box_net_vtable_t box_net_usb_vt = {
    .init           = box_net_init,
    .poll           = box_net_poll,
    .server_poll    = box_net_server_poll,
    .client_service = box_net_client_service,
    .client_send    = box_net_client_send,
    .local_ip       = box_net_local_ip,
    .send_command   = box_net_send_command,
    .name           = box_net_backend_name,
    .phy_link       = box_net_phy_link,     /* -2: no PHY on USB */
    .server_up      = box_net_server_up,
    .client_reading = box_net_client_reading,   /* DTR on the data CDC */
    .send_command_start = box_net_send_command_start,
    .send_command_poll  = box_net_send_command_poll,
    .get_binary         = box_net_get_binary,   /* -1: pull is Ethernet-only (Stage 0) */
};

void box_net_dual_usb_init(void)         { if (!tusb_inited()) tusb_init(); }
void box_net_dual_usb_task(void)         { tud_task(); }

/* Pump TinyUSB (tightly) until the host has mounted us -- i.e. the CDCs have
 * enumerated -- or a timeout elapses. Called BEFORE the transport probe / net init so
 * the USB console+data ALWAYS come up regardless of what the W6300 side does: neither
 * the link-detect wait nor a DHCP/connect stall can then prevent enumeration. Times
 * out harmlessly when USB is power-only (no host will ever mount us). */
void box_net_dual_usb_wait_mounted(int timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!tud_mounted() && absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        tud_task();
        sleep_us(200);
    }
}
