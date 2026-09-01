/*
 * box_status_led.c -- see the .h for why a handheld gets a reserved LED.
 */
#include "box_status_led.h"
#include "box_gpio.h"

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_BOX_BLE_PERIPHERAL)
#include "box_ble_periph.h"
#endif

#define BOX_USER_NODE DT_PATH(zephyr_user)

#if DT_NODE_HAS_PROP(BOX_USER_NODE, box_status_rgb)

/* Which BOX PINS the three colours are, rather than a second set of gpio
 * specs for pads box-gpios already describes. Two reasons, and the second is
 * the one that bites: a duplicate DT spec would carry its own copy of the
 * ACTIVE_LOW flag, so getting it wrong in one place would invert the LED
 * against the rest of the firmware; and going through box_gpio is what makes
 * the claim visible in pins/reserved instead of being a private back door.
 *
 * Exactly three entries: R, G, B. A board with one mono LED declares the same
 * pin three times -- every state then still renders, just monochrome, which is
 * a far better failure than a state that is invisible on that board. */
static const uint8_t rgb_pin[] = DT_PROP(BOX_USER_NODE, box_status_rgb);
BUILD_ASSERT(ARRAY_SIZE(rgb_pin) == 3, "box-status-rgb must be exactly R, G, B");

#define R_BIT BIT(0)
#define G_BIT BIT(1)
#define B_BIT BIT(2)
#define AMBER (R_BIT | G_BIT)

/* on_ms == period_ms means solid. Blip lengths are the shortest that reads as
 * a deliberate flash across a room rather than as a glitch. */
struct pattern {
	uint8_t  rgb;
	uint16_t on_ms;
	uint16_t period_ms;
};

static const struct pattern pat[BOX_STATUS_NSTATES] = {
	/* FAULT       */ { R_BIT, 100, 200 },   /* fast red: nothing works        */
	/* INVISIBLE   */ { R_BIT, 100, 1000 },  /* slow red: alive but unfindable */
	/* ADVERTISING */ { B_BIT,   8, 1500 },  /* blue blip: waiting             */
	/* LINKING     */ { AMBER, 500, 500 },   /* solid amber: mid-handshake     */
	/* STALLED     */ { AMBER,   8, 1500 },  /* amber blip: linked, not synced */
	/* READY       */ { G_BIT,   8, 3000 },  /* green blip: working            */
};

/* How long the echo count may stand still on a live link before this box stops
 * calling itself READY. The receiver sends one request per peer every 300 ms
 * (box_ble.h), so five seconds is ~16 consecutive misses -- far outside the
 * ordinary drop or the sync-burst latency changes, and still quick enough to
 * catch someone's eye while they are still holding the thing. */
#define ECHO_STALL_MS 5000

static box_status_t cur = BOX_STATUS_FAULT;
static int64_t  phase0;              /* when the current state began         */
static uint8_t  driven = 0xff;       /* last mask written; 0xff = none yet   */
static uint8_t  claimed;
static uint8_t  forced;              /* bench override active                */
static uint8_t  forced_rgb;

static void apply(uint8_t mask)
{
	if (mask == driven) {
		return;                  /* the common case: nothing to do */
	}
	for (int i = 0; i < 3; i++) {
		box_gpio_fw_set(rgb_pin[i], (mask & BIT(i)) ? 1 : 0);
	}
	driven = mask;
}

#if defined(CONFIG_BOX_BLE_PERIPHERAL)
static box_status_t decide(void)
{
	static uint32_t last_echo;
	static int64_t  last_advance;
	uint32_t echo = 0;
	uint16_t mtu = 0;

	if (!box_ble_periph_active()) {
		return BOX_STATUS_FAULT;
	}
	box_ble_periph_stats(&mtu, &echo, NULL, NULL);

	if (!box_ble_periph_ready()) {
		/* Not usable yet -- and WHICH not-usable is the whole value here,
		 * because the three cases want three different responses from
		 * whoever is holding the box.
		 *
		 * A non-zero MTU means a receiver IS connected (att_mtu is set in
		 * connected_cb and cleared on disconnect), so we are somewhere in
		 * the connect -> discover -> subscribe window, or the MTU came back
		 * too small for whole frames. Read that off the MTU rather than off
		 * `advertising`: a connected peripheral is not advertising either,
		 * so "not advertising" alone cannot tell a healthy handshake from a
		 * box nobody can see. */
		/* Re-arm the stall detector while there is no link to stall. The
		 * echo count is cumulative and does NOT reset on disconnect, so
		 * without this a box that reconnects after a minute away would be
		 * declared STALLED on its first pass back -- the detector reporting
		 * the gap it was down for as a fault in the link that replaced it. */
		last_advance = 0;
		if (mtu) {
			return BOX_STATUS_LINKING;
		}
		if (box_ble_periph_advertising()) {
			return BOX_STATUS_ADVERTISING;
		}
		/* Radio up, no link, no advertiser: invisible. Indistinguishable
		 * from a flat battery everywhere else in the system -- which is the
		 * single best reason this LED exists. */
		return BOX_STATUS_INVISIBLE;
	}
	/* Linked. Now: is the receiver actually disciplining our clock? */
	if (echo != last_echo) {
		last_echo = echo;
		last_advance = k_uptime_get();
	} else if (last_advance == 0) {
		last_advance = k_uptime_get();   /* first pass on a fresh link */
	}
	if (k_uptime_get() - last_advance > ECHO_STALL_MS) {
		return BOX_STATUS_STALLED;
	}
	return BOX_STATUS_READY;
}
#else
/* No radio compiled in: there is no story to tell, so tell none rather than
 * inventing one. box_status_led_init() never claims pins on such a build, so
 * the LEDs stay ordinary configurable outputs -- which is what a wired box
 * wants them to be. */
static box_status_t decide(void) { return cur; }
#endif

void box_status_led_init(void)
{
#if defined(CONFIG_BOX_BLE_PERIPHERAL)
	uint32_t mask = 0;

	for (int i = 0; i < 3; i++) {
		mask |= BIT(rgb_pin[i]);
	}
	box_gpio_fw_claim(mask);
	claimed = 1;
	phase0 = k_uptime_get();
#endif
}

void box_status_led_service(void)
{
	int64_t now;
	const struct pattern *p;
	uint32_t ph;

	if (!claimed) {
		return;
	}
	if (forced) {
		apply(forced_rgb);
		return;
	}
	now = k_uptime_get();
	{
		box_status_t st = decide();

		if (st != cur) {
			/* Restart the phase on every change so a transition shows
			 * up at once. Without this, entering a 3 s-period state
			 * two thirds of the way through its cycle would leave the
			 * LED dark for a second while the box looked hung. */
			cur = st;
			phase0 = now;
			driven = 0xff;
		}
	}
	p = &pat[cur];
	ph = (uint32_t) ((now - phase0) % p->period_ms);
	apply(ph < p->on_ms ? p->rgb : 0);
}

int box_status_led_claimed(void)
{
	return claimed;
}

box_status_t box_status_led_state(void)
{
	return cur;
}

void box_status_led_override(int auto_mode, uint8_t rgb)
{
	if (!claimed) {
		return;
	}
	if (auto_mode) {
		forced = 0;
		driven = 0xff;           /* force the next service pass to write */
		phase0 = k_uptime_get();
		return;
	}
	forced = 1;
	forced_rgb = rgb & (R_BIT | G_BIT | B_BIT);
}

int box_status_led_overridden(void)
{
	return forced;
}

#else  /* no box-status-rgb on this board */

void box_status_led_init(void) { }
void box_status_led_service(void) { }
int box_status_led_claimed(void) { return 0; }
box_status_t box_status_led_state(void) { return BOX_STATUS_FAULT; }
void box_status_led_override(int auto_mode, uint8_t rgb) { ARG_UNUSED(auto_mode); ARG_UNUSED(rgb); }
int box_status_led_overridden(void) { return 0; }

#endif

const char *box_status_led_state_name(box_status_t st)
{
	switch (st) {
	case BOX_STATUS_FAULT:       return "FAULT (radio down -- this box is mute)";
	case BOX_STATUS_INVISIBLE:   return "INVISIBLE (radio up, not advertising)";
	case BOX_STATUS_ADVERTISING: return "advertising (waiting for a receiver)";
	case BOX_STATUS_LINKING:     return "linking (connected, not subscribed)";
	case BOX_STATUS_STALLED:     return "STALLED (linked, echo-sync stopped)";
	case BOX_STATUS_READY:       return "ready (linked and synced)";
	default:                     return "?";
	}
}
