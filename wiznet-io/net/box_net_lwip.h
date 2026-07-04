/*
 * box_net_lwip.h -- Pico 2 W (CYW43 + lwIP) backend for box_net.h.
 * Included only when BOX_NET_LWIP is defined.
 *
 * STATUS: real implementation, NOT yet hardware-validated (no Pico 2 W / lwIP
 * in the build env used so far). The logic layers above box_net are unchanged
 * and fully tested; this is the piece to bring up on the board. Expect to tune
 * the lwIP raw-API callbacks and cyw43 init against real hardware.
 *
 * Model: WiFi STA join then IP per cfg->net_mode (DHCP default, or static from
 * cfg->net_ip) -- same net_mode semantics as the W6300 backend. One lwIP raw-API
 * TCP listener; accept/recv callbacks copy bytes into a ring buffer that
 * box_net_server_poll() drains -- so the byte-stream contract matches the W6300
 * backend and the framer above doesn't change. (The CYW43 has a factory-unique
 * MAC, so the board-id MAC derivation the W6300 backend needs isn't required.)
 * lwIP's DHCP client renews in the background; on a lease IP change the client
 * pcb errors -> bn_cli_err_cb -> reconnect -> self-register with the new IP.
 *
 * Needs: pico_cyw43_arch_lwip_poll (link), lwipopts.h + wifi_config.h on the
 * include path, PICO_BOARD=pico2_w.  Poll mode: box_net_poll -> cyw43_arch_poll.
 */
#ifndef BOX_NET_LWIP_H
#define BOX_NET_LWIP_H

#include "dserv_config.h"
#include "wifi_config.h"

#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include <string.h>

#define BN_RING_SZ 4096

static struct tcp_pcb *bn_listen_pcb;
static struct tcp_pcb *bn_conn_pcb;
static uint8_t  bn_ring[BN_RING_SZ];
static volatile uint32_t bn_head, bn_tail;   /* SPSC: callback writes, poll reads */
static volatile int bn_new_conn;

static inline const char *box_net_backend_name(void) { return "pico2w"; }

static inline void bn_ring_push(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t nh = (bn_head + 1) % BN_RING_SZ;
        if (nh == bn_tail) break;            /* full: drop (shouldn't happen at config rates) */
        bn_ring[bn_head] = p[i];
        bn_head = nh;
    }
}

static err_t bn_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void) arg; (void) err;
    if (!p) {                                 /* remote closed */
        tcp_close(pcb);
        if (pcb == bn_conn_pcb) bn_conn_pcb = NULL;
        return ERR_OK;
    }
    for (struct pbuf *q = p; q; q = q->next)
        bn_ring_push((const uint8_t *) q->payload, q->len);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t bn_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void) arg; (void) err;
    bn_conn_pcb = newpcb;
    bn_new_conn = 1;                          /* -> box_net_server_poll returns RESET */
    bn_head = bn_tail = 0;
    ip_set_option(newpcb, SOF_KEEPALIVE);     /* detect a dead dserv, recycle socket */
    tcp_recv(newpcb, bn_recv_cb);
    return ERR_OK;
}

/* client (box->dserv) + WiFi-watchdog state, up here so box_net_poll can reset the
 * client when the WiFi link recovers. */
static const pico_config_t *bn_cfg;        /* stashed for reconnect creds        */
static struct tcp_pcb *bn_cli_pcb;
static volatile int    bn_cli_connected;
static volatile int    bn_cli_reg;
static uint8_t         bn_wifi_was_down;
static uint32_t        bn_wifi_next_try_ms;

static inline int box_net_init(const pico_config_t *cfg)
{
    bn_cfg = cfg;                                          /* for WiFi reconnect */
    if (cyw43_arch_init() != 0) return -1;
    cyw43_arch_enable_sta_mode();
    /* power-save trades latency for battery life. Off (default) = lowest latency;
     * on = radio sleeps between beacons -- fine for a battery box since DI edge
     * timestamps are box-captured, so only delivery (not RT accuracy) is affected. */
    cyw43_wifi_pm(&cyw43_state, cfg->wifi_pm ? CYW43_DEFAULT_PM : CYW43_NO_POWERSAVE_MODE);

    /* runtime creds (USB CLI / datapoint) win; compile-time WIFI_SSID is fallback */
    const char *ssid = cfg->wifi_ssid[0] ? cfg->wifi_ssid : WIFI_SSID;
    const char *pass = cfg->wifi_pass[0] ? cfg->wifi_pass : WIFI_PASSWORD;
    /* the first join right after cyw43 init is often flaky, so retry a few times
     * (short per-attempt) rather than fail hard; the poll() watchdog covers the rest. */
    int jr = -1;
    for (int i = 0; i < 3 && jr != 0; i++) {
        if (i) printf("wifi: join retry %d...\n", i + 1);
        jr = cyw43_arch_wifi_connect_timeout_ms(ssid, pass, CYW43_AUTH_WPA2_AES_PSK, 8000);
    }
    if (jr != 0) return -2;

    /* IP mode (same net_mode semantics as W6300): DHCP is the default and is
     * already running from the connect above; static stops it and pins the addr
     * (cfg->net_ip, else the compiled default for parity with the wired box). */
    if (cfg->net_mode == NET_MODE_STATIC) {
        ip4_addr_t ip, mask, gw;
        if (cfg->net_ip[0] || cfg->net_ip[1] || cfg->net_ip[2] || cfg->net_ip[3]) {
            IP4_ADDR(&ip, cfg->net_ip[0], cfg->net_ip[1], cfg->net_ip[2], cfg->net_ip[3]);
            IP4_ADDR(&gw, cfg->net_ip[0], cfg->net_ip[1], cfg->net_ip[2], 1);
        } else {
            IP4_ADDR(&ip, 192, 168, 11, 2);
            IP4_ADDR(&gw, 192, 168, 11, 1);
        }
        IP4_ADDR(&mask, 255, 255, 255, 0);
        dhcp_stop(netif_default);
        netif_set_addr(netif_default, &ip, &mask, &gw);
    }
    return 0;
}

/* Service the stack + a WiFi watchdog: rejoin if the link drops, and on recovery
 * drop the stale client pcb so it reconnects and re-registers immediately (instead
 * of waiting out lwIP's TCP retransmit timeout). Parity with the W6300 self-heal. */
static inline void box_net_poll(void)
{
    cyw43_arch_poll();
    if (!bn_cfg) return;

    if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP) {
        if (bn_wifi_was_down) {                            /* link just came back */
            bn_wifi_was_down = 0;
            if (bn_cli_pcb) { tcp_abort(bn_cli_pcb); bn_cli_pcb = NULL; }
            bn_cli_connected = 0; bn_cli_reg = 0;          /* -> reconnect + self_register */
            printf("wifi: link back up\n");
        }
        return;
    }

    /* link down: throttled, bounded, blocking rejoin. Nothing flows over dead WiFi
     * anyway; DI edges keep their IRQ timestamps and publish once we're back. */
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (bn_wifi_was_down && (int32_t)(now - bn_wifi_next_try_ms) < 0) return;
    bn_wifi_was_down = 1;
    bn_wifi_next_try_ms = now + 10000;                     /* >= 10s between attempts */
    const char *ssid = bn_cfg->wifi_ssid[0] ? bn_cfg->wifi_ssid : WIFI_SSID;
    const char *pass = bn_cfg->wifi_pass[0] ? bn_cfg->wifi_pass : WIFI_PASSWORD;
    printf("wifi: link down, reconnecting to %s...\n", ssid);
    cyw43_arch_wifi_connect_timeout_ms(ssid, pass, CYW43_AUTH_WPA2_AES_PSK, 8000);
}

static inline int box_net_server_poll(uint16_t port, uint8_t *buf, int max)
{
    if (!bn_listen_pcb) {                      /* open the listener once */
        struct tcp_pcb *pcb = tcp_new();
        if (pcb && tcp_bind(pcb, IP_ANY_TYPE, port) == ERR_OK) {
            bn_listen_pcb = tcp_listen(pcb);
            tcp_accept(bn_listen_pcb, bn_accept_cb);
        }
        return 0;
    }
    if (bn_new_conn) { bn_new_conn = 0; return BOX_NET_RESET; }

    int n = 0;
    while (n < max && bn_tail != bn_head) {
        buf[n++] = bn_ring[bn_tail];
        bn_tail = (bn_tail + 1) % BN_RING_SZ;
    }
    return n;
}

/* The box's own IP (advertised to dserv in %reg). */
static inline void box_net_local_ip(uint8_t out[4])
{
    const ip4_addr_t *ip = netif_ip4_addr(netif_default);
    out[0] = ip4_addr1(ip); out[1] = ip4_addr2(ip);
    out[2] = ip4_addr3(ip); out[3] = ip4_addr4(ip);
}

/* Send one text command to dserv on a transient connection (best-effort;
 * needs validation on a real Pico 2 W). One command per connection. */
static volatile int bn_reg_connected;
static err_t bn_reg_conn_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{ (void) arg; (void) pcb; if (err == ERR_OK) bn_reg_connected = 1; return ERR_OK; }

static inline int box_net_send_command(const uint8_t dserv_ip[4], uint16_t port,
                                       const char *cmd)
{
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) return -1;
    bn_reg_connected = 0;
    ip4_addr_t d; IP4_ADDR(&d, dserv_ip[0], dserv_ip[1], dserv_ip[2], dserv_ip[3]);
    if (tcp_connect(pcb, &d, port, bn_reg_conn_cb) != ERR_OK) { tcp_abort(pcb); return -1; }
    for (int i = 0; i < 2000 && !bn_reg_connected; i++) cyw43_arch_poll();
    if (!bn_reg_connected) { tcp_abort(pcb); return -1; }
    tcp_write(pcb, cmd, strlen(cmd), TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    for (int i = 0; i < 500; i++) cyw43_arch_poll();   /* let it flush + reply */
    tcp_close(pcb);
    return 0;
}

/* ---- client: box -> dserv (publish state/(keys)) ---- (state declared above) */
static err_t bn_cli_conn_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{ (void) arg; (void) pcb; if (err == ERR_OK) bn_cli_connected = 1; return ERR_OK; }

static void bn_cli_err_cb(void *arg, err_t err)
{ (void) arg; (void) err; bn_cli_pcb = NULL; bn_cli_connected = 0; bn_cli_reg = 0; }

/* dserv doesn't send us data on this socket, but a NULL pbuf = it closed the
 * connection (a graceful dserv restart) -> tear down so we reconnect + re-register. */
static err_t bn_cli_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void) arg; (void) err;
    if (!p) {
        tcp_close(pcb);
        if (pcb == bn_cli_pcb) { bn_cli_pcb = NULL; bn_cli_connected = 0; bn_cli_reg = 0; }
        return ERR_OK;
    }
    tcp_recved(pcb, p->tot_len); pbuf_free(p);
    return ERR_OK;
}

static inline int box_net_client_service(const uint8_t ip[4], uint16_t port)
{
    if (bn_cli_connected) {
        if (!bn_cli_reg) { bn_cli_reg = 1; return 2; }   /* just (re)connected */
        return 1;
    }
    if (!bn_cli_pcb) {
        bn_cli_pcb = tcp_new();
        if (!bn_cli_pcb) return 0;
        tcp_err(bn_cli_pcb, bn_cli_err_cb);
        tcp_recv(bn_cli_pcb, bn_cli_recv_cb);            /* detect a graceful dserv close */
        ip_set_option(bn_cli_pcb, SOF_KEEPALIVE);        /* detect a silently-dead dserv */
        ip4_addr_t d; IP4_ADDR(&d, ip[0], ip[1], ip[2], ip[3]);
        if (tcp_connect(bn_cli_pcb, &d, port, bn_cli_conn_cb) != ERR_OK)
            bn_cli_pcb = NULL;
    }
    return 0;
}

static inline int box_net_client_send(const uint8_t *buf, int len)
{
    if (!bn_cli_connected || !bn_cli_pcb) return -1;
    if (tcp_write(bn_cli_pcb, buf, len, TCP_WRITE_FLAG_COPY) != ERR_OK) return -1;
    tcp_output(bn_cli_pcb);
    return 0;
}

#endif /* BOX_NET_LWIP_H */
