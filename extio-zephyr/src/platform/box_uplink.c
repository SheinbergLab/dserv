/*
 * box_uplink.c -- uplink arbiter + the eth/usb transport adapters.
 */
#include "box_uplink.h"
#include "box_net_usb.h"
#if defined(CONFIG_NETWORKING)
#include "box_net_eth.h"
#include "box_obs.h"                     /* box_obs_active(): gate the match refresh */
#endif

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

/* ---- transport adapters (wrap the box_net_* backends into the vtable) ---- */

#if defined(CONFIG_NETWORKING)
static uint8_t strap_override(uint8_t persisted);   /* defined with the arbiter below */
#endif

/* A box that has DECLARED Ethernet (persisted `mode eth`, or the strap pulled)
 * enumerates the console CDC only -- no binary data pipe. Otherwise a host sees
 * a pipe the box will never read: dserv's extio subprocess claims the port,
 * lists the box in extio/boxes as though it were USB-attached, and hot-swap-polls
 * it forever, while the real traffic is on the network.
 *
 * AUTO DELIBERATELY KEEPS THE PIPE. In auto, USB is the FALLBACK -- desired()
 * drops back to it whenever carrier is absent -- so suppressing the pipe there
 * would leave a box with a dead cable reachable only by console, which is the
 * one thing auto exists to prevent. Only XMODE_ETH is safe to strip, and it is
 * safe precisely because desired() returns eth unconditionally in that mode:
 * there is no USB fallback to lose.
 *
 * The gate is persisted config + a GPIO strap, both readable at boot. It is NOT
 * PHY carrier sense: enumeration happens long before autonegotiation settles,
 * and boot-time PHY probing is what wedged the Pico (dropped 2026-07-05,
 * PHYSR unreliable at cold boot). */
static int u_usb_init(const box_config_t *c)
{
	int with_data = 1;
#if defined(CONFIG_NETWORKING)
	with_data = (strap_override(c->transport_mode) != XMODE_ETH);
#else
	ARG_UNUSED(c);                          /* USB-only board: always the data pipe */
#endif
	return box_net_usb_init(with_data);
}
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
static int u_eth_init(const box_config_t *c)      { return box_net_eth_init(c); }
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
/* ---- %reg / %match self-registration (Ethernet only) ----
 *
 * dserv forwards a datapoint to a subscriber only if some client has told it to,
 * via "%reg <ip> <port>" (open a connect-back to me) plus one "%match <ip> <port>
 * <pattern> 1" per subscription. Over USB the host module wires this itself, so
 * u_usb_register stays a no-op; over Ethernet the BOX has to ask, and until it
 * does the box is WRITE-ONLY -- publishes flow, but no config, no cmd/*, and no
 * ess/in_obs, which also means the clock can never obs-sync.
 *
 * Run on a dedicated thread, NOT the service loop: each command blocks up to
 * ~600 ms waiting for dserv's reply, and the loop is what gates DI publish
 * latency. This is the idiomatic-Zephyr version of the Pico's async
 * send_command state machine -- an RTOS thread does the same job with none of
 * the interleaving. The loop only ever sets a flag and moves on. */
#define REG_RETRY_MS     5000            /* config link down -> retry cadence   */
/* ...except for the FIRST registration after boot, which is guaranteed to fail.
 *
 * box_uplink_init() fires a registration as part of bringing the transport up,
 * and on a cold boot that necessarily runs BEFORE DHCP has produced an address
 * -- so the box is not registered, the watchdog notices, and then charges the
 * full 5 s cadence before trying again even though the box has been ready the
 * whole time. Measured on box3: a consistent 5.17 s between the service loop
 * starting and `reg: registered`, in every boot, on a rig where the actual
 * register round takes 170 ms.
 *
 * So retry FAST until the first success, then settle into the 5 s cadence. The
 * slow cadence exists for a dserv that is down or restarting, where hammering
 * buys nothing; a box that has never once been up is a different situation, and
 * it is also the one a person is standing there watching. */
#define REG_FIRST_MS     250             /* ...until the first successful one    */
#define MATCH_REFRESH_MS 30000           /* re-assert matches (a lost one is silent) */

#define SYNC_DP "ess/in_obs"             /* obs edge -> box clock anchor */

static K_SEM_DEFINE(reg_wake, 0, 1);
static K_MUTEX_DEFINE(reg_lock);
static struct {
	uint8_t  dserv_ip[4];
	uint16_t port;
	char     prefix[64];                 /* "extio/<name>" */
	uint8_t  full;                       /* include %reg, not just the matches */
	uint8_t  pending;
} reg_req;

/* Snapshot what the thread needs; never let it read the live config. */
static void reg_request(const box_config_t *c, int full)
{
	uint8_t ip[4];
	if (!box_net_eth_get_ip(ip)) {
		return;                          /* no lease yet: nothing to advertise */
	}
	k_mutex_lock(&reg_lock, K_FOREVER);
	memcpy(reg_req.dserv_ip, c->dserv_ip, 4);
	reg_req.port = dserv_cfg_port(c);
	dserv_cfg_prefix(c, reg_req.prefix, sizeof reg_req.prefix);
	reg_req.full |= (full != 0);
	reg_req.pending = 1;
	k_mutex_unlock(&reg_lock);
	k_sem_give(&reg_wake);
}

/* Send one %reg/%match line, RETRYING.
 *
 * A registration round opens a separate short-lived TCP connection per line,
 * back to back. Measured on an RW612 against dserv 2026-07-26: only the FIRST
 * %match of a round lands -- `%getmatch <box> 5010` showed dserv holding
 * `extio/box/config/*` alone, with `extio/box/cmd/*` and `ess/in_obs` missing.
 *
 * That failure is nearly invisible and genuinely dangerous: the box keeps
 * PUBLISHING normally (state/watchdog advances, uplink datapoints flow) while
 * being completely DEAF to cmd/*. It reads as a healthy box right up until you
 * try to command it. The old code just counted these as `fails` and left the
 * watchdog to retry the whole round -- but every round fails the same way, so
 * it never converged.
 *
 * A short gap plus a retry fixes it. Verified by hand: injecting the two missing
 * %match lines with a 300 ms spacing restored the downlink immediately.
 */
static int reg_send_retry(const uint8_t dip[4], uint16_t port, const char *cmd)
{
	for (int attempt = 0; attempt < 4; attempt++) {
		if (box_net_eth_send_command(dip, port, cmd) == 0) {
			return 0;
		}
		k_msleep(120);
	}
	return -1;
}

static void reg_thread_fn(void *a, void *b, void *cc)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(cc);
	for (;;) {
		k_sem_take(&reg_wake, K_FOREVER);

		uint8_t dip[4], bip[4];
		uint16_t port;
		char pfx[64];
		int full;

		k_mutex_lock(&reg_lock, K_FOREVER);
		if (!reg_req.pending) { k_mutex_unlock(&reg_lock); continue; }
		memcpy(dip, reg_req.dserv_ip, 4);
		port = reg_req.port;
		memcpy(pfx, reg_req.prefix, sizeof pfx);
		full = reg_req.full;
		reg_req.pending = 0;
		reg_req.full = 0;
		k_mutex_unlock(&reg_lock);

		/* IP is sampled HERE, at the start of the sequence, so every command in
		 * one run advertises the same address even if DHCP renews mid-sequence
		 * (a split run would leave dserv holding two half-registrations). */
		if (!box_net_eth_get_ip(bip)) {
			continue;
		}

		/* ALL the %match lines go down ONE connection -- see
		 * box_net_eth_send_commands(). One connection per line races dserv's
		 * client teardown and silently lands only one of them. */
		char mcmd[3][160];
		const char *mptr[3];
		int fails = 0;

		if (full) {
			char rcmd[160];

			snprintf(rcmd, sizeof rcmd, "%%reg %u.%u.%u.%u %u 1\n",
				 bip[0], bip[1], bip[2], bip[3], BOX_ETH_CFG_PORT);
			/* %reg is its own connection: it makes dserv connect BACK to us,
			 * so let that settle before the matches follow. */
			if (reg_send_retry(dip, port, rcmd) != 0) fails++;
			k_msleep(150);
		}

		static const char *const pats[] = { "%s/config/*", "%s/cmd/*" };
		int n = 0;

		for (int i = 0; i < (int) ARRAY_SIZE(pats); i++) {
			char pat[96];

			snprintf(pat, sizeof pat, pats[i], pfx);
			snprintf(mcmd[n], sizeof mcmd[n], "%%match %u.%u.%u.%u %u %s 1\n",
				 bip[0], bip[1], bip[2], bip[3], BOX_ETH_CFG_PORT, pat);
			mptr[n] = mcmd[n];
			n++;
		}
		snprintf(mcmd[n], sizeof mcmd[n], "%%match %u.%u.%u.%u %u %s 1\n",
			 bip[0], bip[1], bip[2], bip[3], BOX_ETH_CFG_PORT, SYNC_DP);
		mptr[n] = mcmd[n];
		n++;

		for (int attempt = 0; attempt < 4; attempt++) {
			int bad = box_net_eth_send_commands(dip, port, mptr, n);

			if (bad == 0) {
				break;
			}
			if (attempt == 3) {
				fails += bad;
			}
			k_msleep(150);
		}

		printk("reg: %s as %s%s\n", full ? "registered" : "matches refreshed",
		       pfx, fails ? " (INCOMPLETE, watchdog will retry)" : "");
	}
}

K_THREAD_STACK_DEFINE(reg_stack, 2048);
static struct k_thread reg_thread;

static int u_eth_register(const box_config_t *c)
{
	reg_request(c, 1);
	return 0;
}

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
	/* Priority 7: below the service loop and the net RX threads. Registration is
	 * never urgent -- it must not preempt a DI publish -- but it must not be
	 * starved either, or a box comes up deaf. */
	k_thread_create(&reg_thread, reg_stack, K_THREAD_STACK_SIZEOF(reg_stack),
			reg_thread_fn, NULL, NULL, NULL, 7, 0, K_NO_WAIT);
	k_thread_name_set(&reg_thread, "extio_reg");
	box_net_eth_rx_wake_start();   /* inbound packets wake the loop, not the 1ms timeout */
#endif
	active = NULL;
	eth_streak = 0;
	box_uplink_service(cfg);
	return ok ? 0 : -1;                     /* at least one transport up */
}

#if defined(CONFIG_NETWORKING)
/* dserv NEVER retries its connect-back, so "registered" is not a state we can
 * assume once and forget: if dserv restarts, our %reg is gone from its table and
 * nothing tells the box. box_net_eth_server_up() is the only honest signal --
 * it is the connect-back itself. Down too long -> re-register. Up -> re-assert
 * the matches periodically, since a dropped match is equally silent.
 *
 * The refresh is skipped during an obs: a %match round is several bounded
 * blocking exchanges, and there is no reason to run them next to a sync edge. */
static void eth_reg_watchdog(const box_config_t *cfg)
{
	static int64_t down_ms, fresh_ms;
	static uint16_t rereg;
	static bool ever_up;      /* have we EVER completed a registration? */
	int64_t now = k_uptime_get();

	if (!box_net_eth_server_up()) {
		fresh_ms = now;
		if (!down_ms) { down_ms = now; return; }
		/* Fast ONLY for the first few attempts, then back off even if we have
		 * never registered. Gating purely on ever_up was a regression: a box
		 * whose dserv is unreachable -- a bench box, a wrong target, a box
		 * carried to another network -- never sets ever_up, so it retried
		 * every 250 ms FOREVER, each attempt doing blocking socket work.
		 * Observed on box3 as `re-registering (x276)` with the console
		 * dropping characters. The fast path exists to cover the one
		 * guaranteed-failed attempt at boot, not to poll a dead host. */
		int64_t gap = (ever_up || rereg > 20) ? REG_RETRY_MS : REG_FIRST_MS;

		if (now - down_ms < gap) { return; }
		down_ms = now;
		reg_request(cfg, 1);
		if (++rereg <= 3 || (rereg % 12) == 0) {   /* first few, then ~1/min */
			printk("reg: config link down -> re-registering (x%u)\n", rereg);
		}
		return;
	}
	down_ms = 0;
	if (!ever_up) {
		ever_up = true;
		printk("reg: first registration at %lld ms\n", now);
	}
	if (rereg) { printk("reg: config link restored\n"); rereg = 0; }
	if (now - fresh_ms >= MATCH_REFRESH_MS) {
		fresh_ms = now;
		if (!box_obs_active()) {
			reg_request(cfg, 0);
		}
	}
}
#endif

void box_uplink_service(const box_config_t *cfg)
{
	const box_uplink_if *want = desired(cfg);

#if defined(CONFIG_NETWORKING)
	/* Accept dserv's connect-back and keep registration honest. Runs regardless
	 * of which uplink is active: the config server is cheap, and a box that
	 * later promotes to eth should already be listening. */
	box_net_eth_server_service();
	if (active == &uplink_eth) {
		eth_reg_watchdog(cfg);
	}
#endif

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
