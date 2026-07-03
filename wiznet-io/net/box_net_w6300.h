/*
 * box_net_w6300.h -- W6300 (hardwired TCP/IP) backend for box_net.h.
 * Wraps WIZnet ioLibrary: wizchip init + one TCP server socket, polled.
 * Included only when BOX_NET_LWIP is NOT defined.
 */
#ifndef BOX_NET_W6300_H
#define BOX_NET_W6300_H

#include "dserv_config.h"
#include "port_common.h"
#include "wizchip_conf.h"
#include "wizchip_spi.h"
#include "socket.h"
#include <string.h>

#define BOX_NET_SN     3   /* W6300 socket: config server (dserv -> box)   */
#define BOX_NET_CLI_SN 4   /* W6300 socket: state client  (box -> dserv)   */
#define BOX_NET_TMP_SN 5   /* W6300 socket: transient self-registration    */

static wiz_NetInfo bn_netinfo = {
    .mac  = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56},
    .ip   = {192, 168, 11, 2},
    .sn   = {255, 255, 255, 0},
    .gw   = {192, 168, 11, 1},
    .dns  = {8, 8, 8, 8},
#if _WIZCHIP_ > W5500
    .lla  = {0xfe,0x80,0,0, 0,0,0,0, 0x02,0x08,0xdc,0xff, 0xfe,0x57,0x57,0x25},
    .gua  = {0},
    .sn6  = {0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff, 0,0,0,0, 0,0,0,0},
    .gw6  = {0},
    .dns6 = {0x20,0x01,0x48,0x60, 0x48,0x60,0,0, 0,0,0,0, 0,0,0x88,0x88},
    .ipmode = NETINFO_STATIC_ALL,
#else
    .dhcp = NETINFO_STATIC,
#endif
};

static uint8_t bn_prev_status = SOCK_CLOSED;

static inline const char *box_net_backend_name(void) { return "w6300"; }

static inline int box_net_init(const pico_config_t *cfg)
{
    if (cfg->net_ip[0] || cfg->net_ip[1] || cfg->net_ip[2] || cfg->net_ip[3])
        memcpy(bn_netinfo.ip, cfg->net_ip, 4);      /* persisted static IP */

    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    wizchip_check();
    network_initialize(bn_netinfo);
    print_network_information(bn_netinfo);
    return 0;
}

static inline void box_net_poll(void) { /* W6300 offloads TCP/IP: nothing to do */ }

static inline int box_net_server_poll(uint16_t port, uint8_t *buf, int max)
{
    uint8_t  status;
    uint16_t size;
    int32_t  ret;

    getsockopt(BOX_NET_SN, SO_STATUS, &status);

    switch (status) {
    case SOCK_ESTABLISHED:
        if (bn_prev_status != SOCK_ESTABLISHED) {   /* fresh connection */
            bn_prev_status = SOCK_ESTABLISHED;
            setSn_KPALVTR(BOX_NET_SN, 4);           /* keepalive ~20s: if dserv */
            return BOX_NET_RESET;                    /* dies, socket recycles + relistens */
        }
        getsockopt(BOX_NET_SN, SO_RECVBUF, &size);
        if (size == 0) return 0;
        if ((int) size > max) size = (uint16_t) max;
        ret = recv(BOX_NET_SN, buf, size);
        return ret > 0 ? (int) ret : 0;

    case SOCK_CLOSE_WAIT:
        getsockopt(BOX_NET_SN, SO_RECVBUF, &size);
        if (size) {
            if ((int) size > max) size = (uint16_t) max;
            ret = recv(BOX_NET_SN, buf, size);
            bn_prev_status = status;
            if (ret > 0) return (int) ret;
        }
        disconnect(BOX_NET_SN);
        bn_prev_status = SOCK_CLOSED;
        return 0;

    case SOCK_INIT:
        listen(BOX_NET_SN);
        break;
    case SOCK_CLOSED:
        socket(BOX_NET_SN, Sn_MR_TCP4 | SF_TCP_NODELAY, port, SOCK_IO_NONBLOCK);  /* no delayed-ACK */
        break;
    default:
        break;
    }
    bn_prev_status = status;
    return 0;
}

/* ---- client: box -> dserv (publish state/(keys)) ---- */
static uint8_t bn_cli_ka;    /* keepalive set once per connection */

static inline int box_net_client_service(const uint8_t ip[4], uint16_t port)
{
    uint8_t status;
    getsockopt(BOX_NET_CLI_SN, SO_STATUS, &status);
    switch (status) {
    case SOCK_ESTABLISHED:
        if (!bn_cli_ka) { setSn_KPALVTR(BOX_NET_CLI_SN, 4); bn_cli_ka = 1; return 2; }  /* just (re)connected */
        return 1;                                   /* ready to send */
    case SOCK_CLOSE_WAIT:
        disconnect(BOX_NET_CLI_SN);
        bn_cli_ka = 0;
        return 0;
    case SOCK_INIT: {
        uint8_t dip[4] = { ip[0], ip[1], ip[2], ip[3] };
        connect(BOX_NET_CLI_SN, dip, port, 4);
        return 0;
    }
    case SOCK_CLOSED:
        bn_cli_ka = 0;
        socket(BOX_NET_CLI_SN, Sn_MR_TCP4 | SF_TCP_NODELAY, 50000 + BOX_NET_CLI_SN, SOCK_IO_NONBLOCK);
        return 0;
    default:
        return 0;
    }
}

static inline int box_net_client_send(const uint8_t *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        int32_t r = send(BOX_NET_CLI_SN, (uint8_t *)(buf + sent), (uint16_t)(len - sent));
        if (r < 0) return -1;
        sent += r;
    }
    return 0;
}

/* The box's own IP (what it advertises to dserv in %reg). */
static inline void box_net_local_ip(uint8_t out[4])
{ out[0]=bn_netinfo.ip[0]; out[1]=bn_netinfo.ip[1]; out[2]=bn_netinfo.ip[2]; out[3]=bn_netinfo.ip[3]; }

/* Send ONE text command (e.g. "%reg ...\n") to dserv on a transient
 * connection and wait (bounded) for the reply, so dserv has processed it
 * before we move on. One command per connection sidesteps dserv's greedy '%'
 * reader. Blocks the loop briefly (~ms); only used on (re)connect. 0/-1. */
static inline int box_net_send_command(const uint8_t dserv_ip[4], uint16_t port,
                                       const char *cmd)
{
    const uint8_t sn = BOX_NET_TMP_SN;
    uint8_t st, d[4] = { dserv_ip[0], dserv_ip[1], dserv_ip[2], dserv_ip[3] };
    uint16_t sz;
    long i;

    disconnect(sn); close(sn);
    if (socket(sn, Sn_MR_TCP4 | SF_TCP_NODELAY, 55000, SOCK_IO_NONBLOCK) != sn) return -1;
    connect(sn, d, port, 4);                        /* non-blocking: poll below */

    for (i = 0; i < 300000; i++) {                  /* bounded wait: ESTABLISHED */
        getsockopt(sn, SO_STATUS, &st);
        if (st == SOCK_ESTABLISHED || st == SOCK_CLOSED) break;
    }
    getsockopt(sn, SO_STATUS, &st);
    if (st != SOCK_ESTABLISHED) { disconnect(sn); close(sn); return -1; }

    send(sn, (uint8_t *) cmd, (uint16_t) strlen(cmd));

    for (i = 0; i < 600000; i++) {                  /* bounded wait: dserv reply */
        getsockopt(sn, SO_RECVBUF, &sz);
        if (sz) { uint8_t tmp[16]; recv(sn, tmp, sz > (uint16_t) sizeof tmp ? (uint16_t) sizeof tmp : sz); break; }
        getsockopt(sn, SO_STATUS, &st);
        if (st != SOCK_ESTABLISHED) break;
    }
    disconnect(sn); close(sn);
    return 0;
}

#endif /* BOX_NET_W6300_H */
