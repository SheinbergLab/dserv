/*
 * box_net_eth.h -- Ethernet transport for the extio box (RW612/Zephyr).
 *
 * The native-networking counterpart to box_net_usb: bring up the ENET interface
 * (DHCPv4), open a TCP socket to dserv, and move the same 128-byte dserv frames.
 * Unlike the Pico's W6300 (hardwired TCP offload over SPI), this is Zephyr's own
 * IP stack over a real MAC -- which is also what gives us hardware PTP (box_ptp.h).
 */
#ifndef BOX_NET_ETH_H
#define BOX_NET_ETH_H

#include <stdint.h>

#ifndef BOX_NET_RESET
#define BOX_NET_RESET (-1)
#endif

/* Grab the default iface and start DHCPv4. 0 on success. */
int box_net_eth_init(void);

/* Wait up to timeout_ms for a DHCP IPv4 lease; fills out[4] and returns 1, else 0. */
int box_net_eth_wait_ip(uint8_t out[4], int timeout_ms);

/* 1 iff the interface is up (carrier + admin up) -- the AUTO carrier signal. */
int box_net_eth_link(void);

/* 1 iff a dserv TCP session is currently open. */
int box_net_eth_connected(void);

/* Non-blocking current IPv4 address. Fills out[4] and returns 1 if a lease is
 * held, else 0. (box_net_eth_wait_ip blocks; this just samples.) */
int box_net_eth_get_ip(uint8_t out[4]);

/* Open a TCP connection to dserv (blocking connect, then non-blocking I/O). 0 ok. */
int box_net_eth_connect(const uint8_t dserv_ip[4], uint16_t port);

/* Non-blocking inbound: bytes read (0 if none), or BOX_NET_RESET if the peer closed. */
int box_net_eth_poll(uint8_t *buf, int max);

/* Send one frame; 0 if the whole frame went out, <0 otherwise. */
int box_net_eth_send(const uint8_t *buf, int len);

#endif /* BOX_NET_ETH_H */
