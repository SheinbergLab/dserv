/*
 * box_net.h -- transport seam for the box. Selects a network backend at BUILD
 * time so the same firmware targets either the wired W6300 or the Pico 2 W WiFi:
 *
 *     default            -> box_net_w6300.h  (WIZnet W6300, hardwired TCP/IP)
 *     -DBOX_NET_LWIP     -> box_net_lwip.h   (Pico 2 W, CYW43 + lwIP software stack)
 *
 * Everything above this seam (dserv_msg / dserv_config / pico_persist / pico_cli
 * / pico_gpio) is identical across both -- the same code the POSIX simulator
 * runs. Only these calls differ per transport:
 *
 *   int  box_net_init(const pico_config_t *cfg)
 *        Bring up the link + IP (static from cfg->net_ip if set). 0 ok, <0 fail.
 *   void box_net_poll(void)
 *        Service the stack once per superloop. No-op on W6300 (chip offloads
 *        TCP/IP); drives cyw43/lwIP on Pico 2 W.
 *   int  box_net_server_poll(uint16_t port, uint8_t *buf, int max)
 *        Non-blocking TCP config server. Returns:
 *          >0  bytes received (feed to the framer)
 *           0  nothing this tick
 *          <0  connection (re)opened -> reset your framer
 *   const char *box_net_backend_name(void)   "w6300" | "pico2w"
 *
 *   int  box_net_client_service(const uint8_t dserv_ip[4], uint16_t port)
 *        Maintain a TCP client connection to dserv (for publishing state/(keys)).
 *        Call each loop; returns 2 the tick it (re)connects (-> self-register),
 *        1 when connected/ready, 0 otherwise.
 *   int  box_net_client_send(const uint8_t *buf, int len)
 *        Send a datapoint frame to dserv. 0 ok, <0 not connected/error.
 *   void box_net_local_ip(uint8_t out[4])
 *        The box's own IP (to advertise in %reg).
 *   int  box_net_send_command(const uint8_t dserv_ip[4], uint16_t port, const char *cmd)
 *        Send one text command (e.g. "%reg ...\n") to dserv on a transient
 *        connection; one command per connection (dserv reads '%' greedily).
 */
#ifndef BOX_NET_H
#define BOX_NET_H

#include "dserv_config.h"
#include <stdint.h>

#define BOX_NET_RESET (-1)   /* box_net_server_poll: reset framer on new conn */

#if defined(BOX_NET_LWIP)
#include "box_net_lwip.h"
#else
#include "box_net_w6300.h"
#endif

#endif /* BOX_NET_H */
