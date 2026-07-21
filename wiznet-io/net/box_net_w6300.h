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
#include "wizchip_qspi_pio.h"   /* wiznet_spi_handle_t: for the no-PHY-wait init below */
#include "socket.h"
#include "dhcp.h"
#include "pico/unique_id.h"
#include "pico/time.h"
#include <string.h>

extern alarm_pool_t *box_alarm_pool;   /* RT-core (core 1) alarm pool; see pico_gpio.h */

#define BOX_NET_SN     3   /* W6300 socket: config server (dserv -> box)   */
#define BOX_NET_CLI_SN 4   /* W6300 socket: state client  (box -> dserv)   */
#define BOX_NET_TMP_SN 5   /* W6300 socket: transient self-registration    */
#define BOX_NET_OTA_SN 6   /* W6300 socket: transient OTA binary pull       */
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

/* WIZnet DHCP wants a 1 Hz tick to drive its retransmit/lease timers. The tick
 * must interrupt the CORE THAT RUNS DHCP_run() (core 1) -- DHCP_time_handler
 * mutates the same library state, and the default timer pool interrupts core 0.
 * box_alarm_pool is the RT core's pool (see pico_gpio.h; defined in the main .c). */
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

/* Chip init WITHOUT the vendored PHY-link wait. The port's wizchip_initialize()
 * (wizchip_spi.c) ends in `do { CW_GET_PHYLINK } while (link off)` -- an
 * unbounded spin with no cable, which is what originally forced the GP28 strap
 * design. Everything before that loop is what actually initializes the chip, and
 * it's all public API off the port's global QSPI handle -- so replicate exactly
 * that and return. No link needed: sockets/DHCP simply idle until one appears,
 * and the auto-transport probe reads the link itself, debounced, later. */
extern wiznet_spi_handle_t spi_handle;      /* wizchip_spi.c:74 (non-static) */
static inline int bn_wizchip_init_nowait(void)
{
    (*spi_handle)->frame_end();
    reg_wizchip_qspi_cbfunc((*spi_handle)->read_byte, (*spi_handle)->write_byte);
    reg_wizchip_cs_cbfunc((*spi_handle)->frame_start, (*spi_handle)->frame_end);
    uint8_t memsize[2][8] = {{4,4,4,4,4,4,4,4}, {4,4,4,4,4,4,4,4}};   /* = vendored W6300 sizing */
    if (ctlwizchip(CW_INIT_WIZCHIP, (void *) memsize) == -1) return -1;
    return 0;
}

/* SPI + wizchip bring-up, done exactly ONCE (a second wizchip_spi_initialize()
 * would double-claim the PIO/DMA and panic). Shared between box_net_init() and
 * the lazy paths (box_net_phy_link from the auto probe / `phylink` CLI), which
 * may run first, while the USB transport is still the active one. */
static uint8_t bn_hw_up = 0;      /* 0 not tried, 1 up, 2 failed (chip absent/dead) */
static inline int bn_hw_bringup(void)
{
    if (bn_hw_up) return bn_hw_up == 1 ? 0 : -1;
    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    if (bn_wizchip_init_nowait() != 0) { bn_hw_up = 2; return -1; }
    bn_hw_up = 1;
    return 0;
}

/* PHY link now: 1 up, 0 down, -1 chip error. One SPI register read once the
 * hardware is up (lazy bring-up on first use). NOTE: right after power-on the
 * PHY needs 1-3s of autonegotiation before this reads true -- debounce it. */
static inline int box_net_phy_link(void)
{
    if (bn_hw_bringup() != 0) return -1;
    uint8_t l;
    if (ctlwizchip(CW_GET_PHYLINK, (void *) &l) == -1) return -1;
    return l == PHY_LINK_ON ? 1 : 0;
}

/* 1 iff dserv's connection into our config server is ESTABLISHED. This is the
 * box-visible acknowledgment that %reg landed AND dserv's connect-back
 * succeeded -- dserv never retries that connect, so the caller re-registers
 * when this stays 0 too long while the state client is up. */
static inline int box_net_server_up(void)
{
    return bn_prev_status == SOCK_ESTABLISHED;
}

/* Socket peer: an ESTABLISHED TCP connection is, by definition, being read by
 * dserv -- so the connect burst can fire the instant we're connected (this is
 * only consulted while up). The DTR subtlety is USB-only. */
static inline int box_net_client_reading(void) { return 1; }

static inline int box_net_init(const pico_config_t *cfg)
{
    bn_make_mac();
    if (bn_hw_bringup() != 0) return -1;
#ifndef BOX_NET_DUAL
    wizchip_check();   /* wired build: chip is always present; while(1)-on-mismatch is fine. */
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
    if (box_alarm_pool)
        alarm_pool_add_repeating_timer_ms(box_alarm_pool, 1000, bn_dhcp_1s_cb, NULL, &bn_dhcp_timer);
    else
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
    case SOCK_CLOSED: {
        /* Reconnect gate: without it a down dserv gets a connect attempt every
         * few loop passes -- thousands of SYNs + socket create/teardown SPI
         * churn per second. 500ms is imperceptible on recovery. */
        static absolute_time_t bn_cli_next_try;
        bn_cli_ka = 0; bn_cli_inflight = 0;
        if (absolute_time_diff_us(get_absolute_time(), bn_cli_next_try) > 0) return 0;
        bn_cli_next_try = make_timeout_time_ms(500);
        socket(BOX_NET_CLI_SN, Sn_MR_TCP4 | SF_TCP_NODELAY, 50000 + BOX_NET_CLI_SN, SOCK_IO_NONBLOCK);
        return 0;
    }
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

/* Discovery beacon: best-effort UDP broadcast of `buf` to 255.255.255.255:port.
 * A dedicated UDP socket (BOX_BEACON_SN), opened lazily on first call. Silently
 * no-ops until DHCP hands out a lease (nothing to advertise yet). The caller
 * (core 1) composes the payload + rate-gates, so this just does the transport. */
#define BOX_BEACON_SN 7        /* free W6300 socket (0/3/4/5/6 in use) */
static inline void box_net_beacon(uint16_t port, const uint8_t *buf, int len)
{
    if (!(bn_netinfo.ip[0] || bn_netinfo.ip[1] || bn_netinfo.ip[2] || bn_netinfo.ip[3]))
        return;                                   /* no lease -> no IP to announce */
    uint8_t status;
    getsockopt(BOX_BEACON_SN, SO_STATUS, &status);
    if (status != SOCK_UDP) {                     /* (re)open the UDP socket once */
        socket(BOX_BEACON_SN, Sn_MR_UDP4, port, SOCK_IO_NONBLOCK);
        return;                                   /* opened this tick; send next call */
    }
    uint8_t bcast[16] = { 255, 255, 255, 255 };
    sendto(BOX_BEACON_SN, (uint8_t *) buf, len, bcast, port, 4);
}

/* Graceful transient-socket teardown: FIN and WAIT (bounded) for the close
 * handshake to finish before killing the socket. The old `disconnect(); close();`
 * aborted the handshake -- close() right after DISCON kills the socket before
 * the peer's FIN/ACK is answered -- leaving the dserv host's kernel holding the
 * tuple (LAST_ACK/FIN_WAIT) for seconds. With a FIXED source port, the next
 * command's SYN hit that ghost tuple and was dropped: registration commands
 * were silently lost, most often the LAST one (the ess/in_obs clock-sync
 * match). ~1 RTT on a healthy LAN; 50ms cap if the peer vanished. */
static inline void bn_tmp_close(uint8_t sn)
{
    uint8_t st;
    getsockopt(sn, SO_STATUS, &st);
    if (st != SOCK_CLOSED) {
        disconnect(sn);                             /* nonblock: returns at once */
        absolute_time_t dl = make_timeout_time_ms(50);
        for (;;) {
            getsockopt(sn, SO_STATUS, &st);
            if (st == SOCK_CLOSED) break;
            if (absolute_time_diff_us(get_absolute_time(), dl) <= 0) break;
        }
    }
    close(sn);
}

/* Send ONE text command (e.g. "%reg ...\n") to dserv on a transient
 * connection and wait (bounded) for the reply, so dserv has processed it
 * before we move on. One command per connection sidesteps dserv's greedy '%'
 * reader. The source port ROTATES (55000-55007) so back-to-back commands never
 * reuse a tuple the peer kernel may still be aging out -- belt to bn_tmp_close's
 * suspenders. Blocks the loop briefly (~ms, hard-capped ~0.35s); only used at
 * registration time.
 * dserv answers every '%' command with "<rc> ...\n", rc 1 = accepted -- so the
 * return value is truthful: 0 = dserv ACCEPTED the command; -1 = no/negative
 * reply (down, wedged, %reg connect-back failed, %match with no registration). */
static inline int box_net_send_command(const uint8_t dserv_ip[4], uint16_t port,
                                       const char *cmd)
{
    const uint8_t sn = BOX_NET_TMP_SN;
    static uint8_t bn_tmp_rr;
    uint8_t st, d[4] = { dserv_ip[0], dserv_ip[1], dserv_ip[2], dserv_ip[3] };
    uint16_t sz;
    uint8_t rep[16] = {0};
    int got_reply = 0;
    absolute_time_t dl;

    bn_tmp_close(sn);
    if (socket(sn, Sn_MR_TCP4 | SF_TCP_NODELAY,
               (uint16_t)(55000 + (bn_tmp_rr++ & 7)), SOCK_IO_NONBLOCK) != sn) return -1;
    connect(sn, d, port, 4);                        /* non-blocking: poll below */

    dl = make_timeout_time_ms(100);                 /* bounded wait: ESTABLISHED */
    for (;;) {
        getsockopt(sn, SO_STATUS, &st);
        if (st == SOCK_ESTABLISHED || st == SOCK_CLOSED) break;
        if (absolute_time_diff_us(get_absolute_time(), dl) <= 0) break;
    }
    if (st != SOCK_ESTABLISHED) { bn_tmp_close(sn); return -1; }

    send(sn, (uint8_t *) cmd, (uint16_t) strlen(cmd));

    dl = make_timeout_time_ms(200);                 /* bounded wait: dserv reply */
    for (;;) {
        getsockopt(sn, SO_RECVBUF, &sz);
        if (sz) { recv(sn, rep, sz > (uint16_t) sizeof rep ? (uint16_t) sizeof rep : sz); got_reply = 1; break; }
        getsockopt(sn, SO_STATUS, &st);
        if (st != SOCK_ESTABLISHED) break;
        if (absolute_time_diff_us(get_absolute_time(), dl) <= 0) break;
    }
    bn_tmp_close(sn);
    return (got_reply && rep[0] == '1') ? 0 : -1;
}

/* box_net_bin_sink lives in box_net_iface.h now (box_net.h includes it before
 * this header for the single-w6300 build; box_net_w6300_impl.c does the same for
 * the dual build). */

/* Receive exactly n bytes from sn into buf, bounded (idle timer resets on
 * progress). 0 ok, -1 on close/timeout. */
static inline int bn_recv_exact(uint8_t sn, uint8_t *buf, uint32_t n, uint32_t to_ms)
{
    uint32_t got = 0; uint8_t st;
    absolute_time_t dl = make_timeout_time_ms(to_ms);
    while (got < n) {
        uint16_t avail; getsockopt(sn, SO_RECVBUF, &avail);
        if (avail) {
            uint16_t want = (n - got) < avail ? (uint16_t)(n - got) : avail;
            int32_t r = recv(sn, buf + got, want);
            if (r <= 0) return -1;
            got += (uint32_t) r;
            dl = make_timeout_time_ms(to_ms);
        } else {
            getsockopt(sn, SO_STATUS, &st);
            if (st != SOCK_ESTABLISHED && st != SOCK_CLOSE_WAIT) return -1;
            if (absolute_time_diff_us(get_absolute_time(), dl) <= 0) return -1;
        }
    }
    return 0;
}

/* Pull one datapoint's RAW binary value from dserv via the '<' get protocol
 * (src/Dataserver.cpp): request  '<' + varlen(u16 LE) + varname; reply
 * size(int32 LE) + dpoint_to_binary = varlen(2)+varname+ts(8)+type(4)+
 * datalen(4)+data[datalen]. The DATA span is streamed to `sink` -- the 150KB
 * image never sits in SRAM. Returns datalen (>=0) on success, -1 on any error
 * (size 0 = key not found). BLOCKS the caller for the transfer (bounded);
 * OTA-only, gated to !in_obs by the caller. Uses a transient socket (SN6). */
static inline int box_net_get_binary(const uint8_t dserv_ip[4], uint16_t port,
                                     const char *key, box_net_bin_sink sink, void *ud)
{
    const uint8_t sn = BOX_NET_OTA_SN;
    uint8_t st, d[4] = { dserv_ip[0], dserv_ip[1], dserv_ip[2], dserv_ip[3] };
    uint16_t klen = (uint16_t) strlen(key);
    uint8_t hdr[64];
    if (klen == 0 || klen > 48) return -1;              /* keys are short (extio/<box>/ota/image) */

    bn_tmp_close(sn);
    if (socket(sn, Sn_MR_TCP4 | SF_TCP_NODELAY, 55010, SOCK_IO_NONBLOCK) != sn) return -1;
    connect(sn, d, port, 4);                            /* non-blocking; poll below */
    absolute_time_t dl = make_timeout_time_ms(300);
    for (;;) {
        getsockopt(sn, SO_STATUS, &st);
        if (st == SOCK_ESTABLISHED || st == SOCK_CLOSED) break;
        if (absolute_time_diff_us(get_absolute_time(), dl) <= 0) break;
    }
    if (st != SOCK_ESTABLISHED) { bn_tmp_close(sn); return -1; }

    /* request: '<' + varlen + varname (raw; NOT a text command) */
    hdr[0] = '<'; memcpy(&hdr[1], &klen, sizeof klen); memcpy(&hdr[3], key, klen);
    send(sn, hdr, (uint16_t)(3 + klen));

    /* reply: int32 size, then `size` bytes of dpoint_to_binary */
    int32_t size = 0;
    if (bn_recv_exact(sn, (uint8_t *) &size, 4, 2000) || size <= 0) { bn_tmp_close(sn); return -1; }

    /* header: varlen(2)+varname(varlen)+ts(8)+type(4)+datalen(4). We requested
     * by name, so the reply varlen must equal klen; skip name+ts+type, read datalen. */
    uint16_t rvarlen = 0;
    if (bn_recv_exact(sn, (uint8_t *) &rvarlen, 2, 2000) || rvarlen != klen) { bn_tmp_close(sn); return -1; }
    if (bn_recv_exact(sn, hdr, (uint32_t) rvarlen + 12, 2000)) { bn_tmp_close(sn); return -1; }  /* name+ts+type */
    uint32_t datalen = 0;
    if (bn_recv_exact(sn, (uint8_t *) &datalen, 4, 2000)) { bn_tmp_close(sn); return -1; }
    if (datalen != (uint32_t) size - (2u + rvarlen + 8u + 4u + 4u)) { bn_tmp_close(sn); return -1; }

    /* stream the value to the sink */
    uint32_t left = datalen;
    uint8_t chunk[512];
    dl = make_timeout_time_ms(4000);
    while (left) {
        uint16_t avail; getsockopt(sn, SO_RECVBUF, &avail);
        if (avail) {
            uint16_t want = left < avail ? (uint16_t) left : avail;
            if (want > sizeof chunk) want = (uint16_t) sizeof chunk;
            int32_t r = recv(sn, chunk, want);
            if (r <= 0 || sink(ud, chunk, (uint32_t) r) < 0) { bn_tmp_close(sn); return -1; }
            left -= (uint32_t) r;
            dl = make_timeout_time_ms(4000);
        } else {
            getsockopt(sn, SO_STATUS, &st);
            if (st != SOCK_ESTABLISHED && st != SOCK_CLOSE_WAIT) { bn_tmp_close(sn); return -1; }
            if (absolute_time_diff_us(get_absolute_time(), dl) <= 0) { bn_tmp_close(sn); return -1; }
        }
    }
    bn_tmp_close(sn);
    return (int) datalen;
}

/* ---- async send_command: the same exchange as above, one us-bounded step per
 * poll, so registration never blocks the RT loop (the blocking version costs
 * ~2-10ms per command x4 on a healthy LAN, worst ~0.35s each on a sick one).
 * Same port rotation, same graceful FIN teardown, same truthful "<rc>" check. */
typedef enum { BN_CMD_IDLE = 0, BN_CMD_CONNECTING, BN_CMD_AWAIT_REPLY, BN_CMD_CLOSING } bn_cmd_state_t;
static bn_cmd_state_t   bn_cmd_st;
static int8_t           bn_cmd_ok;       /* result being carried to the CLOSING state */
static char             bn_cmd_buf[120];
static absolute_time_t  bn_cmd_dl;

static inline int box_net_send_command_start(const uint8_t dserv_ip[4], uint16_t port,
                                             const char *cmd)
{
    static uint8_t rr;
    const uint8_t sn = BOX_NET_TMP_SN;
    uint8_t d[4] = { dserv_ip[0], dserv_ip[1], dserv_ip[2], dserv_ip[3] };
    if (bn_cmd_st != BN_CMD_IDLE) return -1;
    close(sn);                                      /* prior command always ends CLOSED; instant */
    if (socket(sn, Sn_MR_TCP4 | SF_TCP_NODELAY,
               (uint16_t)(55000 + (rr++ & 7)), SOCK_IO_NONBLOCK) != sn) return -1;
    strncpy(bn_cmd_buf, cmd, sizeof bn_cmd_buf - 1);
    bn_cmd_buf[sizeof bn_cmd_buf - 1] = '\0';
    connect(sn, d, port, 4);                        /* non-blocking */
    bn_cmd_ok = -1;
    bn_cmd_st = BN_CMD_CONNECTING;
    bn_cmd_dl = make_timeout_time_ms(100);
    return 0;
}

static inline int box_net_send_command_poll(void)
{
    const uint8_t sn = BOX_NET_TMP_SN;
    uint8_t st;
    uint16_t sz;
    switch (bn_cmd_st) {
    case BN_CMD_IDLE:
        return 1;                                   /* nothing pending == done-ok */
    case BN_CMD_CONNECTING:
        getsockopt(sn, SO_STATUS, &st);
        if (st == SOCK_ESTABLISHED) {
            send(sn, (uint8_t *) bn_cmd_buf, (uint16_t) strlen(bn_cmd_buf));  /* fresh socket: single copy+SEND */
            bn_cmd_st = BN_CMD_AWAIT_REPLY;
            bn_cmd_dl = make_timeout_time_ms(200);
            return 0;
        }
        if (st == SOCK_CLOSED ||                    /* refused/reset */
            absolute_time_diff_us(get_absolute_time(), bn_cmd_dl) <= 0) {
            disconnect(sn);
            bn_cmd_st = BN_CMD_CLOSING;
            bn_cmd_dl = make_timeout_time_ms(50);
        }
        return 0;
    case BN_CMD_AWAIT_REPLY:
        getsockopt(sn, SO_RECVBUF, &sz);
        if (sz) {
            uint8_t rep[16] = {0};
            recv(sn, rep, sz > (uint16_t) sizeof rep ? (uint16_t) sizeof rep : sz);
            bn_cmd_ok = (rep[0] == '1') ? 1 : -1;
            disconnect(sn);
            bn_cmd_st = BN_CMD_CLOSING;
            bn_cmd_dl = make_timeout_time_ms(50);
            return 0;
        }
        getsockopt(sn, SO_STATUS, &st);
        if (st != SOCK_ESTABLISHED ||
            absolute_time_diff_us(get_absolute_time(), bn_cmd_dl) <= 0) {
            disconnect(sn);
            bn_cmd_st = BN_CMD_CLOSING;
            bn_cmd_dl = make_timeout_time_ms(50);
        }
        return 0;
    case BN_CMD_CLOSING:                            /* graceful FIN: peer kernel forgets the tuple */
        getsockopt(sn, SO_STATUS, &st);
        if (st == SOCK_CLOSED ||
            absolute_time_diff_us(get_absolute_time(), bn_cmd_dl) <= 0) {
            close(sn);
            bn_cmd_st = BN_CMD_IDLE;
            return bn_cmd_ok;
        }
        return 0;
    }
    return -1;
}

#endif /* BOX_NET_W6300_H */
