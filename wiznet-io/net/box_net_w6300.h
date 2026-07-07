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
#include "dhcp.h"
#include "pico/unique_id.h"
#include "pico/time.h"
#include <string.h>

#define BOX_NET_SN     3   /* W6300 socket: config server (dserv -> box)   */
#define BOX_NET_CLI_SN 4   /* W6300 socket: state client  (box -> dserv)   */
#define BOX_NET_TMP_SN 5   /* W6300 socket: transient self-registration    */
#define BN_DHCP_SN     0   /* W6300 socket: DHCP client (lease + renewal)  */

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
static uint8_t bn_cli_ka;        /* state-client keepalive set once per connection */
static uint8_t bn_cli_inflight;  /* state-client SEND issued, SENDOK not yet observed */

static inline const char *box_net_backend_name(void) { return "w6300"; }

/* ---- unique MAC from the RP2350 board id ----
 * Every box ships with the same compiled MAC; two on one LAN => ARP/DHCP chaos.
 * Derive a per-board MAC (WIZnet OUI 00:08:DC + 3 bytes hashed from the flash
 * unique id) so each box is unique with zero config. Keep the IPv6 link-local
 * EUI-64 suffix consistent with it. */
static inline void bn_make_mac(void)
{
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    uint32_t h = 2166136261u;                       /* FNV-1a over all id bytes */
    for (unsigned i = 0; i < sizeof id.id; i++) { h ^= id.id[i]; h *= 16777619u; }

    bn_netinfo.mac[0] = 0x00; bn_netinfo.mac[1] = 0x08; bn_netinfo.mac[2] = 0xDC;
    bn_netinfo.mac[3] = (uint8_t)(h >> 16);
    bn_netinfo.mac[4] = (uint8_t)(h >> 8);
    bn_netinfo.mac[5] = (uint8_t) h;
#if _WIZCHIP_ > W5500
    /* fe80::(mac0^0x02):mac1:mac2:ff:fe:mac3:mac4:mac5 (EUI-64, U/L bit flipped) */
    bn_netinfo.lla[8]  = bn_netinfo.mac[0] ^ 0x02;
    bn_netinfo.lla[9]  = bn_netinfo.mac[1];
    bn_netinfo.lla[10] = bn_netinfo.mac[2];
    bn_netinfo.lla[11] = 0xff; bn_netinfo.lla[12] = 0xfe;
    bn_netinfo.lla[13] = bn_netinfo.mac[3];
    bn_netinfo.lla[14] = bn_netinfo.mac[4];
    bn_netinfo.lla[15] = bn_netinfo.mac[5];
#endif
}

/* ---- DHCP ---- */
static uint8_t          bn_dhcp_buf[1024];          /* >= RIP_MSG (548)          */
static repeating_timer_t bn_dhcp_timer;
static uint8_t          bn_dhcp_active;             /* DHCP mode: service in poll */
static uint8_t          bn_booted;                  /* init done -> sockets may be up */
static volatile uint8_t bn_leased;                  /* a lease has been applied   */
static volatile uint8_t bn_ip_changed;              /* post-boot IP change        */

/* WIZnet DHCP wants a 1 Hz tick to drive its retransmit/lease timers. */
static bool bn_dhcp_1s_cb(repeating_timer_t *t) { (void) t; DHCP_time_handler(); return true; }

/* assign + update callback: pull the lease into bn_netinfo and apply it. Any IP
 * change AFTER boot (incl. the first lease if it lands once sockets are up, or a
 * renewal to a new address) flags a recycle so the TCP sockets re-bind. */
static void bn_dhcp_apply(void)
{
    uint8_t ip[4]; getIPfromDHCP(ip);
    if (bn_booted && memcmp(ip, bn_netinfo.ip, 4) != 0) bn_ip_changed = 1;
    memcpy(bn_netinfo.ip, ip, 4);
    getGWfromDHCP(bn_netinfo.gw);
    getSNfromDHCP(bn_netinfo.sn);
    getDNSfromDHCP(bn_netinfo.dns);
    network_initialize(bn_netinfo);
    bn_leased = 1;
    printf("dhcp: leased %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
}
static void bn_dhcp_conflict(void) { /* keep going; next DHCP_run re-discovers */ }

static inline void bn_apply_static(const pico_config_t *cfg)
{
    if (cfg->net_ip[0] || cfg->net_ip[1] || cfg->net_ip[2] || cfg->net_ip[3])
        memcpy(bn_netinfo.ip, cfg->net_ip, 4);      /* persisted static IP, else compiled default */
    bn_netinfo.dhcp   = NETINFO_STATIC;
#if _WIZCHIP_ > W5500
    bn_netinfo.ipmode = NETINFO_STATIC_ALL;
#endif
    network_initialize(bn_netinfo);
}

/* SPI + wizchip bring-up, done exactly ONCE. Shared (this header's file scope) with
 * box_net_w6300_link_up() in the dual image so the auto-detect probe and the following
 * box_net_init() don't re-run wizchip_spi_initialize() and double-claim the PIO/DMA
 * (which panics). Harmless in the single wired build (called once anyway). */
static uint8_t bn_hw_up = 0;
static inline void bn_hw_bringup(void)
{
    if (bn_hw_up) return;
    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    bn_hw_up = 1;
}

static inline int box_net_init(const pico_config_t *cfg)
{
    bn_make_mac();
    bn_hw_bringup();
#ifndef BOX_NET_DUAL
    wizchip_check();   /* wired build: chip is always present; while(1)-on-mismatch is fine.
                        * The dual build reads getCIDR() itself in box_net_w6300_link_up(). */
#endif

    if (cfg->net_mode == NET_MODE_STATIC) {
        bn_apply_static(cfg);
        print_network_information(bn_netinfo);
        bn_booted = 1;
        return 0;
    }

    /* DHCP (default): keep the client running for the box's whole life so it
     * self-heals a late/rebooted DHCP server or a boot-before-router race. No
     * static fallback -- the compiled default can't route on the lease subnet,
     * and `net mode static` is the explicit path for no-DHCP wiring. */
    bn_netinfo.dhcp   = NETINFO_DHCP;
#if _WIZCHIP_ > W5500
    bn_netinfo.ipmode = NETINFO_DHCP_V4;            /* IPv4 lease only            */
#endif
    memset(bn_netinfo.ip, 0, 4);                    /* no IP until the lease lands */
    network_initialize(bn_netinfo);                 /* sets MAC/SHAR before DHCP   */
    DHCP_init(BN_DHCP_SN, bn_dhcp_buf);
    reg_dhcp_cbfunc(bn_dhcp_apply, bn_dhcp_apply, bn_dhcp_conflict);
    add_repeating_timer_ms(1000, bn_dhcp_1s_cb, NULL, &bn_dhcp_timer);
    bn_dhcp_active = 1;

#ifndef BOX_NET_DUAL
    /* Wired build: briefly wait for the first lease just to log the IP. The DUAL image
     * MUST NOT block here -- tusb_init() has already begun USB enumeration and the
     * console CDC is serviced only from the main loop, so an 8 s stall kills it. DHCP
     * still runs every loop via box_net_poll(), so the lease just lands a little later. */
    absolute_time_t deadline = make_timeout_time_ms(8000);   /* wait to log the IP */
    while (!bn_leased && absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        DHCP_run();
        sleep_ms(10);
    }
    if (!bn_leased) printf("dhcp: no lease yet, will keep trying\n");
    print_network_information(bn_netinfo);
#endif
    bn_booted = 1;
    return 0;
}

static inline void box_net_poll(void)
{
    if (!bn_dhcp_active) return;                     /* static: nothing to service */
    DHCP_run();                                      /* acquire / maintain / renew */
    if (bn_ip_changed) {                             /* new IP -> recycle sockets  */
        bn_ip_changed = 0;
        disconnect(BOX_NET_SN);     close(BOX_NET_SN);
        disconnect(BOX_NET_CLI_SN); close(BOX_NET_CLI_SN);
        bn_prev_status = SOCK_CLOSED;               /* server re-listens          */
        bn_cli_ka = 0; bn_cli_inflight = 0;         /* client reconnects -> re-reg */
    }
}

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
        bn_cli_ka = 0; bn_cli_inflight = 0;
        return 0;
    case SOCK_INIT: {
        uint8_t dip[4] = { ip[0], ip[1], ip[2], ip[3] };
        connect(BOX_NET_CLI_SN, dip, port, 4);
        return 0;
    }
    case SOCK_CLOSED:
        bn_cli_ka = 0; bn_cli_inflight = 0;
        socket(BOX_NET_CLI_SN, Sn_MR_TCP4 | SF_TCP_NODELAY, 50000 + BOX_NET_CLI_SN, SOCK_IO_NONBLOCK);
        return 0;
    default:
        return 0;
    }
}

/* Publish one 128B frame, best-effort and BOUNDED -- the superloop must never
 * stall on a slow/stalled dserv.
 *
 * The vendored send() (socket.c, the IPV6_AVAILABLE variant the W6300 compiles)
 * copies the frame into the chip's TX buffer -- advancing Sn_TX_WR -- BEFORE its
 * "previous SEND finished?" (SENDOK) gate, and signals busy by returning
 * SOCK_BUSY == 0, not a negative. Two consequences shape this wrapper:
 *   - NEVER loop retrying on 0: each retry copies a DUPLICATE frame that the
 *     eventual SEND command then puts on the wire, and a peer that stops reading
 *     (zero window, socket still ESTABLISHED) turns the retry into an unbounded
 *     superloop wedge that even the keepalive can't break.
 *   - a 0 return is not loss: the frame is parked in TX and rides out with the
 *     next send()'s SEND command (the 1 Hz heartbeat at the latest).
 * So: bounded wait for the previous SENDOK (sticky IR bit, consumed inside
 * send(); ~15us wire time for 128B, deadline well above), drop outright if TX
 * has no room (peer stalled), then call send() exactly once. */
static inline int box_net_client_send(const uint8_t *buf, int len)
{
    if (bn_cli_inflight) {                          /* previous SEND still unconfirmed? */
        absolute_time_t dl = make_timeout_time_us(400);
        while (!(getSn_IR(BOX_NET_CLI_SN) & Sn_IR_SENDOK)) {
            uint8_t st;
            getsockopt(BOX_NET_CLI_SN, SO_STATUS, &st);
            if (st != SOCK_ESTABLISHED && st != SOCK_CLOSE_WAIT) { bn_cli_inflight = 0; return -1; }
            if (absolute_time_diff_us(get_absolute_time(), dl) <= 0) break;   /* frame parks below */
        }
    }
    if (getSn_TX_FSR(BOX_NET_CLI_SN) < (uint16_t) len) return -1;   /* no room: drop, stay alive */
    int32_t r = send(BOX_NET_CLI_SN, (uint8_t *) buf, (uint16_t) len);
    if (r < 0) { bn_cli_inflight = 0; return -1; }
    bn_cli_inflight = 1;   /* r==len: SEND issued. r==0: parked behind the pending SEND. */
    return 0;
}

/* The box's own IP (what it advertises to dserv in %reg). */
static inline void box_net_local_ip(uint8_t out[4])
{ out[0]=bn_netinfo.ip[0]; out[1]=bn_netinfo.ip[1]; out[2]=bn_netinfo.ip[2]; out[3]=bn_netinfo.ip[3]; }

/* Send ONE text command (e.g. "%reg ...\n") to dserv on a transient
 * connection and wait (bounded) for the reply, so dserv has processed it
 * before we move on. One command per connection sidesteps dserv's greedy '%'
 * reader. Blocks the loop briefly (~ms, hard-capped at ~0.3s by TIME -- the old
 * iteration bounds were ~0.5-2s of SPI polling each when dserv was accepting
 * but not answering, x4 commands per self_register); only on (re)connect. 0/-1. */
static inline int box_net_send_command(const uint8_t dserv_ip[4], uint16_t port,
                                       const char *cmd)
{
    const uint8_t sn = BOX_NET_TMP_SN;
    uint8_t st, d[4] = { dserv_ip[0], dserv_ip[1], dserv_ip[2], dserv_ip[3] };
    uint16_t sz;
    absolute_time_t dl;

    disconnect(sn); close(sn);
    if (socket(sn, Sn_MR_TCP4 | SF_TCP_NODELAY, 55000, SOCK_IO_NONBLOCK) != sn) return -1;
    connect(sn, d, port, 4);                        /* non-blocking: poll below */

    dl = make_timeout_time_ms(100);                 /* bounded wait: ESTABLISHED */
    for (;;) {
        getsockopt(sn, SO_STATUS, &st);
        if (st == SOCK_ESTABLISHED || st == SOCK_CLOSED) break;
        if (absolute_time_diff_us(get_absolute_time(), dl) <= 0) break;
    }
    if (st != SOCK_ESTABLISHED) { disconnect(sn); close(sn); return -1; }

    send(sn, (uint8_t *) cmd, (uint16_t) strlen(cmd));

    dl = make_timeout_time_ms(200);                 /* bounded wait: dserv reply */
    for (;;) {
        getsockopt(sn, SO_RECVBUF, &sz);
        if (sz) { uint8_t tmp[16]; recv(sn, tmp, sz > (uint16_t) sizeof tmp ? (uint16_t) sizeof tmp : sz); break; }
        getsockopt(sn, SO_STATUS, &st);
        if (st != SOCK_ESTABLISHED) break;
        if (absolute_time_diff_us(get_absolute_time(), dl) <= 0) break;
    }
    disconnect(sn); close(sn);
    return 0;
}

#endif /* BOX_NET_W6300_H */
