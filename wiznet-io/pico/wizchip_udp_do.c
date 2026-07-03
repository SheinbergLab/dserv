/*
 * wizchip_udp_do.c  --  UDP command -> digital-out, with self-timestamped reply
 *                       W6300-EVB-Pico2, first I/O-extension bring-up for dserv.
 *
 * Fast path (UDP, socket SOCKET_DO):
 *   - Host sends cmd_req_t (little-endian; host and RP2350 are both LE).
 *   - Pico stamps t_rx (its own us clock) as early as possible, drives DO_PIN,
 *     stamps t_tx just before replying, and echoes both in cmd_rep_t.
 *   - Host RTT - (t_tx - t_rx) = network + both host stacks; (t_tx - t_rx) is
 *     the device turnaround, free of host jitter. Calibrate against a scope on
 *     DO_PIN once (Digilent), then trust the on-box number.
 *
 * BOARD: W6300-EVB-Pico2. GPIO15..22 wired to the W6300 (INT=15, CS=16,
 * SCK=17, IO0..3=18..21, RST=22) -- keep DO_PIN out of that range.
 *
 * API matches WIZnet ioLibrary_Driver W6x00 variant (verified against the
 * bundled loopback.c): getsockopt(SO_STATUS/SO_RECVBUF), 6-arg recvfrom/sendto
 * (addrlen is a *pointer* for recvfrom, by value for sendto), Sn_MR_UDP4.
 */

#include <stdio.h>
#include <string.h>

#include "port_common.h"
#include "wizchip_conf.h"
#include "wizchip_spi.h"
#include "socket.h"

#include "pico/time.h"       /* time_us_32() */
#include "hardware/gpio.h"

/* ----- socket / pin map (see README "8-socket map") ---------------------- */
#define SOCKET_DO   0          /* UDP fast path: DO commands + timed reply   */
#define PORT_DO     5000
#define DO_PIN      6          /* free header GPIO for scope + actuation     */
#define BUF_SIZE    2048

/* ----- wire protocol (little-endian, packed) ----------------------------- */
typedef struct __attribute__((packed)) {
    uint16_t seq;              /* host sequence #, echoed back               */
    uint8_t  cmd;              /* 0 = drive DO low, non-0 = drive DO high    */
    uint8_t  pin;              /* reserved (which DO); 0 => DO_PIN for now    */
} cmd_req_t;

typedef struct __attribute__((packed)) {
    uint16_t seq;
    uint8_t  cmd;
    uint8_t  pin;
    uint32_t t_rx_us;          /* box clock, right after recvfrom            */
    uint32_t t_tx_us;          /* box clock, right before reply              */
} cmd_rep_t;

static uint8_t g_ethernet_buf[BUF_SIZE];

/* W6300 is _WIZCHIP_ > W5500, so NetInfo carries IPv6 fields + ipmode.
 * We run IPv4-only here (NETINFO_STATIC_ALL, v6 fields unused/zero-ish). */
static wiz_NetInfo g_net_info = {
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

/* Poll-based on purpose for the first numbers: on a dedicated core this is as
 * low-latency and MORE deterministic than an ISR. Move to the W6300 INT line
 * (GPIO15) later if you want the core free -- see README "Step 2". */
static void udp_do_service(uint8_t sn, uint8_t *buf, uint16_t port)
{
    uint8_t  status;
    uint8_t  destip[16] = {0};
    uint16_t destport;
    uint16_t size;
    uint8_t  addr_len;
    int32_t  ret;

    getsockopt(sn, SO_STATUS, &status);

    switch (status) {
    case SOCK_UDP:
        getsockopt(sn, SO_RECVBUF, &size);
        if (size == 0) return;
        if (size > BUF_SIZE) size = BUF_SIZE;

        ret = recvfrom(sn, buf, size, destip, &destport, &addr_len);
        uint32_t t_rx = time_us_32();          /* stamp ASAP after receive   */
        if (ret <= 0) return;

        cmd_req_t req = {0};
        memcpy(&req, buf, (ret < (int32_t)sizeof req) ? (size_t)ret : sizeof req);

        /* ---- the actuation we are timing ---- */
        gpio_put(DO_PIN, req.cmd ? 1 : 0);
        /* -------------------------------------- */

        cmd_rep_t rep = {
            .seq     = req.seq,
            .cmd     = req.cmd,
            .pin     = req.pin,
            .t_rx_us = t_rx,
        };
        rep.t_tx_us = time_us_32();            /* stamp just before send     */
        sendto(sn, (uint8_t *)&rep, sizeof rep, destip, destport, addr_len);
        break;

    case SOCK_CLOSED:
        if ((ret = socket(sn, Sn_MR_UDP4, port, SOCK_IO_NONBLOCK)) != sn)
            return;
        printf("UDP socket %d open on port %u\n", sn, port);
        break;

    default:
        break;
    }
}

int main(void)
{
    stdio_init_all();
    sleep_ms(3000);            /* let USB-CDC / RTT attach                    */

    gpio_init(DO_PIN);
    gpio_set_dir(DO_PIN, GPIO_OUT);
    gpio_put(DO_PIN, 0);

    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    wizchip_check();

    network_initialize(g_net_info);
    print_network_information(g_net_info);

    while (1) {
        udp_do_service(SOCKET_DO, g_ethernet_buf, PORT_DO);
    }
}
