/*
 * box_uplink.c -- uplink arbiter + the eth/usb transport adapters.
 */
#include "box_uplink.h"
#include "box_net_usb.h"
#if defined(CONFIG_NETWORKING)
#include "box_net_eth.h"
#endif

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

/* ---- transport adapters (wrap the box_net_* backends into the vtable) ---- */

static int u_usb_init(const box_config_t *c)      { (void) c; return box_net_usb_init(); }
static int u_usb_available(void)                  { return 1; }        /* the always-there fallback */
static int u_usb_connect(const box_config_t *c)   { (void) c; return 0; } /* enumeration is implicit */
static int u_usb_connected(void)                  { return box_net_usb_reading(); } /* host draining (DTR) */
static int u_usb_poll(uint8_t *b, int m)          { return box_net_usb_server_poll(b, m); }
static int u_usb_send(const uint8_t *b, int l)    { return box_net_usb_client_send(b, l); }
static int u_usb_register(const box_config_t *c)  { (void) c; return 0; } /* host module owns forwarding (v1) */

static const box_uplink_if uplink_usb = {
	.name = "usb", .init = u_usb_init, .available = u_usb_available,
	.connect = u_usb_connect, .connected = u_usb_connected,
	.poll = u_usb_poll, .send = u_usb_send, .self_register = u_usb_register,
};

/* Ethernet is only present on boards with a MAC+PHY (frdm_rw612, teensy41).
 * A USB-only board (teensy40) compiles this out and the arbiter simply has one
 * candidate -- the policy code below is unchanged either way. */
#if defined(CONFIG_NETWORKING)
static int u_eth_init(const box_config_t *c)      { (void) c; return box_net_eth_init(); }
/* Eth counts as a usable uplink only with BOTH carrier AND a configured dserv
 * target -- otherwise it publishes into the void, so we stay on USB and the box
 * remains reachable/observable (also the sane bench default with no dserv). The
 * target is set over the console CLI or persisted config. cfg is captured at
 * init since the vtable's available() takes no args. */
static const box_config_t *eth_cfg;
static int u_eth_available(void)
{
	int has_target = eth_cfg &&
		(eth_cfg->dserv_ip[0] | eth_cfg->dserv_ip[1] |
		 eth_cfg->dserv_ip[2] | eth_cfg->dserv_ip[3]);
	return box_net_eth_link() && has_target;
}
static int u_eth_connect(const box_config_t *c)   { return box_net_eth_connect(c->dserv_ip, dserv_cfg_port(c)); }
static int u_eth_connected(void)                  { return box_net_eth_connected(); }
static int u_eth_poll(uint8_t *b, int m)          { return box_net_eth_poll(b, m); }
static int u_eth_send(const uint8_t *b, int l)    { return box_net_eth_send(b, l); }
static int u_eth_register(const box_config_t *c)  { (void) c; return 0; } /* %reg/%match handshake: TODO */

static const box_uplink_if uplink_eth = {
	.name = "eth", .init = u_eth_init, .available = u_eth_available,
	.connect = u_eth_connect, .connected = u_eth_connected,
	.poll = u_eth_poll, .send = u_eth_send, .self_register = u_eth_register,
};
#endif /* CONFIG_NETWORKING */

/* ---- arbiter state ---- */

static const box_uplink_if *active;
#define ETH_PROMOTE_PASSES 20            /* debounce: carrier must hold before we pick eth */
static int eth_streak;

/* Physical mode strap (authoritative, overrides the persisted mode). Wired by a
 * board via a "mode-strap" devicetree alias; absent by default -> no override, so
 * a persisted eth + no cable can't wedge boot (the extio GP28 lesson). Open/high
 * = honor the persisted/auto policy; pulled low = force Ethernet. */
#if DT_NODE_EXISTS(DT_ALIAS(mode_strap))
static const struct gpio_dt_spec mode_strap = GPIO_DT_SPEC_GET(DT_ALIAS(mode_strap), gpios);
static uint8_t strap_override(uint8_t persisted)
{
	if (!gpio_is_ready_dt(&mode_strap)) {
		return persisted;
	}
	gpio_pin_configure_dt(&mode_strap, GPIO_INPUT);
	return gpio_pin_get_dt(&mode_strap) ? XMODE_ETH : persisted;
}
#else
static uint8_t strap_override(uint8_t persisted) { return persisted; }
#endif

/* Which uplink the policy wants right now. */
static const box_uplink_if *desired(const box_config_t *cfg)
{
#if !defined(CONFIG_NETWORKING)
	ARG_UNUSED(cfg);
	return &uplink_usb;                     /* USB-only board: one candidate */
#else
	uint8_t mode = strap_override(cfg->transport_mode);

	if (mode == XMODE_ETH) {
		return &uplink_eth;
	}
	if (mode == XMODE_USB) {
		return &uplink_usb;
	}
	/* AUTO: Ethernet when carrier holds (debounced), else USB. */
	if (uplink_eth.available()) {
		if (eth_streak < ETH_PROMOTE_PASSES) {
			eth_streak++;
		}
	} else {
		eth_streak = 0;
	}
	return (eth_streak >= ETH_PROMOTE_PASSES) ? &uplink_eth : &uplink_usb;
#endif
}

int box_uplink_init(const box_config_t *cfg)
{
	int ok = (uplink_usb.init(cfg) == 0);
#if defined(CONFIG_NETWORKING)
	eth_cfg = cfg;
	ok |= (uplink_eth.init(cfg) == 0);
#endif
	active = NULL;
	eth_streak = 0;
	box_uplink_service(cfg);
	return ok ? 0 : -1;                     /* at least one transport up */
}

void box_uplink_service(const box_config_t *cfg)
{
	const box_uplink_if *want = desired(cfg);

	if (want != active) {
		active = want;
		active->connect(cfg);
		if (active->connected()) {
			active->self_register(cfg);
		}
		return;
	}
	/* same uplink: reconnect a dropped session (eth) and re-announce. */
	if (active && !active->connected()) {
		if (active->connect(cfg) == 0 && active->connected()) {
			active->self_register(cfg);
		}
	}
}

int box_uplink_poll(uint8_t *buf, int max)
{
	return active ? active->poll(buf, max) : 0;
}

int box_uplink_send(const uint8_t *buf, int len)
{
	return active ? active->send(buf, len) : -1;
}

const char *box_uplink_active_name(void)
{
	return active ? active->name : "none";
}
