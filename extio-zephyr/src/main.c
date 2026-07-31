/*
 * extio-zephyr -- Zephyr application entry: the converged extio box.
 *
 * Boot order matters: GPIO and the uplink come up BEFORE any printk, because on
 * boards whose console is the box's own USB CDC (the board overlays) anything
 * printed before enumeration is dropped. After a settle delay we run the core
 * smoke test (the on-target twin of tools/box_sim.c --selftest), then enter the
 * service loop.
 *
 * The loop is the whole box in one place: arbitrate the uplink (USB / Ethernet
 * by carrier), dispatch inbound frames to config + GPIO, and publish local DI,
 * BLE ingress, and a 1 Hz watchdog out whichever uplink is active. Subsystems
 * absent on a given board (Ethernet on teensy40, BLE on any Teensy) are
 * compiled out via CONFIG_NETWORKING / CONFIG_BT -- see boards/<board>.conf.
 */
#include <zephyr/kernel.h>
#include <zephyr/version.h>
#include <zephyr/sys/reboot.h>
#include <stdio.h>

#include "dserv_config.h"
#include "box_persist.h"
#include "box_gpio.h"
#include "box_sched.h"
#include "box_group.h"
#include "box_clock.h"
#include "box_announce.h"
#include "box_boot.h"
#if defined(BOX_HAVE_ADC)
#include "box_ain.h"
#include "box_adc.h"
#endif
#include "box_obs.h"
#include "box_console.h"
#if defined(BOX_HAVE_PERSIST)
#include "box_flash.h"
#endif
#include "box_net_usb.h"
#include "box_uplink.h"
#include "box_event.h"
#if defined(CONFIG_NETWORKING)
#include "box_net_eth.h"
#include "box_ptp.h"
#endif
#if defined(CONFIG_BT)
#include "box_ble.h"
#endif
#if defined(BOX_HAVE_OTA_SLOT)
#include "box_ota_flash.h"
#include "box_ota.h"
#endif
#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
#include <zephyr/dfu/mcuboot.h>
#endif

static box_config_t   cfg;
static dserv_framer_t rx_framer;
static group_rt_t     groups[BOX_NGROUPS];   /* DI chord-settle state machines */
static box_clock_t    boxclk;                /* box time -> dserv time alignment */
static uint64_t       obs_begin_us;          /* box time of the last beginobs anchor */
#if defined(BOX_HAVE_PERSIST)
static int            box_persist_mount_err;   /* 0 = NVS mounted; else the errno */
#endif

/* ---- PTP anchoring -------------------------------------------------------
 *
 * When the box's 1588 clock is disciplined to the host NIC's PHC by PTP, the
 * host can hand us ONE constant:
 *
 *     ptp_offset_us = dserv_us - ptp_us
 *
 * It is constant because both terms are: dserv time is CLOCK_MONOTONIC plus a
 * fixed epoch offset, and the PHC is rate-locked to that same clock by phc2sys
 * (0.002 ppm -- see PORTING.md). The host measures it locally, with no wire
 * involved, so unlike an obs anchor it carries NO TRANSPORT TERM AT ALL. That is
 * the whole point: the one-way delay that has been so hard to subtract simply is
 * not in this path.
 *
 * With it we can re-anchor box_clock from the PTP clock alone, as often as we
 * like, without exchanging a single packet.
 */
#if defined(CONFIG_PTP_CLOCK)
static int64_t  ptp_offset_us;        /* dserv_us - ptp_us, from the host */
static int      ptp_offset_valid;

/* Sandwich a PTP read between two local reads, exactly as host/phc_offset.c
 * does: the midpoint pairs the two clocks and the spread bounds our own error.
 * Returns 0 and fills *box_us / *ptp_us, or -1 if the PTP clock is not ready. */
static int ptp_pair_read(uint64_t *box_us, uint64_t *ptp_us, uint32_t *window_us)
{
	if (!box_ptp_ready()) {
		return -1;
	}
	uint64_t b0 = box_gpio_now_us();
	uint64_t p  = box_ptp_now_ns();
	uint64_t b1 = box_gpio_now_us();

	if (!p) {
		return -1;
	}
	*box_us = (b0 + b1) / 2;
	*ptp_us = p / 1000u;
	if (window_us) {
		*window_us = (uint32_t)(b1 - b0);
	}
	return 0;
}

/* Re-anchor box_clock from PTP. Trusted, so it also teaches the rate estimator
 * the box-local-vs-PTP crystal error -- which is exactly what that estimator
 * was built for, now fed by a source with no transport jitter. */
static int ptp_reanchor(uint32_t *window_us)
{
	uint64_t box_us, ptp_us;

	if (!ptp_offset_valid || ptp_pair_read(&box_us, &ptp_us, window_us) != 0) {
		return -1;
	}
	box_clock_sync(&boxclk, (uint64_t)((int64_t) ptp_us + ptp_offset_us),
		       box_us, 1 /* trusted */);
	return 0;
}
#endif /* CONFIG_PTP_CLOCK */

static void publish_do(uint8_t pin, uint8_t level, uint64_t stamp);

/* ---- absolute-time triggering (`cmd/do/<pin>/at_abs <dserv_us>`) ----------
 *
 * The point of PTP: instead of "fire when this packet arrives" (transport delay
 * IN the path, hundreds of us of jitter), the host says "fire at absolute time
 * T" well in advance and every node schedules locally against its own synced
 * clock. Delivery jitter stops mattering entirely -- a frame only has to arrive
 * BEFORE T. Simultaneity across boxes is then bounded by clock sync (~us), not
 * by the network (~hundreds of us).
 *
 * T is in DSERV time. box_clock maps box->dserv; we need the inverse, and it
 * must undo the rate correction too or a distant T lands skewed:
 *
 *   stamp: dserv = box + offset + (box - anchor) * rate
 *   here : box   = dserv - offset - (box - anchor) * rate
 *
 * Solved by one fixed-point pass -- the correction is ppb-scale, so a single
 * refinement is far below the timer's own resolution.
 */
#if defined(CONFIG_PTP_CLOCK)
static struct k_timer abs_timer[BOX_NPINS];
static uint64_t       abs_target_us[BOX_NPINS];   /* intended dserv time */
/* Pins that have fired and still owe a publish. Written from the timer callback,
 * drained by the main loop -- one bit per pin, so BOX_NPINS must stay <= 32. */
BUILD_ASSERT(BOX_NPINS <= 32, "abs_pub_pending is a uint32_t bitmask");
static volatile uint32_t abs_pub_pending;
#endif /* CONFIG_PTP_CLOCK */

/* ---- deferred telemetry queue ----
 * The 1 Hz status burst pushed ~28 frames back-to-back, each a BLOCKING send of
 * 81-410 us. Measured on both boxes: the service loop's typical pass is 20 us,
 * its MAX was ~9.5 ms. Once a second the box stopped serving commands for that
 * long.
 *
 * Timing accuracy never depended on this -- timer callbacks preempt from the
 * timer ISR, and DI edges are stamped in their own ISR -- but an immediate
 * cmd/do/<pin> landing in the burst waited up to 9.5 ms, which is an order of
 * magnitude worse than the transport jitter time-triggering exists to remove.
 *
 * So the burst is now queued and drained a couple of frames per pass. Only
 * TELEMETRY goes in here: it is last-value-wins by nature, which is what makes
 * dropping the oldest on overflow the right policy. Events (DI edges, fired
 * pins) keep their own paths -- they must not be dropped. */
#define PUBQ_DEPTH        40  /* > one full burst, so a burst never self-drops */
#define PUBQ_PER_PASS_DEF  2  /* frames drained per service pass (runtime knob) */

static uint8_t pubq[PUBQ_DEPTH][DSERV_MSG_LEN];
static uint16_t pubq_head, pubq_tail, pubq_dropped;

/* cmd/pubq/bypass 1 -> send inline, i.e. the pre-queue behaviour. Exists ONLY
 * so the queue can be A/B'd at RUNTIME: reflashing between arms would reset the
 * boxes, re-anchor PTP and shift thermal state, confounding exactly the
 * difference being measured. With a runtime switch the two arms can be
 * interleaved instead, so slow drift cannot alias into the comparison. */
static uint8_t pubq_bypass;
/* Drain rate, runtime-settable via cmd/pubq/per_pass so it can be A/B'd without
 * reflashing.
 *
 * SETTLED 2026-07-27: it does not matter. Three arms (inline / 2 per pass /
 * 8 per pass), 5 interleaved reps each, 100 shots per run: two-box skew medians
 * 15.2 / 14.3 / 15.6 us, p90 37.8 / 36.2 / 37.0 us -- indistinguishable, with
 * within-arm spread far exceeding any between-arm difference. An earlier
 * 5-pair run that showed 2/pass ~4 us WORSE on p90 did NOT replicate; p90 from
 * 100 shots carries ~10 samples' worth of information and five reps agreeing is
 * ~6% likely by chance.
 *
 * So the knob exists for re-opening the question with a method that can resolve
 * a few microseconds, not because a setting is known to be better. The queue
 * itself is justified by bounded work per pass, nothing more. */
static uint8_t pubq_per_pass = PUBQ_PER_PASS_DEF;

#if defined(BOX_HAVE_OTA_SLOT)
/* Last cmd/ota/flashtest result, held in statics and RE-ANNOUNCED over several
 * 1 Hz ticks instead of enqueued inline.
 *
 * A flash burst blocks the service loop for its whole duration (MEASURED: 512 kB
 * = ~8 s, over which state/watchdog does not advance at all). When the loop
 * finally resumes, every missed 1 Hz tick fires back-to-back and each pushes a
 * full status burst, which overruns the 40-deep publish queue -- so the frames
 * enqueued inline right at the end of the test are precisely the ones dropped.
 * The test then looks like it never ran (state/ota/dbg/bytes stuck at the
 * previous run's value) while the box console shows it ran perfectly.
 *
 * That is the same trap OTA.md records on the RP2350 ("nothing that would show
 * success was visible, even though it succeeded"), and it produced a wrong
 * diagnosis here before the console was consulted. Re-announcing for a few
 * seconds lets the catch-up storm drain first. */
static uint32_t ota_dbg_bytes, ota_dbg_wall_ms, ota_dbg_e_max, ota_dbg_p_max;
static uint32_t ota_dbg_e_n, ota_dbg_p_n, ota_dbg_slot, ota_dbg_sector;
static int32_t  ota_dbg_verify, ota_dbg_rc;
/* WALL-CLOCK deadline, not a tick count -- and it must stay that way even though
 * the storm that forced it is now fixed at the source (see the catch-up policy on
 * the 1 Hz gate, which no longer fires once per missed period; measured drops
 * during a 512 kB burst went 230 -> 0).
 *
 * Keeping the re-announce is deliberate defence, not leftovers: a terminal OTA
 * result published exactly once, at the end of the operation most likely to have
 * disturbed the link, is the single worst-placed frame in the system, and
 * OTA.md's RP2350 hunt was prolonged by exactly that. Note a tick COUNTDOWN would
 * not do -- "5 ticks" is spent in ~5 ms whenever the gate is catching up, which
 * is the one moment it needs to survive. */
static int64_t  ota_dbg_until;

/* ---- OTA receive state (step 3) ----
 * cmd/ota/begin "<sha256-hex> <size>" opens a transfer into slot1; cmd/ota/chunk
 * carries the image as a strictly sequential byte stream; state/ota/ack reports
 * the contiguous cursor so the host paces and resumes. See box_ota.h.
 *
 * Delivery rides ORDINARY DATAPOINTS rather than the RP2350's raw 'D' frames.
 * The framer here does accept DSERV_OTA_CHAR, but on an Ethernet box there is no
 * host path that can inject raw bytes into the box's link: dserv owns the single
 * connect-back socket on BOX_ETH_CFG_PORT, and a second connection would displace
 * it -- taking the downlink down for the duration of the OTA, i.e. exactly when
 * we need it. A datapoint costs payload (name "extio/<box>/cmd/ota/chunk" eats
 * 24 of the 109-byte budget, leaving 85 -> 77 after the chunk header, vs 117 for
 * a 'D' frame) and buys a path that already works, unchanged, on both eth and
 * USB. The sink behind it is front-end agnostic, so a 'D'-frame or pull path can
 * be added later without touching box_ota.h. */
#define BOX_OTA_ACK_EVERY   8192u   /* ack the cursor at least this often */
#define BOX_OTA_CHUNK_HDR   8u      /* seq u32 LE + crc32 u32 LE */

/* How long each OTA step keeps analog switched off (box_ain_hold).
 *
 * Refreshed on every chunk, so the real hold lasts as long as the transfer keeps
 * moving. 2 s is chosen from the other end: it is how long analog stays dead
 * after the host STOPS, i.e. the cost of a vanished host, and it comfortably
 * covers the gap between chunks even when the host pauses for an ack. Making it
 * generous would only lengthen the outage nobody asked for.
 *
 * WHY HOLD AT ALL. An OTA needs the service loop and the flash; analog needs the
 * same service loop to publish, and at the per-group ceiling two groups can ask
 * for ~400 frames/s at 275-864 us of CPU each. That contention is arithmetic, not
 * a hypothesis -- unlike the analog-vs-PTP story, which our own boot loops
 * refuted (PORTING.md, "the actual bug: an RX context-descriptor race"). */
#define BOX_OTA_AIN_HOLD_MS 2000u
#endif /* BOX_HAVE_OTA_SLOT -- the analog boot hold below is not OTA-specific */

#if defined(BOX_HAVE_ADC)
/* ---- analog does not start until PTP has locked ----
 *
 * The sampler used to come up with the box and run at the configured rate --
 * 1 kHz on this board -- from before the network was even up, straight through
 * PTP convergence. Nothing needs it that early: the first analog sample matters
 * to an experiment, and no experiment has started 8 seconds into boot. Whereas
 * every sample taken before the clock is disciplined carries a box-clock stamp
 * that is seconds-to-decades wrong, so it is not merely useless, it is data that
 * has to be thrown away later by someone who notices.
 *
 * Whether it also HELPS convergence is deliberately left as an open question and
 * not claimed here -- our own boot loops have refuted that story once already.
 * The justification above stands on its own either way.
 *
 * CEILING, because a box with no grandmaster must still work. If PTP never
 * locks, analog starts anyway at BOOT_HOLD_MAX and the console says so, rather
 * than a box that is silently and permanently analog-dead on a LAN that happens
 * to have no master. That is the same rule as every other deadline here: the
 * failure of the thing we are waiting for must not be able to disable us. */
#define BOX_AIN_BOOT_HOLD_MAX_MS  60000
#define BOX_AIN_BOOT_HOLD_STEP_MS 1500     /* re-armed each pass; > the loop period */

static uint8_t ain_boot_released;

static void ain_boot_gate(void)
{
	if (ain_boot_released) {
		return;
	}

	/* NOTHING TO GATE. With ain_en clear, or with no group defined, no sample
	 * was ever going to be taken -- so holding achieves nothing and, worse,
	 * announcing a release states an event that did not happen. The first
	 * version printed "analog: released ... samples are stamped on a
	 * free-running clock" on a box with ain_en=0, i.e. it reported sampling
	 * that could not occur, after holding a nonexistent sampler for a full
	 * minute.
	 *
	 * Deliberately NOT latching ain_boot_released here: analog can be enabled
	 * at runtime, and if that happens before PTP locks it should still get the
	 * same treatment. Re-evaluated every pass instead. */
	if (!cfg.ain_en || dserv_ain_active_count(&cfg) == 0) {
		return;
	}

#if defined(CONFIG_PTP_CLOCK)
	if (box_ptp_synced()) {
		ain_boot_released = 1;
		box_console_printf("analog: released at %lld ms -- PTP locked\n",
				   k_uptime_get());
		return;
	}
	if (k_uptime_get() < BOX_AIN_BOOT_HOLD_MAX_MS) {
		box_ain_hold(BOX_AIN_BOOT_HOLD_STEP_MS);
		return;
	}
	ain_boot_released = 1;
	box_console_printf("analog: released at %d ms WITHOUT PTP -- samples are "
			   "stamped on a free-running clock\n",
			   BOX_AIN_BOOT_HOLD_MAX_MS);
#else
	ain_boot_released = 1;      /* no 1588 on this board; nothing to wait for */
#endif
}
#endif /* BOX_HAVE_ADC */

#if defined(BOX_HAVE_OTA_SLOT)

static int64_t   ota_res_until;   /* re-announce window for the TERMINAL result */
static uint32_t  g_ota_base;      /* where the image starts inside slot1 */
static box_ota_t g_ota;
static uint8_t   g_ota_active;
static uint32_t  g_ota_size;
static uint32_t  g_ota_ack_at;

/* box_ota works in IMAGE offsets (0 = first byte of the image); the flash layer
 * works in SLOT offsets. g_ota_base is the difference, and applying it in one
 * place keeps the portable sink unaware of the swap mode entirely. */
static int ota_erase(uint32_t off)
{
	return box_ota_flash_erase(off + g_ota_base);
}

static int ota_program(uint32_t off, const uint8_t *page, uint32_t len)
{
	return box_ota_flash_program(off + g_ota_base, page, len);
}

static const box_ota_flash_t g_ota_flash_ops = {
	.erase   = ota_erase,
	.program = ota_program,
};
#endif

static void pub_enqueue(const uint8_t *f)
{
	if (pubq_bypass) {
		box_uplink_send(f, DSERV_MSG_LEN);
		return;
	}
	uint16_t nxt = (uint16_t)((pubq_head + 1) % PUBQ_DEPTH);

	if (nxt == pubq_tail) {                 /* full: drop the OLDEST sample */
		pubq_tail = (uint16_t)((pubq_tail + 1) % PUBQ_DEPTH);
		pubq_dropped++;
	}
	memcpy(pubq[pubq_head], f, DSERV_MSG_LEN);
	pubq_head = nxt;
}

static void pub_drain(int max)
{
	while (max-- > 0 && pubq_tail != pubq_head) {
		box_uplink_send(pubq[pubq_tail], DSERV_MSG_LEN);
		pubq_tail = (uint16_t)((pubq_tail + 1) % PUBQ_DEPTH);
	}
}

/* ---- periodic counters: publish ONLY when they change ----
 *
 * The 1 Hz block was emitting 17 datapoints a second, TWELVE of them ain/dbg/*,
 * and on a box with analog idle not one of those twelve ever changed: twelve
 * 128-byte frames per second restating identical numbers. A single frame costs
 * 275-864 us of CPU to send (CMakeLists.txt), so the telemetry alone was
 * 0.5-1.5% of the service loop, most of it spent saying nothing.
 *
 * Bandwidth was never the issue -- 128 bytes is ~10 us of wire time at 100 Mb.
 * It is PER-SEND overhead, which is why not sending is worth far more than
 * sending more efficiently.
 *
 * KEYED BY THE LEAF POINTER, not by call order. Order-keyed slots would work
 * today (the only conditional group is gated on box_adc_ready(), which never
 * changes after init) but would silently compare a value against the WRONG
 * history the first time anyone made a group conditional. The leaves are string
 * literals with stable addresses, so identity is free and the failure mode is
 * removed rather than merely avoided.
 *
 * A FULL REFRESH EVERY PUB_REFRESH_S, because change-only has one real failure:
 * dserv retains, so a frame lost in transit leaves the host holding a stale
 * value with nothing to correct it -- and for a genuinely static field like
 * ain/dbg/chans, "until it changes again" is never. The refresh bounds that
 * staleness to a known number of seconds. It is also what re-seeds a host that
 * connected between refreshes without a full announce.
 *
 * WATCHDOG IS DELIBERATELY NOT ROUTED THROUGH HERE. Its whole purpose is to
 * advance every second so a frozen one is diagnostic; making it conditional on
 * change would be harmless today (it is a counter) and catastrophic the moment
 * someone made it a level. */
#define PUB_PERIODIC_MAX 24
#define PUB_REFRESH_S    30

static struct { const char *leaf; uint32_t v; uint8_t seen; } pub_hist[PUB_PERIODIC_MAX];
static int     pub_force;          /* set once per pass; forces a full refresh */
static int64_t pub_refresh_at;
static uint32_t pub_suppressed;    /* frames NOT sent -- the win, measured */

static void pub_periodic(const char *leaf, uint32_t v)
{
	unsigned i;

	for (i = 0; i < PUB_PERIODIC_MAX; i++) {
		if (pub_hist[i].leaf == leaf) {
			break;
		}
		if (pub_hist[i].leaf == NULL) {
			pub_hist[i].leaf = leaf;
			break;
		}
	}
	if (i < PUB_PERIODIC_MAX && pub_hist[i].seen && pub_hist[i].v == v && !pub_force) {
		pub_suppressed++;
		return;                    /* unchanged -- say nothing */
	}
	if (i < PUB_PERIODIC_MAX) {
		pub_hist[i].seen = 1;
		pub_hist[i].v = v;
	}
	/* i == MAX means the table overflowed: publish unconditionally rather than
	 * drop the datapoint. Costs the old behaviour for the excess, never
	 * silence. */
	{
		uint8_t pf[DSERV_MSG_LEN];
		char pn[80];

		dserv_state_name(&cfg, pn, sizeof pn, leaf);
		dserv_msg_int(pf, pn, 0, (int32_t) v);
		pub_enqueue(pf);
	}
}

#if defined(BOX_HAVE_OTA_SLOT)
static void ota_pub_int(const char *leaf, int32_t v)
{
	uint8_t f[DSERV_MSG_LEN];
	char nm[80];

	dserv_state_name(&cfg, nm, sizeof nm, leaf);
	dserv_msg_int(f, nm, 0, v);
	pub_enqueue(f);
}

static void ota_pub_str(const char *leaf, const char *v)
{
	uint8_t f[DSERV_MSG_LEN];
	char nm[80];

	dserv_state_name(&cfg, nm, sizeof nm, leaf);
	dserv_msg_string(f, nm, 0, v);
	pub_enqueue(f);
}

/* Push analog aside for the next step of this transfer.
 *
 * Called at the TOP of each flash-touching OTA command, but AFTER its obs gate:
 * an observation period outranks an update (the OTA defers to it), so analog --
 * which is what an obs is often there to record -- must not be switched off for
 * work that is not happening. */
static void ota_ain_yield(void)
{
#if defined(BOX_HAVE_ADC)
	box_ain_hold(BOX_OTA_AIN_HOLD_MS);
#endif
}

/* Terminal outcome: state + result together, and mark them for re-announce.
 * Anything published exactly once at the end of an OTA is the frame most likely
 * to be lost -- see the ota_dbg_until comment. */
static void ota_pub_done(void)
{
	ota_pub_str("ota/state",  box_ota_state_str(g_ota.state));
	ota_pub_str("ota/result", box_ota_err_str(g_ota.err));
	ota_pub_int("ota/progress", box_ota_progress_pct(&g_ota));
	ota_pub_int("ota/ack", (int32_t) g_ota.received);
	ota_res_until = k_uptime_get() + 6000;
}
#endif /* BOX_HAVE_OTA_SLOT */

#if defined(CONFIG_PTP_CLOCK)

static uint64_t dserv_to_box_us(const box_clock_t *c, uint64_t dserv_us)
{
	int64_t box = (int64_t) dserv_us - c->offset_us;

	for (int i = 0; i < 2; i++) {
		int64_t dt   = box - (int64_t) c->anchor_box_us;
		int64_t corr = (dt * (int64_t) c->rate_ppb) / 1000000000LL;
		box = (int64_t) dserv_us - c->offset_us - corr;
	}
	return (uint64_t) box;
}

static void abs_fire(struct k_timer *t)
{
	int pin = (int) (intptr_t) k_timer_user_data_get(t);

	if (pin < 0 || pin >= BOX_NPINS) {
		return;
	}
	gpio_cmd_t c = { .op = GPIO_OP_SET, .pin = (uint8_t) pin, .value = 1 };
	box_gpio_exec(&cfg, &c);

	/* DO NOT PUBLISH HERE. publish_do() ends in a BLOCKING box_uplink_send(),
	 * and this is a k_timer callback: two pins scheduled for the same instant
	 * run their callbacks back-to-back, so the first one's send delays the
	 * second one's GPIO write by a whole network round.
	 *
	 * Measured on a scope, two pins on ONE box at one T (so PTP and tick
	 * quantisation contribute exactly zero): 30/30 shots landed 127-254 us
	 * apart, median 208, with the sign varying -- i.e. whichever callback ran
	 * second paid for the first one's publish. That matches dbg/send_us
	 * (median 142 us) almost exactly.
	 *
	 * Set the pin, flag it, get out. The main loop publishes. Accuracy is
	 * untouched: abs_target_us[] already holds the INTENDED time, which is what
	 * gets stamped whenever the publish actually goes out. */
	abs_pub_pending |= (1u << pin);
}

/* Every term behind the armed/late decision, for when a box insists it is late
 * and the clock looks fine. This is how the at_abs 64-bit truncation was found:
 * reading the source could not settle it, dbg_T = 2147483647 did instantly.
 *
 * OFF by default, and the CALLER must invoke this only AFTER k_timer_start():
 * lead_us is measured from a `now` read before the publishes, so seven uplink
 * frames sitting between the two would delay the fire by exactly that much --
 * the one thing this path must never do.
 *
 * Enable live, no reflash:  dservSet <prefix>/cmd/sched/debug 1
 * Deliberately NOT persisted, so a box always boots quiet. */
static uint8_t sched_dbg;

static void sched_dbg_publish(uint64_t T, uint64_t now, uint64_t target_box,
			      int64_t lead_us)
{
	static const char *const leaf[] = {
		"sched/dbg_T",      "sched/dbg_now",    "sched/dbg_off",
		"sched/dbg_anchor", "sched/dbg_rate",   "sched/dbg_target",
		"sched/dbg_lead",
	};
	const int64_t val[] = {
		(int64_t) T, (int64_t) now, boxclk.offset_us,
		(int64_t) boxclk.anchor_box_us, (int64_t) boxclk.rate_ppb,
		(int64_t) target_box, lead_us,
	};
	uint8_t f[DSERV_MSG_LEN];
	char    nm[80];

	for (unsigned i = 0; i < ARRAY_SIZE(val); i++) {
		dserv_state_name(&cfg, nm, sizeof nm, leaf[i]);
		dserv_msg_int64(f, nm, 0, val[i]);
		pub_enqueue(f);
	}
}
#endif /* CONFIG_PTP_CLOCK */

/* A hardware sync edge may anchor an obs toggle only if it is RECENT: well under
 * the shortest obs on/off cadence (seconds), so a stale latched edge from the
 * previous toggle can never anchor the current one. */
#define SYNC_EDGE_WINDOW_US 250000

/* Every published event time goes through here. Before the first sync this
 * returns 0, which tells dserv to arrival-stamp -- events still publish, they
 * are just not aligned yet. */
static inline uint64_t event_stamp(uint64_t t_us)
{
	return box_clock_stamp(&boxclk, t_us);
}

/* The console's `now` command, answered here because boxclk is private to this
 * file. Deliberately the SAME call event_stamp() makes, so `now` reports the
 * mapping that real event timestamps actually use rather than a parallel
 * reimplementation that could agree in testing and diverge in the field. */
uint64_t box_app_dserv_now_us(void)
{
	return event_stamp(box_gpio_now_us());
}

/* Publish one settled chord: extio/<name>/state/group/<label>, value = the
 * member bitmask (bit i = i-th LOWEST member pin, the order the manifest
 * announces), stamped at the episode's onset edge. */
static void publish_group(int g, uint8_t bits, uint64_t t_us)
{
	uint8_t f[DSERV_MSG_LEN];
	char gn[BOX_LABEL_MAX + 4], leaf[40], nm[80];

	dserv_group_name(&cfg, g, gn, sizeof gn);
	snprintf(leaf, sizeof leaf, "group/%s", gn);
	dserv_state_name(&cfg, nm, sizeof nm, leaf);
	dserv_msg_int(f, nm, t_us ? event_stamp(t_us) : 0, bits);
	box_uplink_send(f, DSERV_MSG_LEN);
}

/* Publish a DO level: extio/<name>/state/do/<pin>, stamped at the instant the
 * pin actually moved (not at command arrival). Matches the Pico exactly --
 * SET only, from the datapoint path only -- so hosts decode both boxes alike.
 * Consumers: UIs showing live output state, and host/loopback_rtt.tcl, which
 * reads it to pick the next level (the .sh variant just alternates). */
static void publish_do(uint8_t pin, uint8_t level, uint64_t stamp)
{
	uint8_t f[DSERV_MSG_LEN];
	char leaf[24], nm[80];

	snprintf(leaf, sizeof leaf, "do/%u", pin);
	dserv_state_name(&cfg, nm, sizeof nm, leaf);
	dserv_msg_int(f, nm, stamp, level);
	box_uplink_send(f, DSERV_MSG_LEN);
}

/* Clock-alignment telemetry, published at every obs anchor. This is how you tell
 * from the host side whether stamping is trustworthy: `source` says whether the
 * anchor was the hardware TTL edge or mere frame arrival, `transport_us` is the
 * frame's transit lag measured against that edge (hw anchors only -- it is
 * exactly the error a hw anchor removes), and `rate_ppb` appears once enough
 * trusted pairs have taught the crystal rate. */
static void publish_sync(uint64_t dserv_us, uint64_t box_us, int64_t offset_us,
			 int hw, int64_t transport_us)
{
	uint8_t f[DSERV_MSG_LEN];
	char nm[80];

	dserv_state_name(&cfg, nm, sizeof nm, "sync/dserv_us");
	dserv_msg_int64(f, nm, dserv_us, (int64_t) dserv_us);
	box_uplink_send(f, DSERV_MSG_LEN);

	dserv_state_name(&cfg, nm, sizeof nm, "sync/box_us");
	dserv_msg_int64(f, nm, dserv_us, (int64_t) box_us);
	box_uplink_send(f, DSERV_MSG_LEN);

	dserv_state_name(&cfg, nm, sizeof nm, "sync/offset_us");
	dserv_msg_int64(f, nm, dserv_us, offset_us);
	box_uplink_send(f, DSERV_MSG_LEN);

	dserv_state_name(&cfg, nm, sizeof nm, "sync/source");
	dserv_msg_string(f, nm, dserv_us, hw ? "hw" : "sw");
	box_uplink_send(f, DSERV_MSG_LEN);

	if (transport_us >= 0) {
		dserv_state_name(&cfg, nm, sizeof nm, "sync/transport_us");
		dserv_msg_int64(f, nm, dserv_us, transport_us);
		box_uplink_send(f, DSERV_MSG_LEN);
	}
	if (boxclk.rate_valid) {          /* learned crystal rate: hw anchors only */
		dserv_state_name(&cfg, nm, sizeof nm, "sync/rate_ppb");
		dserv_msg_int(f, nm, dserv_us, boxclk.rate_ppb);
		box_uplink_send(f, DSERV_MSG_LEN);
	}
}

/* (Re)seed every group from the pins' CURRENT logical levels, so a switch
 * already held when a group is (re)configured is the baseline rather than a
 * phantom edge. Call after any change to the group/pin map. */
static void groups_resync(void)
{
	uint8_t levels[BOX_NPINS];

	box_gpio_read_di_levels(&cfg, levels);
	for (int g = 0; g < BOX_NGROUPS; g++) {
		group_reset(&groups[g], &cfg, g, levels);
	}
}

/* Demo pins, per board. box pin n -> <box-gpio-port>.n (see box_gpio.h), so
 * these land on real hardware on each target. */
#if defined(CONFIG_BOARD_FRDM_RW612)
#define LED_PIN  12   /* hsgpio0.12 = user LED (active low physically) */
#define BTN_PIN  11   /* hsgpio0.11 = User SW2 (active low, pull-up)    */
#elif defined(CONFIG_BOARD_FRDM_MCXN947)
/* box pin 9 = D9 = P0_10 = RED LED, declared GPIO_ACTIVE_LOW in the overlay so
 * the pulse lights it. Pin 10 (GREEN) is deliberately NOT used: that is the
 * conventional obs-mirror pin, and a boot heartbeat sharing it makes "is the rig
 * in an observation period" ambiguous for the first second after every reset.
 *
 * Before this branch existed the #else below applied, so LED_PIN was 3 -- box
 * pin 3 is D3 on this board, an ORDINARY HEADER PIN. Two costs, both real:
 * there was no visible boot indicator (so "is it even booting?" needed a console
 * or a scope -- it came up on 2026-07-29 while diagnosing wall power), and every
 * boot fired three 120 ms pulses into D3, which is a signal pin someone may have
 * wired something to. A board-specific default that lands on a signal pin is
 * worse than no default. */
#define LED_PIN  9    /* D9  = P0_10, RED user LED (active low in DT)   */
#define BTN_PIN  17   /* A5  = P0_23, also SW2                          */
#else                 /* Teensy 4.x: board LED is gpio2.3 */
#define LED_PIN  3    /* gpio2.3 = on-board LED                         */
#define BTN_PIN  4    /* gpio2.4 = a free pin for a test button         */
#endif

/* The rig's obs begin/end edge. The host module forwards this to EVERY box
 * (config/extioconf.tcl: dservAddMatch ess/in_obs -> usbio_forward), and unlike
 * everything else inbound it is NOT an extio/<name>/... key -- so dserv_dispatch
 * never matches it and it must be handled before the dispatch, exactly as the
 * Pico's frame handler does. */
#define BOX_SYNC_DP "ess/in_obs"

/* One inbound 128-byte frame (config/cmd/ess-in_obs) from the host module:
 * dispatch it into the config, and run any GPIO command it produced. */
/* Inbound frames accepted, published once a second as state/cmds_rx.
 *
 * This exists because "publishing but deaf" is the nastiest failure this box
 * has: the uplink works, state/* keeps flowing, and every status field says
 * healthy -- while the %match registration is gone and cmd/* silently never
 * arrives. It has cost hours twice now, and no field on the box revealed it.
 *
 * A COUNTER does, and needs no interpretation: watch any state/* timestamp
 * advance while cmds_rx sits still and the downlink is dead. It is monotonic
 * from boot, so a host can also spot a reboot it missed (the count drops).
 * Counts every accepted inbound frame, not just pin commands -- the question is
 * whether the path works at all. */
static uint32_t cmds_rx;

/* Exposed for the console's `show` (box_console.h). */
uint32_t box_cmds_rx(void) { return cmds_rx; }

/* "<prefix>/foo/bar" -> "foo/bar", once per frame.
 *
 * The matchers below used to rebuild a full key per candidate and compare the
 * whole thing -- and the at_abs one did that BOX_NPINS times, so every inbound
 * frame cost ~30 snprintf + strcmp whether or not it matched anything. Measured
 * dbg/disp_us: ~470 us per frame, nearly all of it string formatting.
 *
 * This is the same strip dserv_dispatch() already does (memcmp the prefix, work
 * on the leaf); these Zephyr-only matchers just never used it because src/core/
 * is shared verbatim with the Pico and they live here instead. */
static const char *frame_leaf(const dserv_msg_t *m, char *buf, size_t buflen)
{
	char pfx[64];
	int plen = dserv_cfg_prefix(&cfg, pfx, sizeof pfx);

	if (plen <= 0 || m->namelen < (uint16_t)(plen + 1)) {
		return NULL;
	}
	if (memcmp(m->name, pfx, (size_t) plen) != 0 || m->name[plen] != '/') {
		return NULL;                       /* not addressed to this box */
	}
	uint16_t sl = (uint16_t)(m->namelen - (plen + 1));

	if (sl >= buflen) {
		sl = (uint16_t)(buflen - 1);
	}
	memcpy(buf, m->name + plen + 1, sl);
	buf[sl] = '\0';
	return buf;
}

static void on_usb_frame(const uint8_t *frame, void *ud)
{
	ARG_UNUSED(ud);
	dserv_msg_t m;
	if (dserv_msg_parse(frame, &m) != 0) {
		return;
	}
	cmds_rx++;

	char leafbuf[112];
	const char *leaf = frame_leaf(&m, leafbuf, sizeof leafbuf);

#if defined(CONFIG_PTP_CLOCK)
	/* <prefix>/cmd/ptp/offset <us> -- the host's PHC->dserv constant.
	 *
	 * Handled HERE rather than in dserv_cfg__cmd() because src/core/ is shared
	 * verbatim with the Pico and this is Zephyr/PTP-only. Same precedent as
	 * ess/in_obs below, which is also intercepted before dispatch.
	 *
	 * Anchoring is gated on !in_obs for the reason PORTING.md records: a
	 * re-anchor STEPS the offset, and applying that inside a data-collection
	 * window puts two events of one trial on different mappings. With PTP the
	 * step is sub-us rather than the hundreds of us an obs anchor moves, but
	 * the rule is about correctness, not magnitude.
	 */
	/* <prefix>/cmd/do/<pin>/at_abs <dserv_us> -- fire at an ABSOLUTE instant. */
	{
		int pin2, pos = -1;

		/* ONE parse of the leaf, not BOX_NPINS candidate keys. %n + the
		 * explicit NUL check is the same guard dserv_config.h uses, so
		 * "cmd/do/18/at_absXYZ" cannot match pin 18. */
		if (leaf &&
		    sscanf(leaf, "cmd/do/%d/at_abs%n", &pin2, &pos) == 1 &&
		    pos > 0 && leaf[pos] == '\0' &&
		    pin2 >= 0 && pin2 < BOX_NPINS) {
			uint64_t T   = (uint64_t) dserv_msg_as_ll(&m);
			uint64_t now = box_gpio_now_us();
			uint8_t  f3[DSERV_MSG_LEN];
			char     nm3[80];

			if (!boxclk.synced) {
				dserv_state_name(&cfg, nm3, sizeof nm3, "sched/abs_err");
				dserv_msg_string(f3, nm3, 0, "unsynced");
				pub_enqueue(f3);
				return;
			}

			uint64_t target_box = dserv_to_box_us(&boxclk, T);
			int64_t  lead_us    = (int64_t) target_box - (int64_t) now;

			/* NEVER fire late. A box that silently fires milliseconds after T
			 * is far worse than one that says it missed -- the whole value of
			 * time-triggering is that every node agrees on the instant. */
			if (lead_us <= 0) {
				dserv_state_name(&cfg, nm3, sizeof nm3, "sched/abs_late_us");
				dserv_msg_int64(f3, nm3, 0, -lead_us);
				pub_enqueue(f3);
				dserv_state_name(&cfg, nm3, sizeof nm3, "sched/abs_err");
				dserv_msg_string(f3, nm3, 0, "late");
				pub_enqueue(f3);
				if (sched_dbg) {
					sched_dbg_publish(T, now, target_box, lead_us);
				}
				return;
			}

			abs_target_us[pin2] = T;
			k_timer_stop(&abs_timer[pin2]);
			k_timer_user_data_set(&abs_timer[pin2], (void *) (intptr_t) pin2);
			k_timer_start(&abs_timer[pin2], K_USEC(lead_us), K_NO_WAIT);

			/* Publish the lead so the host can see the margin it actually got
			 * -- shrinking lead is the early warning before anything is late. */
			dserv_state_name(&cfg, nm3, sizeof nm3, "sched/abs_lead_us");
			dserv_msg_int64(f3, nm3, 0, lead_us);
			pub_enqueue(f3);
			dserv_state_name(&cfg, nm3, sizeof nm3, "sched/abs_err");
			dserv_msg_string(f3, nm3, 0, "armed");
			pub_enqueue(f3);
			if (sched_dbg) {
				sched_dbg_publish(T, now, target_box, lead_us);
			}
			return;
		}
	}

#if defined(CONFIG_PTP_CLOCK)
	/* <prefix>/cmd/sched/debug 0|1 -- arm the at_abs diagnostics. Runtime, not
	 * persisted: turn it on against a live box, read sched/dbg_*, turn it off.
	 * See sched_dbg_publish() for why it may only run after the timer is armed. */
	{
		if (leaf && strcmp(leaf, "cmd/pubq/per_pass") == 0) {
			long v = dserv_msg_as_long(&m);

			if (v < 1)  v = 1;
			if (v > PUBQ_DEPTH) v = PUBQ_DEPTH;
			pubq_per_pass = (uint8_t) v;

			uint8_t f5[DSERV_MSG_LEN];
			char nm5[80];
			dserv_state_name(&cfg, nm5, sizeof nm5, "pubq/per_pass");
			dserv_msg_int(f5, nm5, 0, (int32_t) pubq_per_pass);
			pub_enqueue(f5);
			return;
		}

		if (leaf && strcmp(leaf, "cmd/pubq/bypass") == 0) {
			pubq_bypass = dserv_msg_as_long(&m) ? 1 : 0;

			uint8_t f4[DSERV_MSG_LEN];
			char nm4[80];
			dserv_state_name(&cfg, nm4, sizeof nm4, "pubq/bypass");
			dserv_msg_int(f4, nm4, 0, (int32_t) pubq_bypass);
			pub_enqueue(f4);
			return;
		}

		if (leaf && strcmp(leaf, "cmd/sched/debug") == 0) {
			sched_dbg = dserv_msg_as_long(&m) ? 1 : 0;

			uint8_t f2[DSERV_MSG_LEN];
			char nm[80];
			dserv_state_name(&cfg, nm, sizeof nm, "sched/debug");
			dserv_msg_int(f2, nm, 0, (int32_t) sched_dbg);
			pub_enqueue(f2);
			return;
		}
	}
#endif /* CONFIG_PTP_CLOCK */

	{
		if (leaf && strcmp(leaf, "cmd/ptp/offset") == 0) {
			ptp_offset_us    = dserv_msg_as_ll(&m);
			ptp_offset_valid = 1;

			uint32_t win = 0;
			int ok = box_obs_active() ? -1 : ptp_reanchor(&win);

			uint8_t f2[DSERV_MSG_LEN];
			char nm[80];
			dserv_state_name(&cfg, nm, sizeof nm, "ptp/offset_us");
			dserv_msg_int64(f2, nm, 0, ptp_offset_us);
			pub_enqueue(f2);

			if (ok == 0) {
				/* A FOURTH sync source beside hw / swc / sw, so a datafile
				 * records which mechanism produced its timestamps. */
				dserv_state_name(&cfg, nm, sizeof nm, "sync/source");
				dserv_msg_string(f2, nm, 0, "ptp");
				pub_enqueue(f2);

				dserv_state_name(&cfg, nm, sizeof nm, "sync/ptp_window_us");
				dserv_msg_int(f2, nm, 0, (int32_t) win);
				pub_enqueue(f2);

				dserv_state_name(&cfg, nm, sizeof nm, "sync/offset_us");
				dserv_msg_int64(f2, nm, 0, boxclk.offset_us);
				pub_enqueue(f2);
			}
			return;
		}
	}
#endif

#if defined(BOX_HAVE_OTA_SLOT)
	/* <prefix>/cmd/ota/flashtest <kb> -- OTA step 2's execute-while-write probe.
	 *
	 * The hazard: slot1 writes hit the same FlexSPI device slot0 is XIP-ing
	 * from, so every erase/program suspends instruction fetch, and the stall
	 * lands on our single service loop. `cmd/save` already writes NVS while
	 * running, but that is a ~1 kB blob -- a 640 kB image is a completely
	 * different duty cycle, and "it worked for 1 kB" is not evidence.
	 *
	 * So measure it before building the receive path on top. What matters is
	 * NOT the burst total (a real OTA interleaves writes with service passes)
	 * but the worst SINGLE operation, because that is the atomic hole the loop
	 * cannot be scheduled through. Read ota/dbg/erase_max_us and
	 * ota/dbg/prog_max_us, and watch dbg/loop_max_us over the same window.
	 *
	 * Writes a deterministic pattern and reads it back, so this also proves the
	 * slot is actually writable rather than merely accepting the calls -- the
	 * same "check it RAN, not that the register took the write" lesson the TDDR
	 * PLL bug taught. Nothing here touches slot0 or the running image. */
	if (leaf && strcmp(leaf, "cmd/ota/flashtest") == 0) {
		int32_t  verify_ok = 0;
		int      rc;
		uint32_t done = 0, wall_us = 0;

		if (box_obs_active()) {          /* never program flash mid-trial */
			ota_dbg_rc    = -EBUSY;
			ota_dbg_until = k_uptime_get() + 6000;
			return;
		}

		long kb = dserv_msg_as_long(&m);
		if (kb <= 0)    kb = 64;
		if (kb > 1024)  kb = 1024;       /* bounded: this stalls the loop */

		rc = box_ota_flash_open();
		if (rc == 0) {
			uint32_t want   = (uint32_t) kb * 1024u;
			uint32_t sector = box_ota_flash_sector();
			uint32_t cap    = box_ota_flash_size();

			if (want > cap) {
				want = cap;
			}
			box_ota_flash_stats_reset();

			uint8_t  page[256];
			uint32_t c0 = k_cycle_get_32();

			for (done = 0; done < want && rc == 0; done += sizeof page) {
				if (sector && (done % sector) == 0) {
					rc = box_ota_flash_erase(done);
					if (rc != 0) break;
				}
				for (uint32_t i = 0; i < sizeof page; i++) {
					page[i] = (uint8_t) ((done + i) * 31u + 7u);
				}
				rc = box_ota_flash_program(done, page, sizeof page);
			}
			wall_us = k_cyc_to_us_floor32(k_cycle_get_32() - c0);

			/* Read back a sample rather than the whole span: the point is
			 * "did the bytes land", and re-reading 1 MB adds stall for no
			 * extra information. First, last and middle page. */
			if (rc == 0 && done) {
				uint32_t probes[3] = { 0, (done / 2) & ~255u,
						       done - sizeof page };
				verify_ok = 1;
				for (int p = 0; p < 3 && verify_ok; p++) {
					uint8_t got[256];

					if (box_ota_flash_read(probes[p], got, sizeof got) != 0) {
						verify_ok = 0;
						break;
					}
					for (uint32_t i = 0; i < sizeof got; i++) {
						if (got[i] != (uint8_t) ((probes[p] + i) * 31u + 7u)) {
							verify_ok = 0;
							break;
						}
					}
				}
			}

			ota_dbg_slot   = cap;
			ota_dbg_sector = sector;
		}

		uint32_t e_max = 0, p_max = 0, e_n = 0, p_n = 0;
		box_ota_flash_stats(&e_max, &p_max, &e_n, &p_n);

		ota_dbg_bytes   = done;
		ota_dbg_wall_ms = wall_us / 1000u;
		ota_dbg_e_max   = e_max;
		ota_dbg_p_max   = p_max;
		ota_dbg_e_n     = e_n;
		ota_dbg_p_n     = p_n;
		ota_dbg_verify  = verify_ok;
		ota_dbg_rc      = (int32_t) rc;
		ota_dbg_until   = k_uptime_get() + 6000;   /* outlast the catch-up storm */

		box_console_printf("cmd/ota/flashtest -> %u B in %u ms, "
				   "erase_max %u us, prog_max %u us, verify %s (rc %d)\n",
				   done, wall_us / 1000u, e_max, p_max,
				   verify_ok ? "ok" : "FAILED", rc);
		return;
	}

	/* <prefix>/cmd/ota/begin "<sha256-hex> <size>" -- open a transfer into slot1. */
	if (leaf && strcmp(leaf, "cmd/ota/begin") == 0) {
		char     arg[96];
		uint8_t  sha[BOX_OTA_SHA_BYTES];
		uint32_t size = 0;

		dserv_msg_copy_cstr(&m, arg, sizeof arg);

		/* An OTA blocks the service loop in ~60 ms slices for the whole
		 * image (PORTING.md), so it must never overlap a trial. */
		if (box_obs_active()) {
			g_ota.state = BOX_OTA_DONE_FAIL;
			g_ota.err   = BOX_OTA_ERR_STATE;
			ota_pub_done();
			box_console_printf("cmd/ota/begin -> refused, in_obs\n");
			return;
		}
		ota_ain_yield();      /* begin erases the trailer -- that is flash work */
		if (strlen(arg) < 65 || box_ota_parse_sha(arg, sha) != 0) {
			g_ota.state = BOX_OTA_DONE_FAIL;
			g_ota.err   = BOX_OTA_ERR_SHA_INIT;
			ota_pub_done();
			box_console_printf("cmd/ota/begin -> bad sha argument\n");
			return;
		}
		size = (uint32_t) strtoul(arg + 64, NULL, 0);

		int rc = box_ota_flash_open();
		if (rc != 0) {
			g_ota.state = BOX_OTA_DONE_FAIL;
			g_ota.err   = BOX_OTA_ERR_FLASH;
			ota_pub_done();
			box_console_printf("cmd/ota/begin -> slot open failed (%d)\n", rc);
			return;
		}
		/* Start where MCUboot will LOOK, not at the slot's first byte --
		 * swap-using-offset reserves the first sector. box_ota's offsets are
		 * slot-relative, so shift the base and shrink the usable capacity to
		 * match, or a full-size image would run off the end. */
		g_ota_base = box_ota_flash_image_base();

		if (box_ota_begin(&g_ota, &g_ota_flash_ops,
				  box_ota_flash_size() - g_ota_base,
				  box_ota_flash_sector(), sha, size) != 0) {
			g_ota_active = 0;
			ota_pub_done();
			box_console_printf("cmd/ota/begin -> refused (%s)\n",
					   box_ota_err_str(g_ota.err));
			return;
		}
		/* Clear the trailer NOW rather than at arm time: it is an erase, and
		 * begin is already the point where we are allowed to touch flash
		 * (obs-gated above). Failing here is worth reporting but not fatal to
		 * the transfer -- the image is still worth staging, and arm will say so. */
		{
			int trc = box_ota_flash_clear_trailer();

			ota_pub_int("ota/trailer_rc", trc);
			if (trc != 0) {
				box_console_printf("cmd/ota/begin -> WARNING trailer clear failed (%d);"
						   " arm will fail until it is erased\n", trc);
			}
		}

		box_ota_flash_stats_reset();
		g_ota_size    = size;
		g_ota_ack_at  = BOX_OTA_ACK_EVERY;
		g_ota_active  = 1;
		ota_res_until = 0;

		ota_pub_str("ota/state", box_ota_state_str(g_ota.state));
		ota_pub_int("ota/progress", 0);
		ota_pub_int("ota/ack", 0);
		box_console_printf("cmd/ota/begin -> staging %u B into slot1 "
				   "(cap %u, sector %u)\n",
				   size, box_ota_flash_size(), box_ota_flash_sector());
		return;
	}

	/* <prefix>/cmd/ota/chunk -- one sequential span of image bytes.
	 *   [0..3] seq_off u32 LE   [4..7] crc32 u32 LE   [8..] data
	 * Strictly sequential: seq_off must equal the sink cursor. EVERY reject
	 * re-acks the current cursor rather than staying silent, so the host always
	 * has something to resume from and a lost frame costs one retry instead of
	 * a stalled transfer. Rejecting is therefore idempotent by construction. */
	if (leaf && strcmp(leaf, "cmd/ota/chunk") == 0) {
		uint32_t seq, crc;

		if (!g_ota_active) {
			return;                       /* stray frame, no transfer open */
		}
		/* A trial started mid-transfer: hold, do not abort. Re-acking the
		 * cursor lets the host retry and make progress once the obs ends,
		 * which is friendlier than throwing away a part-written image. */
		if (box_obs_active()) {
			ota_pub_int("ota/ack", (int32_t) g_ota.received);
			return;
		}
		/* Refreshed per chunk, which is what makes the hold last exactly as
		 * long as the transfer is actually progressing -- and no longer. */
		ota_ain_yield();
		if (m.datalen <= BOX_OTA_CHUNK_HDR) {
			ota_pub_int("ota/ack", (int32_t) g_ota.received);
			return;
		}

		const uint8_t *p = m.data;
		uint32_t n = m.datalen - BOX_OTA_CHUNK_HDR;

		memcpy(&seq, p, 4);
		memcpy(&crc, p + 4, 4);
		p += BOX_OTA_CHUNK_HDR;

		if (seq != g_ota.received || box_crc32(p, n) != crc) {
			ota_pub_int("ota/ack", (int32_t) g_ota.received);
			return;
		}
		if (box_ota_sink(&g_ota, p, n) != 0) {
			g_ota_active = 0;
			box_ota_finish(&g_ota, -1);
			ota_pub_done();
			box_console_printf("cmd/ota/chunk -> abort (%s) at %u\n",
					   box_ota_err_str(g_ota.err), g_ota.received);
			return;
		}
		if (g_ota.received >= g_ota_size) {          /* all bytes in -> verify */
			uint32_t e_max = 0, p_max = 0, e_n = 0, p_n = 0;

			g_ota_active = 0;
			box_ota_finish(&g_ota, 0);
			box_ota_flash_stats(&e_max, &p_max, &e_n, &p_n);
			ota_pub_done();
			box_console_printf("cmd/ota/chunk -> %s (%s), %u B, "
					   "erase_max %u us, prog_max %u us\n",
					   box_ota_state_str(g_ota.state),
					   box_ota_err_str(g_ota.err), g_ota.received,
					   e_max, p_max);
			return;
		}
		if (g_ota.received >= g_ota_ack_at) {
			g_ota_ack_at = g_ota.received + BOX_OTA_ACK_EVERY;
			ota_pub_int("ota/ack", (int32_t) g_ota.received);
			ota_pub_int("ota/progress", box_ota_progress_pct(&g_ota));
		}
		return;
	}

	/* <prefix>/cmd/ota/verify -- re-hash the image BY READING IT BACK OUT OF
	 * slot1, and compare against the sha from cmd/ota/begin.
	 *
	 * This is not redundant with the transfer's own sha. That one hashes bytes
	 * as they ARRIVE, so `ota/state=ok` means "the image crossed the link
	 * intact and no flash call reported an error" -- it says nothing about what
	 * is actually in the slot. Only a read-back does. It is the same distinction
	 * cmd/ota/flashtest draws by reading its pattern back, and the same lesson
	 * as the TDDR PLL bug: confirm the operation HAPPENED, do not infer it from
	 * a call that returned 0.
	 *
	 * Step 4 must run this before arming a TBYB trial: MCUboot will happily
	 * try to boot a slot we merely believe we wrote. */
	if (leaf && strcmp(leaf, "cmd/ota/verify") == 0) {
		ota_sha_t vs;
		uint8_t   buf[256], out[BOX_OTA_SHA_BYTES];
		uint32_t  off = 0, left = g_ota.expected_size;
		int       rc = 0, match = 0;

		if (box_obs_active()) {
			ota_pub_int("ota/flash_verify", -1);
			return;
		}
		/* A read-back of the whole slot is a long uninterrupted stretch of
		 * flash work on this very thread -- the same reason as the blast. */
		ota_ain_yield();
		if (left == 0 || box_ota_flash_open() != 0) {
			ota_pub_int("ota/flash_verify", -1);
			box_console_printf("cmd/ota/verify -> nothing staged\n");
			return;
		}

		uint32_t c0 = k_cycle_get_32();

		ota_sha_start(&vs);
		while (left) {
			uint32_t n = (left > sizeof buf) ? (uint32_t) sizeof buf : left;

			rc = box_ota_flash_read(off + g_ota_base, buf, n);
			if (rc != 0) {
				break;
			}
			ota_sha_update(&vs, buf, n);
			off  += n;
			left -= n;
		}
		ota_sha_finish(&vs, out);
		match = (rc == 0) &&
			memcmp(out, g_ota.expected_sha, BOX_OTA_SHA_BYTES) == 0;

		uint32_t ms = k_cyc_to_us_floor32(k_cycle_get_32() - c0) / 1000u;

		/* ...and the OTHER half of "is this bootable": a readable MCUboot header
		 * at the offset the bootloader will use. The sha answers "are these the
		 * right bytes"; this answers "are they in the right place", which is a
		 * separate question with its own way of going wrong (step 4). Reported
		 * as its own leaf so a host can tell the two failures apart. */
		char vver[16];
		int  vhrc = box_boot_image_ver(1, vver, sizeof vver);

		ota_pub_int("ota/hdr_ok", vhrc == 0 ? 1 : 0);
		ota_pub_str("ota/staged_ver", vhrc == 0 ? vver : "none");

		ota_pub_int("ota/flash_verify", match ? 1 : 0);
		ota_pub_int("ota/flash_ms", (int32_t) ms);
		ota_res_until = k_uptime_get() + 6000;
		box_console_printf("cmd/ota/verify -> slot1 sha %s (%u B read in %u ms, rc %d); "
				   "header %s\n",
				   match ? "MATCHES" : "DIFFERS", off, ms, rc,
				   vhrc == 0 ? vver : "UNREADABLE");
		return;
	}

#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
	/* <prefix>/cmd/ota/arm -- boot the staged image ONCE, on trial.
	 *
	 * boot_request_upgrade(BOOT_UPGRADE_TEST) marks slot1 to be swapped in for a
	 * SINGLE boot. If that boot never confirms, the next reset puts the old
	 * image back with no action from anyone -- which is the whole point: every
	 * failure path (bad image, wedge, power cut, failed self-test) converges on
	 * "the old firmware boots".
	 *
	 * PERMANENT IS DELIBERATELY NOT OFFERED. A permanent upgrade is a one-way
	 * door with no recovery if the new image cannot talk to dserv, and this box
	 * may be physically inaccessible. Confirmation must be EARNED by the running
	 * image, from cmd/ota/confirm below.
	 *
	 * Refuses unless the staged image was verified BY READ-BACK, not merely
	 * received: MCUboot will happily try to boot a slot we only believe we
	 * wrote, and a trial that bricks into a reboot loop is a rig visit. */
	if (leaf && strcmp(leaf, "cmd/ota/arm") == 0) {
		if (box_obs_active()) {
			ota_pub_str("ota/arm", "refused_in_obs");
			return;
		}
		if (g_ota.state != BOX_OTA_DONE_OK) {
			ota_pub_str("ota/arm", "refused_no_verified_image");
			box_console_printf("cmd/ota/arm -> refused: no verified image staged\n");
			return;
		}

		/* Last gate before the box disappears: does slot1 hold a readable
		 * MCUboot header WHERE THE BOOTLOADER WILL LOOK?
		 *
		 * This is not the sha check repeated. The sha proves the bytes we sent
		 * are the bytes in flash; this proves they are in the right PLACE.
		 * Step 4 shipped an image at slot offset 0 under swap-using-offset,
		 * which verified perfectly and then simply never booted -- MCUboot
		 * reads the header one sector further in and found nothing. A refusal
		 * here costs a re-push; the alternative cost a reboot, a swap, and a
		 * silent revert with no explanation anywhere. */
		char sver[16];
		int hrc = box_boot_image_ver(1, sver, sizeof sver);

		if (hrc != 0) {
			ota_pub_str("ota/arm", "refused_no_image_header");
			ota_pub_int("ota/arm_rc", hrc);
			box_console_printf("cmd/ota/arm -> refused: no MCUboot header in slot1 (%d) "
					   "-- image staged at the wrong offset?\n", hrc);
			return;
		}

		int rc = boot_request_upgrade(BOOT_UPGRADE_TEST);

		if (rc != 0) {
			ota_pub_str("ota/arm", "failed");
			ota_pub_int("ota/arm_rc", rc);
			box_console_printf("cmd/ota/arm -> boot_request_upgrade failed (%d)\n", rc);
			return;
		}
		/* Send this DIRECTLY, not via pub_enqueue. The queue is drained by the
		 * service loop, and the k_msleep below blocks that very loop -- so a
		 * queued "armed" is still sitting there when the reset lands and the
		 * host never sees the acknowledgement for the one command that makes
		 * the box disappear. Observed exactly that: dserv kept showing a stale
		 * "refused_no_verified_image" from an earlier attempt while the box was
		 * demonstrably armed and swapping. */
		{
			uint8_t fa2[DSERV_MSG_LEN];
			char nma[80];

			dserv_state_name(&cfg, nma, sizeof nma, "ota/arm");
			dserv_msg_string(fa2, nma, 0, "armed");
			box_uplink_send(fa2, DSERV_MSG_LEN);

			dserv_state_name(&cfg, nma, sizeof nma, "ota/arm_rc");
			dserv_msg_int(fa2, nma, 0, 0);
			box_uplink_send(fa2, DSERV_MSG_LEN);
		}

		/* Drop the breadcrumb LAST, so it records an arm that actually
		 * happened. From here the next boot can name its own outcome:
		 * trial -> revert if it runs and nobody keeps it, rejected if MCUboot
		 * declines to run it at all. */
		int brc = box_boot_note_arm(sver);

		if (brc != 0) {
			/* Not fatal -- MCUboot's trailer is what decides the boot, and it
			 * is already written. We just lose the ability to EXPLAIN the next
			 * boot, so say so rather than reverting silently later. */
			box_console_printf("cmd/ota/arm -> WARNING breadcrumb write failed (%d); "
					   "a rollback will not be reported\n", brc);
		}
		box_console_printf("cmd/ota/arm -> armed for ONE trial boot (slot1 v%s); rebooting\n",
				   sver);
		k_msleep(600);                 /* let the wire settle before the reset */
		sys_reboot(SYS_REBOOT_WARM);
		return;
	}

	/* <prefix>/cmd/ota/confirm -- keep the image we are RUNNING.
	 *
	 * Only meaningful on a trial boot. Until this lands, the next reset reverts.
	 * The host should send it only after the box has proven itself -- and the
	 * proof that matters is state/cmds_rx advancing, because that is the one
	 * thing a "publishing but deaf" image cannot fake: it means this very
	 * command arrived over the very path the box needs to remain useful. */
	if (leaf && strcmp(leaf, "cmd/ota/confirm") == 0) {
		int rc = boot_write_img_confirmed();

		if (rc == 0) {
			/* Close the breadcrumb, or the NEXT boot sees an armed record with
			 * a trial already seen and reports a revert that never happened.
			 * The record has to track the decision, not just the arming. */
			(void) box_boot_note_confirm();
		}
		ota_pub_str("ota/confirm", rc == 0 ? "confirmed" : "failed");
		ota_pub_int("ota/confirm_rc", rc);
		/* The box is no longer on the clock, and the update counted: say both
		 * now rather than at the next connect. These leaves are otherwise only
		 * published by the announce burst, so without this they keep reporting
		 * the state the box booted with -- a stale ota/updates=0 next to a
		 * fresh ota/confirm=confirmed, which is the same read-the-memory-not-
		 * the-reality trap the announce path exists to avoid. */
		ota_pub_int("ota/trial", box_boot_on_trial());
		ota_pub_int("ota/updates", box_boot_updates());
		ota_res_until = k_uptime_get() + 6000;
		box_console_printf("cmd/ota/confirm -> %s (%d)\n",
				   rc == 0 ? "CONFIRMED, image is permanent" : "FAILED", rc);
		return;
	}
#endif /* CONFIG_MCUBOOT_IMG_MANAGER */

	/* <prefix>/cmd/ota/abort -- drop a transfer in progress. */
	if (leaf && strcmp(leaf, "cmd/ota/abort") == 0) {
		if (g_ota_active) {
			g_ota_active = 0;
			box_ota_finish(&g_ota, -1);
		}
		ota_pub_done();
		box_console_printf("cmd/ota/abort -> %s\n", box_ota_state_str(g_ota.state));
		return;
	}
#endif /* BOX_HAVE_OTA_SLOT */

	if (dserv_msg_name_eq(&m, BOX_SYNC_DP)) {
		int obs = (int) dserv_msg_as_long(&m);
		uint64_t now_box = box_gpio_now_us();

		/* ANCHOR. Prefer the IRQ-latched TTL edge (jitter ~us) over frame
		 * arrival (100s of us of transport jitter): a hardware anchor takes
		 * the transport out of the error budget entirely, and only trusted
		 * (hw) anchors are allowed to teach the crystal rate. */
		uint64_t anchor_box = now_box;
		int hw = 0;

		if (sync_input_enabled(&cfg)) {
			uint64_t e = box_gpio_sync_edge_us(obs);   /* rising for obs=1 */

			if (e && now_box - e < SYNC_EDGE_WINDOW_US) {
				anchor_box = e;
				hw = 1;
			}
		}
		box_clock_sync(&boxclk, m.timestamp, anchor_box, hw);
		box_obs_set(obs);            /* keep the LEVEL, not just the edge */
		if (obs) {
			obs_begin_us = anchor_box;   /* epoch for box-scheduled events */
		}

		/* drive the obs-mirror output (LED / scope trace) */
		box_gpio_obs_mirror(&cfg, obs);

		/* publish the box's OWN live copy, so obs state is visible per-box in
		 * dserv without a scope -- honest, since it only updates when THIS box
		 * actually received the edge. */
		uint8_t of[DSERV_MSG_LEN];
		char onm[80];
		dserv_state_name(&cfg, onm, sizeof onm, "in_obs");
		dserv_msg_int(of, onm, m.timestamp, obs);
		box_uplink_send(of, DSERV_MSG_LEN);

		publish_sync(m.timestamp, anchor_box, boxclk.offset_us, hw,
			     hw ? (int64_t)(now_box - anchor_box) : -1);
		return;
	}

	gpio_cmd_t cmd;
	cfg_result_t r = dserv_dispatch(&cfg, &m, &cmd);
	if (r == CFG_GPIO && cmd.op == GPIO_OP_SCHED_PULSE) {
		/* do/<n>/at: pulse at beginobs + delta, width from the pin's
		 * configured pulse_us -- and post state/timer/<n> at the fire. */
		if (obs_begin_us == 0) {
			box_console_printf("sched: no beginobs yet, ignoring\n");
		} else {
			uint32_t w = cfg.do_pulse_us[cmd.pin] ? cfg.do_pulse_us[cmd.pin] : 1000;
			if (box_sched_arm(&cfg, cmd.pin, cmd.pin, w,
					  obs_begin_us + cmd.value) != 0) {
				box_console_printf("sched: table full\n");
			}
		}
	} else if (r == CFG_GPIO && cmd.op == GPIO_OP_SCHED_TIMER) {
		/* timer/<t>/at: notify-only at beginobs + delta */
		if (obs_begin_us == 0) {
			box_console_printf("sched: no beginobs yet, ignoring\n");
		} else if (box_sched_arm(&cfg, BOX_SCHED_NOTIFY_ONLY, cmd.pin, 0,
					 obs_begin_us + cmd.value) != 0) {
			box_console_printf("sched: table full\n");
		}
	} else if (r == CFG_GPIO && cmd.op != GPIO_OP_NONE) {
		box_gpio_exec(&cfg, &cmd);            /* immediate DO set/pulse */
		if (cmd.op == GPIO_OP_SET) {
			/* the pin has just moved -> stamp its actuation, not arrival */
			publish_do(cmd.pin, (uint8_t) (cmd.value ? 1 : 0),
				   event_stamp(box_gpio_now_us()));
		}
#if defined(BOX_HAVE_ADC)
	} else if (r == CFG_AIN || r == CFG_AIN_EN) {
		/* Tell the sampler its policy changed. Without this the running
		 * thread keeps its old channel mask forever: on 2026-07-28 a rescue
		 * that set `channels off` was accepted, persisted, and had NO effect
		 * on the live sampler, which carried on saturating the uplink until
		 * the box went silent. Config that applies only after a reboot is a
		 * config field that lies. */
		box_ain_apply();
#endif
	} else if (r == CFG_CONSOLE) {
		box_console_printf("console=%s -- save+reboot to apply\n",
		       dserv_console_str((uint8_t) dserv_cfg_console_mode(&cfg)));
	} else if (r == CFG_GROUP || r == CFG_LABEL || r == CFG_DESC) {
		/* group/label/desc change: reseed the chord machines from the
		 * pins' current levels, and re-announce so the edit reaches
		 * consumers without waiting for a reconnect. */
		if (r == CFG_GROUP) {
			groups_resync();
		}
		box_announce_manifest(&cfg);
	} else if (r == CFG_PIN_MODE || r == CFG_OBS_PIN || r == CFG_SYNC_PIN ||
		   r == CFG_ACTIVE_LOW || r == CFG_DEBOUNCE) {
		/* ANY change to the pin map has to be pushed to the hardware. Notably
		 * obs/sync pins are claimed (as output / edge-latched input) only by
		 * apply_config -- without this, `config/obs/pin` set the config but
		 * left the pin unclaimed, so the mirror drove nothing. The console CLI
		 * path already re-applied (CLI_PIN); the datapoint path did not. */
		box_gpio_apply_config(&cfg);
#if defined(BOX_HAVE_ADC)
		/* ...AND the sampler, for the same reason one line up. `pin N mode
		 * ain` moves a pad between GPIO and the converter, so it changes
		 * box_adc_active_mask() -- and the sweep mask is derived from that.
		 * The console path (CLI_PIN) already did this; the datapoint path did
		 * not, so `config/pin/14/mode = ain` over dserv re-muxed the pad and
		 * left the sampler's mask stale: a channel now wired to the ADC and
		 * never swept, or worse, a pad handed back to GPIO while the mask
		 * still asked for it. Same defect the comment above describes for
		 * obs/sync pins, one subsystem over. */
		box_ain_apply();
#endif
		groups_resync();          /* DI levels may have changed meaning */
		box_announce_manifest(&cfg);   /* pins/in|out, obs_pin, sync_pin moved */
	} else if (r == CFG_ANNOUNCE) {
		/* Everything, not just the manifest: identity and the OTA counters
		 * live in the burst, and a UI asking "what are you now" wants the
		 * same picture it gets on connect rather than a subset that happens
		 * to cover today's caller. */
		box_announce_burst(&cfg, groups);
		box_console_printf("cmd/announce -> full re-announce\n");
	} else if (r == CFG_SAVE) {
#if defined(BOX_HAVE_PERSIST)
		if (box_obs_active()) {      /* never program flash mid-trial */
			box_obs_defer(BOX_DEFER_SAVE);
			box_console_printf("cmd/save -> deferred to end of obs\n");
			return;
		}
		/* Persist the whole config blob so it survives reboot/power-cycle. */
		uint8_t blob[BOX_PERSIST_BLOB_MAX];
		uint32_t n = box_persist_serialize(&cfg, blob, sizeof blob);
		int rc = box_flash_save(blob, n);
		box_console_printf("cmd/save -> %s (%u bytes)\n", rc == 0 ? "ok" : "FAILED", n);
#else
		box_console_printf("cmd/save -> no persistence on this board\n");
#endif
	} else if (r == CFG_REBOOT) {
		/* Warm reset: the firmware restarts. Portable on every board. NOTE this
		 * does NOT enter the bootloader -- see CFG_BOOTSEL below. */
		box_console_printf("cmd/reboot -> warm reset\n");
		k_msleep(100);                        /* let the console drain */
		sys_reboot(SYS_REBOOT_WARM);
	} else if (r == CFG_BOOTSEL) {
		/* Program-mode entry is board-specific and NOT universally reachable:
		 *   RP2350   reset_usb_boot() -- trivial (the Pico's `bootsel`)
		 *   Teensy   bootloader is a separate chip watching the Program button;
		 *            Teensyduino's handshake is not exposed by Zephyr
		 *   RW612    moot -- MCUboot + mcumgr does DFU over the live link
		 * Report honestly rather than silently doing nothing. */
		box_console_printf("cmd/bootsel -> not supported on this board; "
		       "press the Program button to enter the bootloader\n");
	}
}

static cfg_result_t feed(const uint8_t *frame, gpio_cmd_t *cmd)
{
	dserv_msg_t m;
	if (dserv_msg_parse(frame, &m) != 0) {
		return CFG_NONE;
	}
	return dserv_dispatch(&cfg, &m, cmd);
}

int main(void)
{
	uint8_t f[DSERV_MSG_LEN];
	gpio_cmd_t cmd;
	cfg_result_t r;

	/* NOTE: NVS mount is deferred until AFTER the console is up (below) so a
	 * flash fault/hang is visible rather than silent -- an XIP-from-flash erase
	 * on the RT1062 is a known hazard. Demo pins for now; a loaded config
	 * re-applies after the mount. */
	int cfg_loaded = 0;
	cfg.pin_mode[LED_PIN] = 1;   /* demo output   */
	cfg.pin_mode[BTN_PIN] = 3;   /* demo in_pullup */

	/* Bring the platform up BEFORE any printk. On boards whose console is the
	 * box's own USB CDC (see the board overlays), output produced before the
	 * device enumerates is lost -- so init GPIO (so inbound commands can act
	 * immediately), then the uplink, then give the host a moment to open the
	 * console port before we start talking. */
	if (box_gpio_init() != 0) {
		/* nothing to print to yet; the LED demo below simply won't run */
	}
	box_sched_init();
	box_gpio_apply_config(&cfg);

	/* Load the persisted config BEFORE the uplink AND before the console.
	 *
	 * Before the UPLINK because transport_mode is persisted policy that the
	 * uplink acts on at init: `mode eth` suppresses the USB data pipe entirely
	 * (box_usbd_start), and enumeration happens inside box_uplink_init. Load it
	 * afterwards and that decision is made against the FACTORY DEFAULT -- the box
	 * enumerates a data pipe a declared-Ethernet box was told not to have, with
	 * no symptom other than the setting appearing to do nothing.
	 *
	 * Before the CONSOLE because console_mode (v22) decides WHICH device the
	 * console binds to, so the saved choice has to be known first.
	 *
	 * A flash fault here has no box-console to report on, but Zephyr's own
	 * printk/LOG is on the board UART (chosen zephyr,console) and still works --
	 * the boot-log channel PORTING.md documents. */
	/* Board default FIRST, so the persisted blob below always overrides it and
	 * an explicit `console cdc|uart` + save is never second-guessed. This is
	 * what a fresh box -- or one after `factory` -- comes up on. */
	cfg.console_mode = BOX_DEFAULT_CONSOLE_MODE;

#if defined(BOX_HAVE_PERSIST)
	int frc = box_flash_init();
	if (frc != 0) {
		box_persist_mount_err = frc;   /* surfaced in the boot banner */
	}
	if (frc == 0) {
		uint8_t lb[BOX_PERSIST_BLOB_MAX];
		int ln = box_flash_load(lb, sizeof lb);

		cfg_loaded = (ln > 0 && box_persist_deserialize(lb, (uint32_t) ln, &cfg) == 0);
		if (cfg_loaded) {
			box_gpio_apply_config(&cfg);   /* apply the loaded pin map/name */
			groups_resync();               /* seed chords from real pin state */
		}
	}
#endif

	/* Why we booted, and which image this is. AFTER the store mounts, because
	 * the OTA breadcrumb lives there; BEFORE anything announces, because
	 * box_announce reports the latch this produces. Also clears the reset-cause
	 * register, so nothing else may read it directly. */
	box_boot_init();

#if defined(BOX_HAVE_ADC)
	/* Same synthesizer the RP2350 runs: with the ADC enabled and no group
	 * defined, give ch0/ch1 an on-change "joystick" group so a freshly fitted
	 * Click does something visible instead of nothing. No-op once any group
	 * exists, and no-op while ain_en is clear. */
	dserv_cfg_ain_default(&cfg);

	/* After the persisted config (rate/groups come from it) and before the
	 * service loop, so the first blocks are already queued when the uplink
	 * comes up. Returns -ENODEV on a box with no Click fitted, which is the
	 * ordinary case and not worth reporting as a failure. */
	(void) box_ain_init(&cfg);

	/* Hold from BEFORE the sampler's first tick, so analog never runs even
	 * momentarily on an undisciplined clock. The service loop re-arms this via
	 * ain_boot_gate() until PTP locks; if we somehow never reach the loop, the
	 * deadline expires and analog starts by itself. */
	box_ain_hold(BOX_AIN_BOOT_HOLD_STEP_MS);

	/* Teach the CLI what the converter actually has, so `ain group G channels`
	 * validates against the fitted count instead of the MCP3204's four. Must
	 * follow box_ain_init(), which is what runs box_adc_init(). A board with no
	 * ADC reports 0 and the setter keeps the safe default. */
	box_console_set_ain_channels((int) box_adc_channels());
#endif

	/* Pins this board refuses -- unmapped indices plus anything in
	 * `box-reserved` (the PHY's MDIO pin, and any pad handed to the ADC). */
	box_console_set_reserved_pins(box_gpio_reserved_mask());


#if defined(CONFIG_PTP_CLOCK)
	for (int i = 0; i < BOX_NPINS; i++) {
		k_timer_init(&abs_timer[i], abs_fire, NULL);
	}
#endif
	box_uplink_init(&cfg);       /* USB (and Ethernet where present) up */

	box_console_init(&cfg);      /* binds CDC or console UART per console_mode */
	dserv_framer_reset(&rx_framer);
	k_msleep(2000);              /* let the host enumerate + open the console */

	box_console_printf("\n=== extio core smoke test (Zephyr %s) ===\n", KERNEL_VERSION_STRING);

	/* config datapoint: set pin 5 to output */
	dserv_msg_int(f, "extio/box/config/pin/5/mode", 0, 1);
	r = feed(f, &cmd);
	box_console_printf("apply config/pin/5/mode=out    -> %-9s pin_mode[5]=%u\n",
	       dserv_cfg_result_str(r), cfg.pin_mode[5]);

	/* No demo dserv target: leaving dserv_ip unset keeps eth from claiming the
	 * uplink (it needs a configured target), so a bench box stays on USB and
	 * reachable. A real box gets its target from persisted config / the console
	 * CLI (a later block). */

	/* transient command: box-timed pulse on pin 6 -> a gpio_cmd for the platform */
	dserv_msg_int(f, "extio/box/cmd/do/6/pulse_us", 0, 500);
	r = feed(f, &cmd);
	box_console_printf("apply cmd/do/6/pulse_us=500    -> %-9s op=%d pin=%u value=%u\n",
	       dserv_cfg_result_str(r), cmd.op, cmd.pin, cmd.value);

	/* persistence round-trip (the flash write itself is platform; the codec is core) */
	uint8_t blob[BOX_PERSIST_BLOB_MAX];
	uint32_t n = box_persist_serialize(&cfg, blob, sizeof blob);
	box_config_t restored = {0};
	int ok = box_persist_deserialize(blob, n, &restored);
	box_console_printf("persist round-trip             -> %-9s %u bytes, applied_count=%u\n",
	       ok == 0 ? "ok" : "FAIL", n, restored.applied_count);

#if defined(BOX_HAVE_PERSIST)
	/* (the store was mounted + loaded above, before the console bound) */
	box_console_printf("persist store                  -> config %s\n",
	       cfg_loaded ? "LOADED from flash" : "fresh (defaults)");
#else
	box_console_printf("persist store                  -> none on this board\n");
#endif

	/* the box's datapoint identity */
	char pfx[64];
	dserv_cfg_prefix(&cfg, pfx, sizeof pfx);
	box_console_printf("datapoint prefix               -> %s  (dserv port %u)\n",
	       pfx, dserv_cfg_port(&cfg));

	box_console_printf("=== codec smoke test done ===\n\n");

	/* ---- block #3: GPIO (already initialised above, before the console) ---- */
	box_console_printf("gpio: pin %d=out (LED), pin %d=in_pullup\n", LED_PIN, BTN_PIN);

	/* Boot heartbeat: three hardware-timed LED pulses (the hardware counter
	 * drops each falling edge, not software). */
	gpio_cmd_t pulse = { .op = GPIO_OP_PULSE, .pin = LED_PIN, .value = 120000 };
	for (int i = 0; i < 3; i++) {
		box_gpio_exec(&cfg, &pulse);
		k_msleep(250);
	}

#if defined(CONFIG_NETWORKING)
	/* one-shot status: DHCP lease (if eth came up) + the PTP hardware clock */
	uint8_t ip[4];
	if (box_net_eth_wait_ip(ip, 5000)) {
		box_console_printf("eth DHCP IPv4: %u.%u.%u.%u  link=%d\n",
		       ip[0], ip[1], ip[2], ip[3], box_net_eth_link());
	} else {
		box_console_printf("eth: no lease (link=%d)\n", box_net_eth_link());
	}
#if defined(BOX_HAVE_PERSIST)
	/* Say so LOUDLY when persistence is dead: every `save` will fail and every
	 * reboot silently reverts to defaults, which reads as a firmware bug. */
	if (box_persist_mount_err) {
		/* Everything needed to diagnose it, in one line: a bare "FAILED" sent
		 * this hunt into the FlexSPI driver for hours when the answer was a
		 * wrong partition offset. See PORTING.md 2026-07-25. */
		int pirc = 0; uint32_t pisz = 0, poff = 0, ss = 0, sc = 0;
		box_flash_debug(&pirc, &pisz, &poff);
		box_flash_geometry(&ss, &sc);
		box_console_printf("persist: NVS MOUNT FAILED err=%d -- `save` will fail, "
		                   "config will NOT survive reboot\n", box_persist_mount_err);
		box_console_printf("persist:   page_info(off=0x%x) rc=%d size=%u; "
		                   "sectors %ux%u\n",
		                   (unsigned) poff, pirc, (unsigned) pisz,
		                   (unsigned) sc, (unsigned) ss);
	}
#endif
	box_console_printf("PTP hw clock: ready=%d  now=%llu ns\n",
	       (int) box_ptp_ready(), (unsigned long long) box_ptp_now_ns());
#else
	box_console_printf("no Ethernet on this board -- USB-only uplink\n");
#endif
	box_console_printf("console: %s (config/console cdc|uart; save+reboot)\n",
	       dserv_console_str((uint8_t) dserv_cfg_console_mode(&cfg)));
	box_console_printf("active uplink: %s\n", box_uplink_active_name());
#if defined(BOX_HAVE_ADC)
	if (box_adc_ready()) {
		box_console_printf("analog: %s, %u ch, %u-bit, %u mV fs; %d group(s) at %d Hz base\n",
		       box_adc_name(), box_adc_channels(), box_adc_bits(), box_adc_vref_mv(),
		       dserv_ain_active_count(&cfg), dserv_cfg_ain_rate(&cfg));
	} else {
		box_console_printf("analog: no ADC fitted\n");
	}
#endif
	/* Why we are running, in the box's own words. The raw mask rides along
	 * because the word collapses it (both watchdog and software can be set),
	 * and 0x0 is itself informative on the RW612 -- that SoC has no power-on
	 * bit, so a clean cold start sets nothing. */
	box_console_printf("boot: %s (reset cause 0x%02x)\n",
	       box_boot_reason(), (unsigned) box_boot_reset_cause());
#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
	/* Loud on purpose: a trial boot that nobody confirms REVERTS on the next
	 * reset, and finding that out from a mysterious version rollback later is
	 * far worse than one line here. */
	box_console_printf("image: v%s, %s\n", box_boot_img_ver(),
	       box_boot_on_trial()
	       ? "ON TRIAL -- send cmd/ota/confirm or the next reset REVERTS"
	       : "confirmed");
	if (strcmp(box_boot_reason(), "revert") == 0) {
		box_console_printf("image: ROLLED BACK from v%s -- the trial image was never "
		       "confirmed (%d revert(s) lifetime)\n",
		       box_boot_last_arm_ver(), box_boot_reverts());
	} else if (strcmp(box_boot_reason(), "rejected") == 0) {
		box_console_printf("image: MCUboot REFUSED v%s and it never ran -- bad signature, "
		       "bad header, or staged at the wrong offset (%d rejection(s) lifetime)\n",
		       box_boot_last_arm_ver(), box_boot_rejects());
	}
#endif

#if defined(CONFIG_BT)
	/* ---- block #6 (ingress): multi-peripheral BLE central ----
	 *
	 * GATED on the persisted flag, matching the RP2350's stated contract
	 * ("ENABLE CONTRACT: persisted cfg->ble_en, CLI `ble enable 1`, default
	 * OFF" -- wiznet-io/pico/box_ble_central.h). This port carried the config
	 * field over but initialised unconditionally, so `show` reported ble.en=0
	 * while the radio was up and scanning -- a config field that did nothing,
	 * which is the same "field that lies" class as net.ip and sync/source.
	 * A wired-only box should not be running a radio it was told to leave off. */
	if (!cfg.ble_en) {
		box_console_printf("BLE disabled (ble enable 1 -- live, `save` to persist)\n\n");
	} else if (box_ble_init() == 0) {
		box_console_printf("BLE central up; scanning for d5e7000x peripherals (max %d)\n\n",
		       CONFIG_BT_MAX_CONN);
	} else {
		box_console_printf("BLE init failed (continuing wired-only)\n\n");
	}
#else
	box_console_printf("no radio on this board -- BLE ingress disabled\n\n");
#endif

	/* ---- converged box service loop (blocks #2-6 together) ----
	 * arbitrate the uplink; inbound frames -> dispatch -> GPIO; local DI, BLE
	 * ingress, and a 1 Hz watchdog -> whichever uplink is active. */
	uint8_t rx[256];
	int watchdog = 0;
	uint32_t loop_last_us = 0, loop_max_us = 0, loop_t0 = 0;
	uint32_t wd_skipped = 0;          /* 1 Hz beats skipped after a loop stall */
	uint32_t disp_last_us = 0, disp_max_us = 0;
	int64_t next_wd = k_uptime_get() + 1000;
	char name[80];

	while (1) {
		box_uplink_service(&cfg);         /* carrier/strap selection + (re)connect */
		box_console_service(&cfg);        /* two-way CLI (non-blocking, bounded) */

		/* A couple of queued telemetry frames per pass. Bounded ON PURPOSE:
		 * the point is that no single pass can be held hostage by the backlog,
		 * so command latency stays flat instead of spiking once a second. */
		pub_drain(pubq_per_pass);

#if defined(BOX_HAVE_ADC)
		/* Keep analog held until the 1588 clock is disciplined. Re-arms a
		 * short deadline each pass rather than latching a flag, so the hold
		 * dies with the loop instead of outliving it. */
		ain_boot_gate();

		/* Analog blocks, built on the sampling thread and sent from HERE:
		 * exactly one thread ever writes the uplink. Bounded per pass for the
		 * same reason as pub_drain -- a burst of blocks must not hold the loop
		 * hostage and spike command latency. */
		for (int ai = 0; ai < 2; ai++) {
			ain_block_t blk;
			uint8_t payload[12 + AIN_BLOCK_MAX * 2];
			uint8_t f[DSERV_MSG_LEN];
			char leaf[BOX_LABEL_MAX + 8], nm[80];

			if (!box_ain_pop(&blk)) {
				break;
			}
			int plen = ain_block_payload(&blk, payload);

			dserv_ain_group_leaf(&cfg, blk.gidx, leaf, sizeof leaf);
			dserv_state_name(&cfg, nm, sizeof nm, leaf);
			/* The block's own t0 is the frame timestamp -- sample k of column
			 * c is at t0 + k*interval_us, so the host reshapes from the block
			 * alone. Publish time is NOT the sample time and must not be
			 * substituted here. */
			/* > 0, NOT == 0: dserv_msg_build returns the FRAME LENGTH on
			 * success and -1 only when name+payload overflow the 109-byte
			 * payload. Checking for 0 silently publishes nothing, which on
			 * the bench is indistinguishable from a dead ADC. */
			/* event_stamp(), NOT the raw box time. blk.t0_us is the box's
			 * local monotonic clock; every other event this box publishes
			 * goes through box_clock_stamp() to reach dserv's timeline, and
			 * analog was the one path that did not -- the RP2350 has always
			 * done it (wizchip_dserv_config.c: event_stamp(b->t0_us)) and the
			 * Zephyr port dropped it.
			 *
			 * The failure is silent and plausible, which is why it survived:
			 * the samples arrive, decode cleanly, and have exactly the right
			 * SPACING -- lib/extio-1.0.tm reconstructs row k at (frame
			 * timestamp) + k*interval_us and applies no correction -- so they
			 * are simply placed at 1970 plus a few seconds while DI edges from
			 * the same box sit correctly on the dserv timeline. A stream that
			 * is internally consistent and absolutely wrong.
			 *
			 * event_stamp() also returns 0 before the clock is aligned, which
			 * tells dserv to arrival-stamp: unaligned analog is then merely
			 * imprecise instead of decades adrift. */
			if (dserv_msg_bytes(f, nm, event_stamp(blk.t0_us), payload,
					    (uint32_t) plen) > 0) {
				pub_enqueue(f);
			}
		}
#endif

		int n = box_uplink_poll(rx, sizeof rx);
		if (n == BOX_NET_RESET) {
			dserv_framer_reset(&rx_framer);
			/* A host just opened the pipe. dserv only learns what it is
			 * told while listening, so describe the box now. */
			box_announce_burst(&cfg, groups);

			/* ...and CORRECT state/sync/source, which the announce burst
			 * does not carry.
			 *
			 * dserv RETAINS datapoints, and nothing else publishes this leaf
			 * until an anchor arrives -- so a box that reboots leaves the old
			 * "ptp" standing and reads as synced when it is not. That is not
			 * cosmetic: rig_check step 5 passes on the stale value while the
			 * box refuses every at_abs, which is exactly what happened on
			 * 2026-07-27 (step 5 PASS, step 7 "unsynced", and the PASS was the
			 * one lying). An anchor never survives a box reboot.
			 *
			 * CONDITIONAL on purpose. This fires on any dserv (re)connect, and
			 * a dserv RESTART leaves the box up with its anchor intact -- so
			 * publishing "none" unconditionally would destroy a good value to
			 * fix a stale one. Report what is actually true. */
			/* ota/trial and the rest of the bootloader state now ride in the
			 * announce burst above (box_announce.c, announce_ident) alongside
			 * state/boot, so that "which image am I and how did I get here" is
			 * answered by one writer in one place. */

#if defined(CONFIG_PTP_CLOCK)
			{
				uint8_t f0[DSERV_MSG_LEN];
				char nm0[80];

				dserv_state_name(&cfg, nm0, sizeof nm0, "sync/source");
				dserv_msg_string(f0, nm0, 0,
						 ptp_offset_valid ? "ptp" : "none");
				pub_enqueue(f0);
			}
#endif
		} else if (n > 0) {
			/* Cost of turning received bytes into an executed command
			 * (framer + dispatch + GPIO write). Completes the inbound
			 * split alongside wake_us / recv_us in box_net_eth.c. */
			uint32_t d0 = k_cycle_get_32();
			dserv_framer_feed(&rx_framer, rx, (uint32_t) n, on_usb_frame, NULL);
			uint32_t dd = k_cyc_to_us_floor32(k_cycle_get_32() - d0);
			disp_last_us = dd;
			if (dd > disp_max_us) {
				disp_max_us = dd;
			}
		}

#if defined(CONFIG_PTP_CLOCK)
		/* Publishes owed by scheduled fires (see abs_fire): done HERE, off the
		 * timer callback, so a blocking send can never sit between one pin's
		 * edge and another's. The stamp is abs_target_us[], the intended time,
		 * so nothing about the reported instant depends on when this runs. */
		if (abs_pub_pending) {
			uint32_t due = abs_pub_pending;
			abs_pub_pending = 0;
			for (int p = 0; p < BOX_NPINS; p++) {
				if (due & (1u << p)) {
					publish_do((uint8_t) p, 1, abs_target_us[p]);
				}
			}
		}
#endif

		box_di_event_t ev;
		while (box_gpio_poll_di(&cfg, &ev)) {
			uint8_t f[DSERV_MSG_LEN];
			char leaf[24];
			/* publish the LOGICAL level, so `pin N active_low 1` means
			 * something on the wire (box_gpio reports raw). */
			int lvl = di_logical(&cfg, ev.pin, ev.level);

			/* Feed every configured chord group. A member of a `quiet`
			 * group is reported ONLY as part of its settled chord -- the
			 * group replaces the per-pin event rather than doubling it. */
			int quiet = 0;
			for (int g = 0; g < BOX_NGROUPS; g++) {
				if (cfg.group_pins[g] &&
				    group_feed(&groups[g], &cfg, g, ev.pin, lvl, ev.t_us) &&
				    cfg.group_quiet[g]) {
					quiet = 1;
				}
			}
			if (quiet) {
				continue;
			}
			snprintf(leaf, sizeof leaf, "di/%u", ev.pin);
			dserv_state_name(&cfg, name, sizeof name, leaf);   /* extio/<name>/state/di/<pin> */
			dserv_msg_int(f, name, event_stamp(ev.t_us), lvl);
			box_uplink_send(f, DSERV_MSG_LEN);
		}

		/* Settle windows expire between edges, so poll them every pass --
		 * a chord closes settle_ms after its LAST member edge, and is stamped
		 * at the FIRST (the true movement onset). */
		{
			group_out_t go[2];
			uint64_t gnow = box_gpio_now_us();

			for (int g = 0; g < BOX_NGROUPS; g++) {
				if (!cfg.group_pins[g]) {
					continue;
				}
				int gn = group_poll(&groups[g], &cfg, g, gnow, go);

				for (int k = 0; k < gn; k++) {
					publish_group(g, go[k].bits, go[k].t_us);
				}
			}
		}

		/* Scheduled events that fired (timer ISR already drove the pulse):
		 * post state/timer/<tid>, stamped at the INTENDED fire instant --
		 * not at this drain -- so host-side timing analysis sees the truth. */
		{
			box_sched_fired_t sf;

			while (box_sched_poll(&sf)) {
				uint8_t f[DSERV_MSG_LEN];
				char leaf[20];

				snprintf(leaf, sizeof leaf, "timer/%u", sf.tid);
				dserv_state_name(&cfg, name, sizeof name, leaf);
				dserv_msg_int(f, name, event_stamp(sf.fire_us), 1);
				box_uplink_send(f, DSERV_MSG_LEN);
			}
		}

#if defined(CONFIG_BT)
		/* BLE ingress: each peripheral's frame is already source-stamped
		 * (extio/<client>/...); relay it out the active uplink verbatim. */
		uint8_t bframe[DSERV_MSG_LEN];
		while (box_ble_poll(bframe)) {
			box_uplink_send(bframe, DSERV_MSG_LEN);
		}
#endif

		int64_t wd_now = k_uptime_get();

		if (wd_now >= next_wd) {
			/* Normal case is `next_wd += 1000` -- phase-preserving, so the
			 * heartbeat does not drift. The extra clause is the CATCH-UP
			 * policy, and it is not cosmetic.
			 *
			 * Anything that blocks the service loop for seconds (a flash
			 * burst does: 512 kB into slot1 = ~8 s, OTA step 2) leaves
			 * next_wd many periods in the past. `+= 1000` alone then fires
			 * the gate once per MISSED period, i.e. N full ~28-frame status
			 * bursts in N consecutive ~1 ms passes -- hundreds of frames into
			 * a 40-deep queue. Measured: dbg/pubq_dropped 128 -> 358 across
			 * two bursts. So a stall became a telemetry OUTAGE on top of the
			 * stall, and the frames dropped were preferentially the
			 * single-shot ones published at the end of the stalling operation
			 * -- which is precisely how an OTA result reads as "never ran".
			 *
			 * Emit ONE burst and skip the rest. The missed beats are still
			 * ADDED to the count, so state/watchdog keeps meaning
			 * seconds-since-boot rather than becoming a count of beats that
			 * happened to be emitted -- rig_check and any historical reading
			 * are unaffected. And the gap is now published explicitly as
			 * dbg/wd_skipped instead of only being inferable from a jump in
			 * the watchdog value, so a stall is easier to see, not harder. */
			next_wd += 1000;
			if (next_wd <= wd_now) {
				uint32_t miss = (uint32_t)((wd_now - next_wd) / 1000) + 1u;

				watchdog   += (int) miss;
				wd_skipped += miss;
				next_wd     = wd_now + 1000;
			}

			uint8_t f[DSERV_MSG_LEN];

			/* WATCHDOG GETS ITS OWN BUFFER, AND IS ENQUEUED EXACTLY ONCE.
			 *
			 * It used to be built into the shared scratch `f` and rely on a
			 * trailing pub_enqueue(f) far below to actually send it -- with each
			 * intervening sub-block "borrowing" f and then RESTORING the watchdog
			 * frame into it on the way out. That idiom published state/watchdog
			 * TWICE PER TICK, with the same value, ~20-40 ms apart (observed on a
			 * clean subscriber 2026-07-30, David spotted it): once at the enqueue
			 * that used to sit here, and again at the trailing one. One of the two
			 * restores was also dead code -- cmds_rx overwrote f before any
			 * enqueue could see it.
			 *
			 * Worse, the trailing pub_enqueue(f) sat AFTER an #endif, so on a board
			 * where that block compiles out it published whatever happened to be
			 * in f -- a duplicate of some unrelated datapoint. That is the whole
			 * problem with a shared scratch buffer: correctness depends on the
			 * distance between a build and its enqueue, across #ifdefs, and
			 * nothing checks it.
			 *
			 * A dedicated buffer makes the watchdog immune to whatever the rest of
			 * the block does with f. An audit of all 33 enqueues in this block
			 * found watchdog was the ONLY datapoint affected -- every other one is
			 * build-then-enqueue in order -- so this is the minimum change that
			 * removes the class, not just the instance. */
			/* One decision per pass, not per datapoint: a refresh deadline
			 * crossed mid-block would otherwise split the pass into refreshed
			 * and non-refreshed halves. */
			pub_force = (k_uptime_get() >= pub_refresh_at);
			if (pub_force) {
				pub_refresh_at = k_uptime_get() + PUB_REFRESH_S * 1000;
			}
			{
				uint8_t wf[DSERV_MSG_LEN];
				char wn[80];

				dserv_state_name(&cfg, wn, sizeof wn, "watchdog");
				dserv_msg_int(wf, wn, 0, watchdog++);
				pub_enqueue(wf);
			}
			{
				/* USB caller cost + TX-ring drops. Published on EVERY
				 * board: the comparison against the Ethernet numbers is
				 * what turns "how many datapoints can I push" into a
				 * budget rather than a guess. */
				uint32_t ul = 0, um = 0, ud = 0;
				box_net_usb_send_stats(&ul, &um, &ud);
				pub_periodic("dbg/usb_send_us",     ul);
				pub_periodic("dbg/usb_send_max_us", um);
				pub_periodic("dbg/usb_drops",       ud);
#if defined(BOX_HAVE_ADC)
				/* Analog health, published because its absence cost a box.
				 * box3 went silent on 2026-07-28 and there was NOTHING to
				 * look at -- the sampler had stats and published none of
				 * them, so a runaway publish rate was invisible from the
				 * host. `blocks` climbing fast, or any `throttled`, is that
				 * failure in one glance. Only sent when an ADC is fitted. */
				if (box_adc_ready()) {
					uint32_t asw = 0, abl = 0, adr = 0, alt = 0, ath = 0, amx = 0, an = 0;
					uint32_t asp = 0, arm = 0;
					box_ain_stats(&asw, &abl, &adr, &alt, &ath);
					box_adc_stats(&amx, &an);
					box_adc_pm_stats(&asp, &arm);
					struct { const char *leaf; uint32_t v; } as[] = {
						{ "ain/dbg/sweeps",    asw },
						{ "ain/dbg/blocks",    abl },
						{ "ain/dbg/dropped",   adr },
						{ "ain/dbg/late",      alt },
						{ "ain/dbg/throttled", ath },
						{ "ain/dbg/sweep_max_us", amx },
						{ "ain/dbg/chans",     box_adc_channels() },
						/* WHAT THE BOX IS DOING, next to what it was told.
						 * `running` 0 with ain_en 1 is now a legible state
						 * (held for an OTA, no group defined, or no pin in
						 * ain mode) rather than a silent dead sampler --
						 * which is the state box3 was in with nothing to
						 * look at. `powered` distinguishes "we suspended
						 * the converter" from "we merely stopped asking":
						 * on a board with no PM hook the first stays 1. */
						{ "ain/dbg/running",   (uint32_t) box_ain_running() },
						{ "ain/dbg/powered",   (uint32_t) box_adc_powered() },
						{ "ain/dbg/holds",     box_ain_holds() },
						{ "ain/dbg/suspends",  asp },
						{ "ain/dbg/resumes",   arm },
					};
					for (unsigned ai2 = 0; ai2 < ARRAY_SIZE(as); ai2++) {
						pub_periodic(as[ai2].leaf, as[ai2].v);
					}
				}
#endif
			}

			/* Deliberately alongside watchdog: watchdog proves the UPLINK
			 * is alive, cmds_rx proves the DOWNLINK is. Together they make
			 * "publishing but deaf" a one-glance diagnosis -- watchdog
			 * climbing while cmds_rx sits frozen -- instead of the hours it
			 * has cost twice, hiding behind a box whose every status field
			 * read healthy. */
			pub_periodic("cmds_rx", (uint32_t) cmds_rx);

			/* How many frames change-detection SAVED. Sent only on the refresh
			 * tick, because it is a counter that moves every second and would
			 * otherwise reintroduce exactly the per-second frame this whole
			 * mechanism exists to remove -- an instrument that consumes what it
			 * measures. Divide by the refresh interval for frames/s avoided. */
			if (pub_force) {
				pub_periodic("dbg/pub_suppressed", pub_suppressed);
			}

			/* Telemetry the queue itself dropped. Non-zero means the burst is
			 * outrunning the drain. Published rather than left silent: a
			 * queue that quietly discards data is exactly the failure shape
			 * this project keeps getting caught by. */
			dserv_state_name(&cfg, name, sizeof name, "dbg/pubq_dropped");
			dserv_msg_int(f, name, 0, (int32_t) pubq_dropped);
			pub_enqueue(f);

			/* Cumulative 1 Hz beats the loop was too stalled to emit. Nonzero
			 * means something held the loop for >1 s -- an OTA burst does this
			 * legitimately; anything else wants explaining. */
			dserv_state_name(&cfg, name, sizeof name, "dbg/wd_skipped");
			dserv_msg_int(f, name, 0, (int32_t) wd_skipped);
			pub_enqueue(f);

#if defined(BOX_HAVE_OTA_SLOT)
			/* Re-announce the last flashtest result for a few ticks --
			 * see the ota_dbg_* declarations for why once is not enough. */
			if (ota_res_until && k_uptime_get() < ota_res_until) {
				ota_pub_str("ota/state",  box_ota_state_str(g_ota.state));
				ota_pub_str("ota/result", box_ota_err_str(g_ota.err));
				ota_pub_int("ota/progress", box_ota_progress_pct(&g_ota));
				ota_pub_int("ota/ack", (int32_t) g_ota.received);
			}

			if (ota_dbg_until && k_uptime_get() < ota_dbg_until) {
				static const struct { const char *k; uint32_t *v; } ota_kv[] = {
					{ "ota/slot/size",        &ota_dbg_slot    },
					{ "ota/slot/sector",      &ota_dbg_sector  },
					{ "ota/dbg/bytes",        &ota_dbg_bytes   },
					{ "ota/dbg/wall_ms",      &ota_dbg_wall_ms },
					{ "ota/dbg/erase_max_us", &ota_dbg_e_max   },
					{ "ota/dbg/prog_max_us",  &ota_dbg_p_max   },
					{ "ota/dbg/erase_n",      &ota_dbg_e_n     },
					{ "ota/dbg/prog_n",       &ota_dbg_p_n     },
				};

				for (unsigned i = 0; i < ARRAY_SIZE(ota_kv); i++) {
					dserv_state_name(&cfg, name, sizeof name, ota_kv[i].k);
					dserv_msg_int(f, name, 0, (int32_t) *ota_kv[i].v);
					pub_enqueue(f);
				}
				dserv_state_name(&cfg, name, sizeof name, "ota/dbg/verify");
				dserv_msg_int(f, name, 0, ota_dbg_verify);
				pub_enqueue(f);
				dserv_state_name(&cfg, name, sizeof name, "ota/dbg/rc");
				dserv_msg_int(f, name, 0, ota_dbg_rc);
				pub_enqueue(f);
			}
#endif

#if defined(CONFIG_NETWORKING)
			/* Publish-latency investigation: how long does one
			 * zsock_send() actually take, and how long is a full
			 * service-loop pass? Two frames the box emits us apart
			 * arrive at dserv ~650 us apart and three host-side
			 * theories were wrong -- these two numbers separate
			 * "the send is expensive" from "the loop is slow"
			 * from "neither, look outside the box". */
			{
				uint32_t sl = 0, sm = 0;
				box_net_eth_send_stats(&sl, &sm);
				dserv_state_name(&cfg, name, sizeof name, "dbg/send_us");
				dserv_msg_int(f, name, 0, (int32_t) sl);
				pub_enqueue(f);
				dserv_state_name(&cfg, name, sizeof name, "dbg/send_max_us");
				dserv_msg_int(f, name, 0, (int32_t) sm);
				pub_enqueue(f);
				dserv_state_name(&cfg, name, sizeof name, "dbg/loop_us");
				dserv_msg_int(f, name, 0, (int32_t) loop_last_us);
				pub_enqueue(f);
				dserv_state_name(&cfg, name, sizeof name, "dbg/loop_max_us");
				dserv_msg_int(f, name, 0, (int32_t) loop_max_us);
				/* WINDOWED: reset after publishing, so this reads "worst pass
				 * in the last second" rather than "worst pass since boot".
				 * The since-boot form cannot distinguish a one-off from a
				 * recurring stall, and reading recurrence into it produced a
				 * confidently wrong diagnosis on 2026-07-26. */
				loop_max_us = 0;
				pub_enqueue(f);
				{
					uint32_t wu = 0, wm = 0, ru = 0, rm = 0;
					box_net_eth_rx_stats(&wu, &wm, &ru, &rm);
					dserv_state_name(&cfg, name, sizeof name, "dbg/wake_us");
					dserv_msg_int(f, name, 0, (int32_t) wu);
					pub_enqueue(f);
					dserv_state_name(&cfg, name, sizeof name, "dbg/recv_us");
					dserv_msg_int(f, name, 0, (int32_t) ru);
					pub_enqueue(f);
					dserv_state_name(&cfg, name, sizeof name, "dbg/disp_us");
					dserv_msg_int(f, name, 0, (int32_t) disp_last_us);
					pub_enqueue(f);
					dserv_state_name(&cfg, name, sizeof name, "dbg/disp_max_us");
					dserv_msg_int(f, name, 0, (int32_t) disp_max_us);
					disp_max_us = 0;          /* windowed, see loop_max_us */
					pub_enqueue(f);
				}
				{
					/* The previously-dark segment: the stack's own
					 * residence measurement (see box_net_eth.h).
					 * Interval means; a side with no traffic since
					 * the last tick publishes nothing. */
					box_eth_stack_stats_t ss;
					if (box_net_eth_stack_stats(&ss) == 0) {
						char d[64];
						int off;
						if (ss.rx_count) {
							dserv_state_name(&cfg, name, sizeof name, "dbg/rxstack_us");
							dserv_msg_int(f, name, 0, (int32_t) ss.rx_avg_us);
							pub_enqueue(f);
							dserv_state_name(&cfg, name, sizeof name, "dbg/rxstack_n");
							dserv_msg_int(f, name, 0, (int32_t) ss.rx_count);
							pub_enqueue(f);
						}
						if (ss.rx_count && ss.rx_detail_n) {
							off = 0;
							for (int i = 0; i < ss.rx_detail_n; i++) {
								off += snprintf(d + off, sizeof d - off,
										i ? "/%u" : "%u",
										ss.rx_detail_us[i]);
							}
							dserv_state_name(&cfg, name, sizeof name, "dbg/rxstack_detail");
							dserv_msg_string(f, name, 0, d);
							pub_enqueue(f);
						}
						if (ss.tx_count) {
							dserv_state_name(&cfg, name, sizeof name, "dbg/txstack_us");
							dserv_msg_int(f, name, 0, (int32_t) ss.tx_avg_us);
							pub_enqueue(f);
							dserv_state_name(&cfg, name, sizeof name, "dbg/txstack_n");
							dserv_msg_int(f, name, 0, (int32_t) ss.tx_count);
							pub_enqueue(f);
						}
						if (ss.tx_count && ss.tx_detail_n) {
							off = 0;
							for (int i = 0; i < ss.tx_detail_n; i++) {
								off += snprintf(d + off, sizeof d - off,
										i ? "/%u" : "%u",
										ss.tx_detail_us[i]);
							}
							dserv_state_name(&cfg, name, sizeof name, "dbg/txstack_detail");
							dserv_msg_string(f, name, 0, d);
							pub_enqueue(f);
						}
					}
				}
#if defined(CONFIG_SOC_SERIES_IMXRT10XX)
				{
					/* TEMP: DI-silence hunt. Raw IGPIO interrupt
					 * state + ISR entry count, once a second. */
					uint32_t r[5];
					char g[96];
					box_gpio_dbg_regs(r);
					snprintf(g, sizeof g,
						 "imr=%lx isr=%lx icr1=%lx edge=%lx psr=%lx isrn=%lu",
						 (unsigned long) r[0], (unsigned long) r[1],
						 (unsigned long) r[2], (unsigned long) r[3],
						 (unsigned long) r[4],
						 (unsigned long) box_gpio_di_isr_count());
					dserv_state_name(&cfg, name, sizeof name, "dbg/gpio");
					dserv_msg_string(f, name, 0, g);
					pub_enqueue(f);
				}
#endif
			}
#endif

			/* box status as datapoints -- observable any time over the active
			 * uplink, not just at boot: active transport, and (where present)
			 * the Ethernet link/lease and the PTP hardware clock. */
			dserv_state_name(&cfg, name, sizeof name, "uplink");
			dserv_msg_string(f, name, 0, box_uplink_active_name());
			pub_enqueue(f);
#if defined(CONFIG_NETWORKING)
			dserv_state_name(&cfg, name, sizeof name, "net/link");
			dserv_msg_int(f, name, 0, box_net_eth_link());
			pub_enqueue(f);

			uint8_t ip[4];
			if (box_net_eth_get_ip(ip)) {
				char ips[16];
				snprintf(ips, sizeof ips, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
				dserv_state_name(&cfg, name, sizeof name, "net/ip");
				dserv_msg_string(f, name, 0, ips);
				pub_enqueue(f);
			}
			dserv_state_name(&cfg, name, sizeof name, "ptp/ns");
			dserv_msg_int64(f, name, 0, (int64_t) box_ptp_now_ns());
			pub_enqueue(f);

#if defined(CONFIG_PTP_CLOCK)
			/* Re-anchor from PTP once a second. Costs NO packets -- the
			 * offset the host gave us is constant, so this is two local
			 * clock reads. Contrast the obs anchor, which needs a frame to
			 * arrive and inherits its transport delay.
			 *
			 * Skipped during an obs (a re-anchor steps the offset), which is
			 * also why the 1 Hz cadence is harmless: the drift it corrects is
			 * the box crystal against PTP, and box_clock's rate estimator
			 * covers the gap across a trial. */
			if (ptp_offset_valid && !box_obs_active()) {
				uint32_t win = 0;

				if (ptp_reanchor(&win) == 0) {
					dserv_state_name(&cfg, name, sizeof name,
							 "sync/ptp_window_us");
					dserv_msg_int(f, name, 0, (int32_t) win);
					pub_enqueue(f);

					dserv_state_name(&cfg, name, sizeof name,
							 "sync/offset_us");
					dserv_msg_int64(f, name, 0, boxclk.offset_us);
					pub_enqueue(f);
				}
			}
#endif
#endif
			/* status is available on demand via the `show` CLI command and as
			 * these datapoints -- no periodic console spam. */
		}

		/* Work parked while the rig was mid-trial (see box_obs.h): run it now
		 * that we are idle. Nothing here is time-critical BY DEFINITION -- it
		 * was deferred precisely because it can block. */
		{
			uint32_t due = box_obs_take_deferred();

#if defined(BOX_HAVE_PERSIST)
			if (due & BOX_DEFER_SAVE) {
				uint8_t blob[BOX_PERSIST_BLOB_MAX];
				uint32_t bn = box_persist_serialize(&cfg, blob, sizeof blob);

				int src = box_flash_save(blob, bn);
				if (src == 0) {
					box_console_printf("deferred save -> ok (%u bytes)\n", bn);
				} else {
					/* Report the errno. A bare "FAILED" sent the
					 * RW612 hunt toward the FlexSPI driver when the
					 * answer was -EDEADLK (-45): NVS refusing an
					 * unrecognised region. */
					box_console_printf("deferred save -> FAILED (%u bytes, err=%d)\n",
					       bn, src);
				}
			}
#else
			(void) due;
#endif
		}

		/* Cost of the pass we just finished -- measured to the point of
		 * blocking, so the wait itself is excluded. */
		if (loop_t0) {
			uint32_t d = k_cyc_to_us_floor32(k_cycle_get_32() - loop_t0);
			loop_last_us = d;
			if (d > loop_max_us) {
				loop_max_us = d;
			}
		}

		/* Block until an ISR has work for us (CDC RX / DI edge), or the
		 * watchdog tick is due. Replaces a flat k_msleep(1) that added up to
		 * 1 ms to BOTH halves of every round trip. */
		box_event_wait(K_MSEC(1));
		loop_t0 = k_cycle_get_32();
	}
	return 0;
}
