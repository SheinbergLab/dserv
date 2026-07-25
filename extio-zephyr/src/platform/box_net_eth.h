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

#include "dserv_config.h"     /* box_config_t: net_mode/net_ip/net_gw/net_sn */

#ifndef BOX_NET_RESET
#define BOX_NET_RESET (-1)
#endif

/* Grab the default iface and bring up IPv4 addressing per the persisted
 * config: net.mode=static applies net.ip/gw/mask (zero mask => /24, zero
 * gateway => none -- fine on a direct box<->host link, which is exactly the
 * case static exists for: no DHCP server on a point-to-point cable). Anything
 * else starts DHCPv4, the default. Parity with the Pico/W6300 box, which has
 * honored these fields all along. 0 on success. */
int box_net_eth_init(const box_config_t *cfg);

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

/* ---- in-stack residence time (CONFIG_NET_PKT_RXTIME/TXTIME_STATS) ----
 *
 * The stack's OWN packet measurement -- the segment wake/recv/disp cannot see:
 *   RX: net_pkt alloc in the ENET RX thread (just after the ISR) -> the moment
 *       zsock_recv hands the payload to us.
 *   TX: net_pkt alloc inside zsock_send -> driver TX complete. With
 *       NET_TC_TX_COUNT=1 this INCLUDES the TX-thread queue wait, which
 *       dbg/send_us (wall time of the enqueue) structurally cannot.
 * detail[] holds per-stage means when the _DETAIL configs are on (RX stages:
 * TC-fifo enqueue / stack entry / socket-fifo put / final recv). */
typedef struct {
	uint32_t rx_avg_us, rx_count;      /* mean + frames, since previous call */
	uint32_t tx_avg_us, tx_count;
	int rx_detail_n, tx_detail_n;
	uint32_t rx_detail_us[4];
	uint32_t tx_detail_us[4];
} box_eth_stack_stats_t;

/* Fill with the delta since the previous call (first call = since boot).
 * 0 on success; -1 when the stats API is not in this build or no iface. */
int box_net_eth_stack_stats(box_eth_stack_stats_t *out);

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
