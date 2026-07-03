/*
 * box_net_lwip.h -- Pico 2 W (CYW43 + lwIP) backend for box_net.h.
 * Included only when BOX_NET_LWIP is defined.
 *
 * STATUS: real implementation, NOT yet hardware-validated (no Pico 2 W / lwIP
 * in the build env used so far). The logic layers above box_net are unchanged
 * and fully tested; this is the piece to bring up on the board. Expect to tune
 * the lwIP raw-API callbacks and cyw43 init against real hardware.
 *
 * Model: WiFi STA join (static IP from cfg->net_ip if set, else DHCP), one lwIP
 * raw-API TCP listener. accept/recv callbacks copy bytes into a ring buffer
 * that box_net_server_poll() drains -- so the byte-stream contract matches the
 * W6300 backend and the framer above doesn't change.
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

static inline int box_net_init(const pico_config_t *cfg)
{
    if (cyw43_arch_init() != 0) return -1;
    cyw43_arch_enable_sta_mode();
    cyw43_wifi_pm(&cyw43_state, CYW43_NO_POWERSAVE_MODE);   /* lower latency */

    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
            CYW43_AUTH_WPA2_AES_PSK, WIFI_CONNECT_TIMEOUT_MS) != 0)
        return -2;

    /* static IP if configured, else DHCP already ran during connect */
    if (cfg->net_ip[0] || cfg->net_ip[1] || cfg->net_ip[2] || cfg->net_ip[3]) {
        ip4_addr_t ip, mask, gw;
        IP4_ADDR(&ip,   cfg->net_ip[0], cfg->net_ip[1], cfg->net_ip[2], cfg->net_ip[3]);
        IP4_ADDR(&mask, 255, 255, 255, 0);
        IP4_ADDR(&gw,   cfg->net_ip[0], cfg->net_ip[1], cfg->net_ip[2], 1);
        netif_set_addr(netif_default, &ip, &mask, &gw);
    }
    return 0;
}

static inline void box_net_poll(void) { cyw43_arch_poll(); }

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

/* ---- client: box -> dserv (publish state/(keys)) ---- */
static struct tcp_pcb *bn_cli_pcb;
static volatile int    bn_cli_connected;
static volatile int    bn_cli_reg;

static err_t bn_cli_conn_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{ (void) arg; (void) pcb; if (err == ERR_OK) bn_cli_connected = 1; return ERR_OK; }

static void bn_cli_err_cb(void *arg, err_t err)
{ (void) arg; (void) err; bn_cli_pcb = NULL; bn_cli_connected = 0; bn_cli_reg = 0; }

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
