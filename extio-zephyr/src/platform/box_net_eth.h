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

/* Non-blocking inbound: bytes read (0 if none), or BOX_NET_RESET when the config
 * link changes state (dserv just connected back, or it dropped).
 *
 * INBOUND ARRIVES ON THE CONFIG SERVER, NOT ON THE CLIENT SOCKET. dserv's
 * subscription model is connect-back: the box tells dserv "send my matches to
 * <my-ip>:<CFG_PORT>" and dserv opens a SECOND connection in that direction.
 * The client socket (box -> dserv) carries publishes and the %-command channel
 * only; nothing ever arrives on it. */
int box_net_eth_poll(uint8_t *buf, int max);

/* Send one frame; 0 if the whole frame went out, <0 otherwise. */
int box_net_eth_send(const uint8_t *buf, int len);

/* ---- config server: dserv's connect-back for subscribed datapoints ---- */

/* The port advertised in %reg. Same value as the Pico (CFG_PORT) so a rig's
 * firewall rules and habits carry over unchanged. */
#define BOX_ETH_CFG_PORT 5010

/* Create/bind/listen the config server and accept a pending connect-back.
 * Non-blocking; call once per service pass. */
void box_net_eth_server_service(void);

/* 1 iff dserv's connect-back is established. This is the box-visible proof that
 * %reg landed AND dserv could reach us -- dserv NEVER retries that connect, so a
 * caller that sees this stay 0 while the client socket is up must re-register. */
int box_net_eth_server_up(void);

/* Cost of the last zsock_send() and the running max, in us. Instrumentation for
 * the publish-latency investigation; see box_net_eth.c. */
void box_net_eth_send_stats(uint32_t *last_us, uint32_t *max_us);

/* Inbound split: RX-thread-signal -> loop-reached-recv, and the recv cost. */
void box_net_eth_rx_stats(uint32_t *wake_us, uint32_t *wake_max,
			  uint32_t *recv_us, uint32_t *recv_max);

/* Start the RX wake thread: signals box_event when the config link becomes
 * readable, so an inbound command does not wait out the service loop's 1 ms
 * poll timeout. Call once, after init. */
void box_net_eth_rx_wake_start(void);

/* Send ONE text command ("%reg ...\n" / "%match ...\n") to dserv and wait
 * (bounded) for its "<rc> ...\n" reply. 0 = dserv ACCEPTED (rc 1), <0 otherwise.
 *
 * Runs on the registration thread, never the service loop -- it blocks for up to
 * a few hundred ms and the loop is what gates DI publish latency. One command
 * per connection: dserv's '%' reader is greedy and will swallow a second line. */
int box_net_eth_send_command(const uint8_t dserv_ip[4], uint16_t port,
			     const char *cmd);

#endif /* BOX_NET_ETH_H */
