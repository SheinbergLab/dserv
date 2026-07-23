/*
 * box_net_eth.c -- RW612/Zephyr Ethernet transport over BSD-style sockets.
 */
#include "box_net_eth.h"

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/socket.h>
#include <string.h>

static struct net_if *iface;
static int sock = -1;

int box_net_eth_init(void)
{
	iface = net_if_get_default();
	if (!iface) {
		return -1;
	}
	net_dhcpv4_start(iface);
	return 0;
}

int box_net_eth_link(void)
{
	return (iface && net_if_is_up(iface)) ? 1 : 0;
}

int box_net_eth_connected(void)
{
	return sock >= 0 ? 1 : 0;
}

int box_net_eth_wait_ip(uint8_t out[4], int timeout_ms)
{
	int waited = 0;
	while (waited <= timeout_ms) {
		struct in_addr *a = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
		if (a) {
			out[0] = a->s4_addr[0];
			out[1] = a->s4_addr[1];
			out[2] = a->s4_addr[2];
			out[3] = a->s4_addr[3];
			return 1;
		}
		k_msleep(100);
		waited += 100;
	}
	return 0;
}

int box_net_eth_connect(const uint8_t dserv_ip[4], uint16_t port)
{
	struct sockaddr_in addr = { 0 };
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	memcpy(&addr.sin_addr, dserv_ip, 4);

	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		return -1;
	}
	if (zsock_connect(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
		zsock_close(sock);
		sock = -1;
		return -1;
	}
	return 0;
}

int box_net_eth_poll(uint8_t *buf, int max)
{
	if (sock < 0 || max <= 0) {
		return 0;
	}
	int n = zsock_recv(sock, buf, (size_t) max, ZSOCK_MSG_DONTWAIT);
	if (n > 0) {
		return n;
	}
	if (n == 0) {                        /* peer closed */
		zsock_close(sock);
		sock = -1;
		return BOX_NET_RESET;
	}
	return 0;                            /* EAGAIN / EWOULDBLOCK */
}

int box_net_eth_send(const uint8_t *buf, int len)
{
	if (sock < 0) {
		return -1;
	}
	int n = zsock_send(sock, buf, (size_t) len, 0);
	return (n == len) ? 0 : -1;
}
