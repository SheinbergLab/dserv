/*
 * wizchip_dserv_wdt.c -- W6300-EVB-Pico2 TCP client that pushes a watchdog /
 * heartbeat datapoint into dserv, using the 128-byte binary message format
 * (same path the Teensy / eye-tracker use: src/Dataserver.cpp '>' handler).
 *
 * Every HEARTBEAT_MS it connects to dserv (localhost:4620 by default) and sends
 * `pico/watchdog` as an incrementing int. A dserv-side subscriber treats the
 * datapoint going stale (stops incrementing) as "box dead / link down" -- that
 * is the watchdog. Also sends `pico/uptime_us` so you can see the box clock.
 *
 * Uses socket 2 per the wiznet-io socket map (TCP telemetry channel).
 *
 * Requires dserv_msg.h next to this source (copy from wiznet-io/common/).
 * W6x00 TCP API verified against ioLibrary loopback_tcpc:
 *   socket(sn,Sn_MR_TCP4,port,SOCK_IO_NONBLOCK) -> connect(sn,ip,port,4) ->
 *   send(sn,buf,len); state via getsockopt(SO_STATUS).
 */

#include <stdio.h>
#include <string.h>

#include "port_common.h"
#include "wizchip_conf.h"
#include "wizchip_spi.h"
#include "socket.h"

#include "pico/time.h"
#include "dserv_msg.h"

#define SOCKET_WDT   2           /* TCP telemetry channel (socket map)        */
#define LOCAL_PORT   50000
#define DSERV_PORT   4620
#define HEARTBEAT_MS 1000

static uint8_t g_dserv_ip[4] = {192, 168, 11, 1};   /* host running dserv     */
static uint8_t g_frame[DSERV_MSG_LEN];

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

/* send exactly n bytes; returns 0 on success, <0 on socket error */
static int send_all(uint8_t sn, const uint8_t *buf, int n)
{
    int sent = 0;
    while (sent < n) {
        int32_t r = send(sn, (uint8_t *)(buf + sent), (uint16_t)(n - sent));
        if (r < 0) return (int)r;      /* real error (SOCKERR_BUSY is 0)       */
        sent += r;
    }
    return 0;
}

static void wdt_service(uint8_t sn)
{
    static uint32_t last_ms = 0;
    static int32_t  counter = 0;
    uint8_t status;

    getsockopt(sn, SO_STATUS, &status);

    switch (status) {
    case SOCK_ESTABLISHED: {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if ((now_ms - last_ms) < HEARTBEAT_MS) break;
        last_ms = now_ms;

        /* timestamp 0 => dserv stamps arrival. Send box uptime separately so
         * the host can also see the device clock. */
        dserv_msg_int(g_frame, "pico/watchdog", 0, counter);
        if (send_all(sn, g_frame, DSERV_MSG_LEN) < 0) { disconnect(sn); break; }

        dserv_msg_int64(g_frame, "pico/uptime_us", 0, (int64_t)time_us_64());
        if (send_all(sn, g_frame, DSERV_MSG_LEN) < 0) { disconnect(sn); break; }

        printf("watchdog %ld\n", (long)counter);
        counter++;
        break;
    }
    case SOCK_CLOSE_WAIT:
        disconnect(sn);
        break;
    case SOCK_INIT:
        connect(sn, g_dserv_ip, DSERV_PORT, 4);   /* 4 = IPv4 addrlen          */
        break;
    case SOCK_CLOSED:
        socket(sn, Sn_MR_TCP4, LOCAL_PORT, SOCK_IO_NONBLOCK);
        break;
    default:
        break;
    }
}

int main(void)
{
    stdio_init_all();
    sleep_ms(3000);

    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    wizchip_check();

    network_initialize(g_net_info);
    print_network_information(g_net_info);

    printf("dserv watchdog client -> %d.%d.%d.%d:%d\n",
           g_dserv_ip[0], g_dserv_ip[1], g_dserv_ip[2], g_dserv_ip[3], DSERV_PORT);

    while (1) {
        wdt_service(SOCKET_WDT);
    }
}
