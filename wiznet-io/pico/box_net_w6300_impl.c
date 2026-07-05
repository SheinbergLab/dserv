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

/*
 * Auto-detect: is a live Ethernet cable present? Brings the W6300 up (once; shares
 * bn_hw_up with box_net_init so no double PIO/DMA claim), confirms a W6300 is actually
 * there via getCIDR() (NOT wizchip_check(), which while(1)-hangs on a chipless board
 * like a plain Pico 2), then polls the PHY link bit for up to ~2 s. `service` is pumped
 * throughout so the USB console keeps enumerating during the wait (tusb_init() has
 * already started enumeration and it's serviced only from the main loop). Returns 1 =>
 * live link (-> Ethernet), 0 => no chip or no link within the window (-> USB).
 */
int box_net_w6300_link_up(void (*service)(void))
{
    bn_hw_bringup();
    int cidr = getCIDR();
    if (cidr != 0x6300) {                        /* no W6300 present (e.g. plain Pico 2) -> USB */
        printf("net-probe: no W6300 (CIDR=0x%04x) -> USB\n", cidr);
        return 0;
    }

    /* Go Ethernet as soon as a CABLE is detected -- PHYSR_CAB asserts well before full
     * link auto-negotiation (which is what the first 2s attempt was missing). NB the CAB
     * bit is INVERTED: bit SET = cable OFF, so cable-present = bit CLEAR. Fall back to the
     * LNK bit too. Pump `service` so the USB console keeps enumerating during the wait. */
    absolute_time_t deadline = make_timeout_time_ms(3000);
    uint8_t sr = 0;
    while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        sr = getPHYSR();
        int cable = !(sr & PHYSR_CAB);          /* CAB clear => cable present */
        int link  =  (sr & PHYSR_LNK);          /* LNK set   => link up       */
        if (cable || link) {
            printf("net-probe: %s (PHYSR=0x%02x) -> Ethernet\n", link ? "link up" : "cable in", sr);
            return 1;
        }
        if (service) service();
        sleep_ms(10);
    }
    printf("net-probe: no cable/link in 3s (PHYSR=0x%02x) -> USB\n", sr);
    return 0;
}
