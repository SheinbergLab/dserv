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
#include "box_fast.h"
#include "box_announce.h"
#include "box_boot.h"
#if defined(BOX_HAVE_ADC)
#include "box_ain.h"
#include "box_adc.h"
#endif
#if defined(CONFIG_DAC)
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dac.h>
#if DT_NODE_HAS_STATUS(DT_NODELABEL(dac0), okay)
/* the wire-contract DAC (cmd/dac/<ch>) -- MCXN947's dac0 on box pin 1 (D1) */
#define BOX_HAVE_DAC0 1
#endif
#endif
#include "box_obs.h"
#include "box_console.h"
#if defined(BOX_HAVE_PERSIST)
#include "box_flash.h"
#endif
#include "box_net_usb.h"
#include "box_uplink.h"
#include "box_beacon.h"                 /* LAN discovery broadcast (UDP :5011) */
#include "box_pub.h"
#include "box_event.h"
#if defined(BOX_HAVE_CPU1)
#include "box_cpu1.h"
#endif
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
#if defined(CONFIG_NETWORKING)
#include "box_fetch.h"
#endif
#endif
#if defined(CONFIG_BOX_ADC_STREAM)
#include "box_adc_stream.h"
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
/* +30 box-authoritative obs onset: whoever is bound to the obs line emits
 * BEGINOBS. When an at_abs fires on cfg.obs_pin, the ISR captures the actual
 * assertion instant and the loop publishes state/in_obs stamped with it --
 * the host's ess waits on exactly that event. Unbound rigs never take this
 * path and behave as they always did. */
static volatile uint64_t obs_fire_actual_us;
static volatile uint8_t  obs_fire_pending;
#endif /* CONFIG_PTP_CLOCK */

/* ---- deferred publishing ----
 * The telemetry pubq that lived here (drained N frames per loop pass) grew into
 * box_pub.c: two classes of queue and a dedicated below-main thread that is the
 * only writer of the uplink. The history that motivated it -- the ~9.5 ms loop
 * stalls from inline send bursts, and the SETTLED 2026-07-27 A/B showing drain
 * granularity does NOT move two-box skew (stamps are ISR-side; the stall is
 * pure command latency) -- is recorded with the mechanism, in box_pub.c. */

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
 * Two front-ends, one sink (box_ota.h):
 *
 *   cmd/ota/fetch "<sha256-hex> <size>"  -- +56, Ethernet: the box PULLS.
 *   box_fetch's worker '<'-gets extio/<name>/ota/image from dserv and streams
 *   it in, self-paced by TCP backpressure -- transfer time is flash-bound
 *   (~3 s for ~286 KB on this board). Announced via state/ota/fetch_ok so the
 *   host can pick this path without version guessing.
 *
 *   cmd/ota/begin + cmd/ota/chunk        -- the datapoint stream, kept for
 *   USB carriers and pre-+56 hosts/firmware. Strictly sequential;
 *   state/ota/ack is the contiguous cursor the host paces and resumes by.
 *   A chunk datapoint costs payload (name "extio/<box>/cmd/ota/chunk" eats
 *   24 of the 109-byte budget, leaving 85 -> 77 after the chunk header) but
 *   rides both carriers unchanged. The host side must respect eth_inq's
 *   64-frame drop-on-overflow depth: its ack-clocked window does (measured
 *   2026-08-10 -- the open-loop drip before it collapsed 13x under its own
 *   rate on exactly that queue).
 *
 * The framer also accepts DSERV_OTA_CHAR ('D' raw frames), still with no
 * eth-side injector: dserv owns the single connect-back socket, and the pull
 * made a second raw path moot. */
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

/* Publish classes (box_pub.h): box_pub_event for the record and for replies --
 * things a host is waiting on or that must not be silently lost -- and
 * box_pub_bulk for re-emittable state (telemetry, announce, OTA status), where
 * drop-oldest under pressure is the right policy. Every site below names its
 * class explicitly; the stage-1 pub_enqueue shim is gone. */

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
/* 64, was 32. SIZE THIS ABOVE THE LEAF COUNT OR THE SUPPRESSION SILENTLY STOPS
 * WORKING FOR THE EXCESS -- the table is first-come-first-served and overflow
 * publishes unconditionally (see below), so the leaves that lose are simply the
 * ones registered last, and nothing says so. Stage 2's twelve ain/dbg pacing
 * leaves went on the end of that array and pushed the total past 32: measured
 * on boxa, telemetry was running at ~48 frames/s against 250 frames/s of actual
 * eye data, i.e. a sixth of the uplink spent restating numbers that had not
 * changed. The whole point of this table is that not sending is worth more than
 * sending efficiently; an undersized one gives that back without a symptom you
 * would ever look for.
 *
 * There are ~53 distinct leaves today (24 literals + the ain/dbg block, plus 20
 * ain/stream/* that only appear when streamtest runs). If that grows past 64,
 * raise this -- dbg/pub_suppressed collapsing toward zero while pub_frames
 * climbs is the tell. */
#define PUB_PERIODIC_MAX 64
#define PUB_REFRESH_S    30

static struct { const char *leaf; uint32_t v; uint8_t seen; } pub_hist[PUB_PERIODIC_MAX];
static int     pub_force;          /* set once per pass; forces a full refresh */
static int64_t pub_refresh_at;
static uint32_t pub_suppressed;    /* frames NOT sent -- the win, measured */

/* ---- telemetry CLASS (v26) ----
 *
 * HEALTH answers "is this box alive, and is it losing data" -- the handful you
 * want from every box, always. FULL is everything else: the counters you turn on
 * for an afternoon while chasing something and turn off again.
 *
 * The split exists because telemetry is a PER-SEND CPU cost, not a bandwidth
 * one: 275-864 us per 128-byte frame. By stage 2 a box was spending ~48
 * frames/s on diagnostics against 250 of actual eye data, and at batch 10 the
 * diagnostics OUTWEIGHED the science. Suppression can't help there -- a counter
 * that advances every second genuinely changes every second -- so the only
 * lever left is not asking for it.
 *
 * The watchdog does not come through here at any level, deliberately: a debug
 * setting that could silence the liveness beacon would let a dead box look like
 * a quiet one. */
static void pub_periodic_raw(const char *leaf, uint32_t v)
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
		box_pub_bulk(pf);
	}
}

/* HEALTH: published at every level except `off`. */
static void pub_periodic(const char *leaf, uint32_t v)
{
	if (cfg.dbg_level == DBG_LEVEL_OFF) {
		return;
	}
	pub_periodic_raw(leaf, v);
}

/* FULL: published only when someone asked for it. */
static void pub_dbg(const char *leaf, uint32_t v)
{
	if (cfg.dbg_level != DBG_LEVEL_FULL) {
		return;
	}
	pub_periodic_raw(leaf, v);
}

/* Which ain/dbg leaves are health. Named rather than flagged in the array so
 * the classification reads as one list instead of a column of booleans that
 * nobody checks against each other. */
static int ain_dbg_is_health(const char *leaf)
{
	static const char *const h[] = {
		"ain/dbg/sweeps",   /* is it sampling at all */
		"ain/dbg/running",  /* ...and does it think it is */
		"ain/dbg/powered",
		"ain/dbg/pace",     /* hardware or software paced RIGHT NOW */
		"ain/dbg/dropped",  /* losing blocks to a slow uplink */
		"ain/dbg/late",     /* polled: samples never taken */
		"ain/dbg/overruns", /* stream: samples taken, not collected */
		"ain/dbg/slips",    /* stream: channel identity lost -- never ignore */
		"ain/dbg/chans",
	};

	for (unsigned i = 0; i < ARRAY_SIZE(h); i++) {
		if (strcmp(leaf, h[i]) == 0) {
			return 1;
		}
	}
	return 0;
}

#if defined(BOX_HAVE_OTA_SLOT)
/* BULK: rolling transfer status (ota/state, ota/bytes, the flashtest
 * re-announce) -- re-emittable, drop-oldest is fine. */
static void ota_pub_int(const char *leaf, int32_t v)
{
	uint8_t f[DSERV_MSG_LEN];
	char nm[80];

	dserv_state_name(&cfg, nm, sizeof nm, leaf);
	dserv_msg_int(f, nm, 0, v);
	box_pub_bulk(f);
}

static void ota_pub_str(const char *leaf, const char *v)
{
	uint8_t f[DSERV_MSG_LEN];
	char nm[80];

	dserv_state_name(&cfg, nm, sizeof nm, leaf);
	dserv_msg_string(f, nm, 0, v);
	box_pub_bulk(f);
}

/* EVENT: command verdicts (ota/arm and its rc). The host's arm wrapper is
 * polling these to decide its next move, and a refusal displaced by a manifest
 * burst reads as a box that never answered. */
static void ota_ack_int(const char *leaf, int32_t v)
{
	uint8_t f[DSERV_MSG_LEN];
	char nm[80];

	dserv_state_name(&cfg, nm, sizeof nm, leaf);
	dserv_msg_int(f, nm, 0, v);
	box_pub_event(f);
}

static void ota_ack_str(const char *leaf, const char *v)
{
	uint8_t f[DSERV_MSG_LEN];
	char nm[80];

	dserv_state_name(&cfg, nm, sizeof nm, leaf);
	dserv_msg_string(f, nm, 0, v);
	box_pub_event(f);
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

/* Open slot1 for a transfer -- the front half shared by BOTH front-ends,
 * cmd/ota/begin (host-pushed chunks) and cmd/ota/fetch (box-side pull): obs
 * gate, "<sha256-hex> <size>" parse, slot open + geometry, trailer/lead
 * hygiene, stats reset, and the staging announce. Returns 0 with the transfer
 * OPEN (g_ota staging, g_ota_active set), or -1 with the refusal already
 * published. `who` is the command name, for the console only. */
static int ota_open_slot(const char *who, const char *arg)
{
	uint8_t  sha[BOX_OTA_SHA_BYTES];
	uint32_t size = 0;

	/* An OTA blocks the service loop in ~60 ms slices for the whole
	 * image (PORTING.md), so it must never overlap a trial. */
	if (box_obs_active()) {
		g_ota.state = BOX_OTA_DONE_FAIL;
		g_ota.err   = BOX_OTA_ERR_STATE;
		ota_pub_done();
		box_console_printf("%s -> refused, in_obs\n", who);
		return -1;
	}
	ota_ain_yield();      /* begin erases the trailer -- that is flash work */
	if (strlen(arg) < 65 || box_ota_parse_sha(arg, sha) != 0) {
		g_ota.state = BOX_OTA_DONE_FAIL;
		g_ota.err   = BOX_OTA_ERR_SHA_INIT;
		ota_pub_done();
		box_console_printf("%s -> bad sha argument\n", who);
		return -1;
	}
	size = (uint32_t) strtoul(arg + 64, NULL, 0);

	int rc = box_ota_flash_open();
	if (rc != 0) {
		g_ota.state = BOX_OTA_DONE_FAIL;
		g_ota.err   = BOX_OTA_ERR_FLASH;
		ota_pub_done();
		box_console_printf("%s -> slot open failed (%d)\n", who, rc);
		return -1;
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
		box_console_printf("%s -> refused (%s)\n",
				   who, box_ota_err_str(g_ota.err));
		return -1;
	}
	/* Clear the trailer NOW rather than at arm time: it is an erase, and
	 * begin is already the point where we are allowed to touch flash
	 * (obs-gated above). Failing here is worth reporting but not fatal to
	 * the transfer -- the image is still worth staging, and arm will say so. */
	{
		int trc = box_ota_flash_clear_trailer();

		ota_pub_int("ota/trailer_rc", trc);
		if (trc != 0) {
			box_console_printf("%s -> WARNING trailer clear failed (%d);"
					   " arm will fail until it is erased\n", who, trc);
		}
	}
	/* ...and the LEAD sector, which staging never touches (the update
	 * starts one sector in under swap-using-offset) but a completed
	 * swap leaves holding the outgoing image's header. Skipping this
	 * cost every post-swap OTA one rejected boot -- MCUboot: "Secondary
	 * header magic detected in first sector, wrong upload address?" --
	 * with only the repush landing (box02, 2026-08-01). */
	{
		int lrc = box_ota_flash_clear_lead();

		if (lrc != 0) {
			ota_pub_int("ota/lead_rc", lrc);
			box_console_printf("%s -> WARNING lead-sector clear failed (%d);"
					   " MCUboot will reject this stage once\n", who, lrc);
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
	box_console_printf("%s -> staging %u B into slot1 (cap %u, sector %u)\n",
			   who, size, box_ota_flash_size(), box_ota_flash_sector());
	return 0;
}

#if defined(CONFIG_NETWORKING)
/* ---- fetch (pull) front-end: box_fetch's worker -> the same sink --------
 * Both callbacks run ON THE WORKER THREAD, and everything they touch is safe
 * there: g_ota is exclusively this transfer's while the fetch is busy (the
 * chunk handler bows out, see cmd/ota/chunk), box_ain_hold is atomic, and
 * every publish is a memcpy into box_pub's k_msgq. */
static int ota_fetch_sink(void *ud, const uint8_t *data, uint32_t len)
{
	ARG_UNUSED(ud);
	/* Refreshed per span, so the analog hold lasts exactly as long as the
	 * stream keeps moving -- the chunk path's rule, kept. */
	ota_ain_yield();
	if (box_ota_sink(&g_ota, data, len) != 0) {
		return -1;
	}
	if (g_ota.received >= g_ota_ack_at) {
		g_ota_ack_at = g_ota.received + BOX_OTA_ACK_EVERY;
		ota_pub_int("ota/ack", (int32_t) g_ota.received);
		ota_pub_int("ota/progress", box_ota_progress_pct(&g_ota));
	}
	return 0;
}

static void ota_fetch_done(void *ud, int rc)
{
	uint32_t e_max = 0, p_max = 0, e_n = 0, p_n = 0;

	ARG_UNUSED(ud);
	g_ota_active = 0;
	box_ota_finish(&g_ota, rc < 0 ? -1 : 0);
	box_ota_flash_stats(&e_max, &p_max, &e_n, &p_n);
	ota_pub_int("ota/fetch_rc", rc);     /* <0 disambiguates transport death */
	ota_pub_done();
	box_console_printf("cmd/ota/fetch -> %s (%s), %u B, "
			   "erase_max %u us, prog_max %u us\n",
			   box_ota_state_str(g_ota.state),
			   box_ota_err_str(g_ota.err), g_ota.received,
			   e_max, p_max);
}
#endif /* CONFIG_NETWORKING */
#endif /* BOX_HAVE_OTA_SLOT */

#if defined(CONFIG_PTP_CLOCK)

/* dserv_to_box_us lived here; it is box_clock_unstamp (core/box_clock.h) now,
 * seqlock-guarded because +29 moved its callers onto the reader thread while
 * anchors keep landing elsewhere. */

static void abs_fire(struct k_timer *t)
{
	int pin = (int) (intptr_t) k_timer_user_data_get(t);

	if (pin < 0 || pin >= BOX_NPINS) {
		return;
	}
	gpio_cmd_t c = { .op = GPIO_OP_SET, .pin = (uint8_t) pin, .value = 1 };
	box_gpio_exec(&cfg, &c);

	/* DO NOT PUBLISH HERE. This is a k_timer callback: two pins scheduled for
	 * the same instant run their callbacks back-to-back, so ANY work in the
	 * first delays the second's GPIO write. When publish_do() ended in an
	 * inline network send this was measured on a scope at 127-254 us between
	 * two same-T pins (30/30 shots, median 208, sign varying -- whichever
	 * callback ran second paid for the first one's publish, matching the
	 * dbg/send_us median of 142 us almost exactly). publish_do() is a memcpy
	 * into the event queue now, but the discipline stays: a timer callback
	 * does the pin and nothing else, because "cheap" work in ISR context is
	 * how the next 127 us creeps back in.
	 *
	 * Set the pin, flag it, get out. The main loop publishes. Accuracy is
	 * untouched: abs_target_us[] already holds the INTENDED time, which is what
	 * gets stamped whenever the publish actually goes out. */
	abs_pub_pending |= (1u << pin);

	/* +30: an at_abs fire ON THE OBS PIN is a scheduled obs onset, and the
	 * box that owns the line is the authority on when it rose -- capture
	 * the ACTUAL instant, correct the provisional epoch (set at arming so
	 * lead-window at-commands could arm), mark obs active, flag the
	 * publish. Flag-and-timestamp only: the ISR discipline above holds. */
	if (obs_is_leader(&cfg) && pin == (int) cfg.obs_pin) {
		obs_fire_actual_us = box_gpio_now_us();
		obs_begin_us = obs_fire_actual_us;
		box_obs_set(1);
		obs_fire_pending = 1;
	}
}

/* Every term behind the armed/late decision, for when a box insists it is late
 * and the clock looks fine. This is how the at_abs 64-bit truncation was found:
 * reading the source could not settle it, dbg_T = 2147483647 did instantly.
 *
 * OFF by default, and the CALLER must invoke this only AFTER k_timer_start():
 * lead_us is measured from a `now` read before the publishes, and when these
 * seven frames were inline sends they would have delayed the fire by their
 * full cost. They are memcpy-cheap enqueues now, but the ordering costs
 * nothing to keep and guards the one thing this path must never do.
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
		box_pub_bulk(f);
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
	box_pub_event(f);
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
	box_pub_event(f);
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
	box_pub_event(f);

	dserv_state_name(&cfg, nm, sizeof nm, "sync/box_us");
	dserv_msg_int64(f, nm, dserv_us, (int64_t) box_us);
	box_pub_event(f);

	dserv_state_name(&cfg, nm, sizeof nm, "sync/offset_us");
	dserv_msg_int64(f, nm, dserv_us, offset_us);
	box_pub_event(f);

	dserv_state_name(&cfg, nm, sizeof nm, "sync/source");
	dserv_msg_string(f, nm, dserv_us, hw ? "hw" : "sw");
	box_pub_event(f);

	if (transport_us >= 0) {
		dserv_state_name(&cfg, nm, sizeof nm, "sync/transport_us");
		dserv_msg_int64(f, nm, dserv_us, transport_us);
		box_pub_event(f);
	}
	if (boxclk.rate_valid) {          /* learned crystal rate: hw anchors only */
		dserv_state_name(&cfg, nm, sizeof nm, "sync/rate_ppb");
		dserv_msg_int(f, nm, dserv_us, boxclk.rate_ppb);
		box_pub_event(f);
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
/* box pin n = gpio2.n = pad GPIO_B0_n on this SoC (the legacy single-port
 * scheme -- box pin numbers here are GPIO2 bit indices, NOT the silkscreen).
 * LED: B0_03 (ball D8) -> Teensy pin 13, per the PJRC schematic.
 * BTN: was 4, but B0_04 is not brought out to a pad on the Teensy 4.0 at all
 * (the low pins expose B0_00/01/02/03/10/11, with EMC_* and AD_B0_* filling
 * the rest), so the "test button" input floated on a pin nobody could reach.
 * B0_01 (ball E7) -> Teensy pin 12 is broken out on both the 4.0 and the 4.1,
 * and sits next to the LED on the header. Pin 12 is also SPI MISO; nothing in
 * this firmware claims SPI on Teensy, but a build that adds it must move. */
#define LED_PIN  3    /* gpio2.3 = B0_03 = Teensy pin 13, on-board LED  */
#define BTN_PIN  1    /* gpio2.1 = B0_01 = Teensy pin 12, header pin    */
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

#if defined(BOX_HAVE_DAC0)
/* One-time channel setup, then immediate writes. Setup is deferred to first
 * use (not boot) so a board whose DAC is unused never touches the device;
 * re-setup after the console's `adccal` has run is harmless (idempotent). */
static int dac_apply(uint16_t code)
{
	static const struct device *dac;
	static int dac_ready;

	if (!dac_ready) {
		static const struct dac_channel_cfg dcfg = {
			.channel_id = 0,
			.resolution = 12,
			.buffered   = true,
		};
		dac = DEVICE_DT_GET(DT_NODELABEL(dac0));
		if (!device_is_ready(dac) || dac_channel_setup(dac, &dcfg) != 0) {
			return -1;
		}
		dac_ready = 1;
	}
	return dac_write_value(dac, 0, code);
}
#endif /* BOX_HAVE_DAC0 */

/* +23 instrumentation: the residual certification failures were at-trains
 * losing their LEADING commands with a silent console -- so the schedule
 * path gets a full wire-visible ledger. accepted/fired/refused are
 * cumulative (published 1 Hz with the other counters); every refusal also
 * answers immediately on state/sched/err, event class, exactly as at_abs
 * does. With these, a host can classify any missing pulse from the
 * datafile alone: sent > accepted = lost before the scheduler; accepted >
 * fired = armed-but-unfired (contract violation); fired but no edge =
 * output/input path. Knowing exactly what happened is this box's job. */
static uint32_t sched_acc, sched_fired_n, sched_ref;

/* +25: is the box clock currently PTP-disciplined? Set by every successful
 * PTP re-anchor, cleared when a hardware TTL edge takes over. While held,
 * the beginobs frame-arrival anchor must NOT touch the clock: box_clock
 * snaps its offset at EVERY anchor "trusted or not", so each obs was
 * demoting a sub-us disciplined clock to transport jitter (100s of us) --
 * caught 2026-08-02 watching sync/source flip ptp->sw per obs in the fleet
 * viewer, and measured as `at` med 406 us vs `at_abs` med 24 us on the
 * same PTP-locked Pi rig (the at epoch was frame ARRIVAL; at_abs cancels
 * the offset error by construction). */
static int clock_ptp_held;

static void sched_refuse_reply(const char *why)
{
	uint8_t f[DSERV_MSG_LEN];
	char nm[80];

	sched_ref++;
	dserv_state_name(&cfg, nm, sizeof nm, "sched/err");
	dserv_msg_string(f, nm, 0, why);
	box_pub_event(f);
}

/* ---- +29: the inbound schedule fast path (box_fast.h) ----
 *
 * Everything an `at` needs to arm correctly is decided in the microseconds
 * after its frame leaves the wire, so that is where it now runs: the eth
 * reader thread (priority 2) calls box_main_fast_frame() per received frame
 * and these handlers arm at ARRIVAL. Before this, arming waited on the
 * service loop -- whose passes measure up to ~3.7 ms -- and a 2 ms-spaced
 * burst landing inside a pass had its first targets already in the past:
 * fire-ASAP merged adjacent pulses with nothing dropped anywhere (the
 * soak's events signature, a few percent of sessions). The USB path calls
 * the same handlers from the service loop with arr_us = processing time --
 * the historical behaviour, byte-identical.
 *
 * The late split makes the ledger honest about WHOSE fault a late arm is:
 *   sched/late_arr   the frame ARRIVED after its target (host/network late)
 *   sched/late_proc  arrived in time, armed late (the pre-29 conflation;
 *                    pinned ~0 on eth by construction -- its return is the
 *                    regression canary)
 * Both still arm and fire ASAP: the ladder counts edges, and a late pulse
 * beats a silently missing one (the gates grade the lateness). */
static uint32_t sched_late_arr, sched_late_proc;

static void sched_count_late(uint64_t target, uint64_t now, uint64_t arr_us)
{
	if (target > now) {
		return;
	}
	if (arr_us && arr_us <= target) {
		sched_late_proc++;
	} else {
		sched_late_arr++;
	}
}

static int fast_msg(const dserv_msg_t *m, const char *leaf, uint64_t arr_us)
{
#if defined(CONFIG_PTP_CLOCK)
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
			uint64_t T   = (uint64_t) dserv_msg_as_ll(m);
			uint64_t now = box_gpio_now_us();
			uint8_t  f3[DSERV_MSG_LEN];
			char     nm3[80];

			/* T == 0 is CANCEL (+30): disarm whatever is pending on this
			 * pin. Exists for the scheduled-obs abort window -- a quit
			 * between request and onset must not leave a timer that
			 * fires a stray obs later. Replies "cancelled", which the
			 * host's reply monitor treats as benign. */
			if (T == 0) {
				k_timer_stop(&abs_timer[pin2]);
				dserv_state_name(&cfg, nm3, sizeof nm3, "sched/abs_err");
				dserv_msg_string(f3, nm3, 0, "cancelled");
				box_pub_event(f3);
				return 1;
			}

			/* The abs_err/abs_lead answers are REPLIES -- ess acts on
			 * armed-vs-refused before the pulse time arrives -- so they ride
			 * the event class, never behind a manifest drain. */
			if (!boxclk.synced) {
				dserv_state_name(&cfg, nm3, sizeof nm3, "sched/abs_err");
				dserv_msg_string(f3, nm3, 0, "unsynced");
				box_pub_event(f3);
				return 1;
			}

			uint64_t target_box = box_clock_unstamp(&boxclk, T);
			int64_t  lead_us    = (int64_t) target_box - (int64_t) now;

			/* NEVER fire late. A box that silently fires milliseconds after T
			 * is far worse than one that says it missed -- the whole value of
			 * time-triggering is that every node agrees on the instant. */
			if (lead_us <= 0) {
				sched_count_late(target_box, now, arr_us);
				dserv_state_name(&cfg, nm3, sizeof nm3, "sched/abs_late_us");
				dserv_msg_int64(f3, nm3, 0, -lead_us);
				box_pub_event(f3);
				dserv_state_name(&cfg, nm3, sizeof nm3, "sched/abs_err");
				dserv_msg_string(f3, nm3, 0, "late");
				box_pub_event(f3);
				if (sched_dbg) {
					sched_dbg_publish(T, now, target_box, lead_us);
				}
				return 1;
			}

			abs_target_us[pin2] = T;
			k_timer_stop(&abs_timer[pin2]);
			k_timer_user_data_set(&abs_timer[pin2], (void *) (intptr_t) pin2);
			k_timer_start(&abs_timer[pin2], K_USEC(lead_us), K_NO_WAIT);

			/* +30: arming the OBS pin declares a scheduled onset. The
			 * epoch is knowable NOW (the target), and the whole point of
			 * the lead window is that at-commands arrive inside it -- so
			 * they must find the epoch already set. Provisional here;
			 * the fire corrects it to the actual instant (<= the
			 * certified 120 us away) and marks obs ACTIVE. */
			if (obs_is_leader(&cfg) && pin2 == (int) cfg.obs_pin) {
				obs_begin_us = target_box;
			}

			/* Publish the lead so the host can see the margin it actually got
			 * -- shrinking lead is the early warning before anything is late. */
			dserv_state_name(&cfg, nm3, sizeof nm3, "sched/abs_lead_us");
			dserv_msg_int64(f3, nm3, 0, lead_us);
			box_pub_event(f3);
			dserv_state_name(&cfg, nm3, sizeof nm3, "sched/abs_err");
			dserv_msg_string(f3, nm3, 0, "armed");
			box_pub_event(f3);
			if (sched_dbg) {
				sched_dbg_publish(T, now, target_box, lead_us);
			}
			return 1;
		}
	}
#endif /* CONFIG_PTP_CLOCK */

	/* <prefix>/cmd/do/<n>/at and <prefix>/cmd/timer/<t>/at -- obs-relative
	 * schedules. These used to ride dserv_dispatch on the service loop; the
	 * interception here mirrors dispatch's semantics exactly (same width
	 * default, same refusals, same +23 ledger) and simply happens at
	 * arrival. The %n guard keeps "at" from matching "at_abs" -- and the
	 * at_abs block above runs FIRST anyway. */
	{
		int pin3, pos = -1;

		if (leaf &&
		    sscanf(leaf, "cmd/do/%d/at%n", &pin3, &pos) == 1 &&
		    pos > 0 && leaf[pos] == '\0' &&
		    pin3 >= 0 && pin3 < BOX_NPINS) {
			if (obs_begin_us == 0) {
				box_console_printf("sched: no beginobs yet, ignoring\n");
				sched_refuse_reply("no_beginobs");
				return 1;
			}
			uint64_t delta  = (uint64_t) dserv_msg_as_ll(m);
			uint64_t target = obs_begin_us + delta;
			uint32_t w = cfg.do_pulse_us[pin3] ? cfg.do_pulse_us[pin3] : 1000;

			sched_count_late(target, box_gpio_now_us(), arr_us);
			if (box_sched_arm(&cfg, (uint8_t) pin3, (uint8_t) pin3, w,
					  target) != 0) {
				box_console_printf("sched: table full\n");
				sched_refuse_reply("table_full");
			} else {
				sched_acc++;
			}
			return 1;
		}
	}
	{
		int tid, pos = -1;

		if (leaf &&
		    sscanf(leaf, "cmd/timer/%d/at%n", &tid, &pos) == 1 &&
		    pos > 0 && leaf[pos] == '\0' &&
		    tid >= 0 && tid <= 255) {
			if (obs_begin_us == 0) {
				box_console_printf("sched: no beginobs yet, ignoring\n");
				sched_refuse_reply("no_beginobs");
				return 1;
			}
			uint64_t target = obs_begin_us + (uint64_t) dserv_msg_as_ll(m);

			sched_count_late(target, box_gpio_now_us(), arr_us);
			if (box_sched_arm(&cfg, BOX_SCHED_NOTIFY_ONLY, (uint8_t) tid, 0,
					  target) != 0) {
				box_console_printf("sched: table full\n");
				sched_refuse_reply("table_full");
			} else {
				sched_acc++;
			}
			return 1;
		}
	}

	/* ess/in_obs -- the obs epoch every `at` above anchors on, so it MUST be
	 * handled on the same path at the same priority: a beginobs queued
	 * behind a fat pass while its at-burst fast-pathed ahead would arm
	 * against the PREVIOUS epoch. Moved here verbatim from the dispatch
	 * path; the one improvement is the sw-anchor using the true arrival
	 * stamp instead of processing time. */
	if (dserv_msg_name_eq(m, BOX_SYNC_DP)) {
		int obs = (int) dserv_msg_as_long(m);
		uint64_t now_box = box_gpio_now_us();

		/* ANCHOR. Prefer the IRQ-latched TTL edge (jitter ~us) over frame
		 * arrival (100s of us of transport jitter): a hardware anchor takes
		 * the transport out of the error budget entirely, and only trusted
		 * (hw) anchors are allowed to teach the crystal rate. */
		uint64_t anchor_box = arr_us ? arr_us : now_box;
		int hw = 0;

		if (sync_input_enabled(&cfg)) {
			uint64_t e = box_gpio_sync_edge_us(obs);   /* rising for obs=1 */

			if (e && now_box - e < SYNC_EDGE_WINDOW_US) {
				anchor_box = e;
				hw = 1;
			}
		}
		/* +25: a PTP-held clock is never demoted by frame arrival. A
		 * hardware TTL edge still outranks everything (it also directly
		 * measures the obs instant); otherwise, while PTP holds, keep
		 * the disciplined clock untouched and derive the obs epoch from
		 * the frame's DSERV timestamp mapped through it -- transport
		 * leaves the at-epoch error budget entirely. */
		int ptp_held = 0;
#if defined(CONFIG_PTP_CLOCK)
		ptp_held = clock_ptp_held && !hw;
#endif
		if (!ptp_held) {
			box_clock_sync(&boxclk, m->timestamp, anchor_box, hw);
			if (hw) {
				clock_ptp_held = 0;   /* hw anchors own the clock now */
			}
		}
		box_obs_set(obs);            /* keep the LEVEL, not just the edge */
		if (obs) {
#if defined(CONFIG_PTP_CLOCK)
			obs_begin_us = ptp_held
				? box_clock_unstamp(&boxclk, m->timestamp)
				: anchor_box;
#else
			obs_begin_us = anchor_box;   /* epoch for box-scheduled events */
#endif
		}

		/* drive the obs-mirror output (LED / scope trace) -- unless the
		 * pin is the LEADER-owned obs line (+31): then it moves only at
		 * scheduled instants (at_abs fire) plus the end clear below, so
		 * anything triggering on it never sees a frame-arrival twitch. */
		if (!obs_is_leader(&cfg)) {
			box_gpio_obs_mirror(&cfg, obs);
		} else if (!obs) {
			box_gpio_obs_mirror(&cfg, 0);   /* end clear is always honest */
		}

		/* publish the box's OWN live copy, so obs state is visible per-box in
		 * dserv without a scope -- honest, since it only updates when THIS box
		 * actually received the edge. */
		uint8_t of[DSERV_MSG_LEN];
		char onm[80];
		dserv_state_name(&cfg, onm, sizeof onm, "in_obs");
		dserv_msg_int(of, onm, m->timestamp, obs);
		box_pub_event(of);

		if (ptp_held) {
			/* no anchor happened; re-affirm the held source so the
			 * fleet viewer shows the truth at each obs boundary */
			uint8_t sf[DSERV_MSG_LEN];
			char snm[80];

			dserv_state_name(&cfg, snm, sizeof snm, "sync/source");
			dserv_msg_string(sf, snm, 0, "ptp");
			box_pub_event(sf);
			dserv_state_name(&cfg, snm, sizeof snm, "sync/offset_us");
			dserv_msg_int64(sf, snm, 0, boxclk.offset_us);
			box_pub_event(sf);
		} else {
			publish_sync(m->timestamp, anchor_box, boxclk.offset_us, hw,
				     hw ? (int64_t)(now_box - anchor_box) : -1);
		}
		return 1;
	}

	return 0;
}

int box_main_fast_frame(const uint8_t *frame, uint64_t arr_us)
{
	dserv_msg_t m;

	if (dserv_msg_parse(frame, &m) != 0) {
		return 1;                     /* malformed: consume (drop) */
	}
	cmds_rx++;

	char leafbuf[112];
	const char *leaf = frame_leaf(&m, leafbuf, sizeof leafbuf);

	return fast_msg(&m, leaf, arr_us);
}

/* Set around the dispatch of a frame the eth reader already screened and
 * counted (BOX_UPLINK_RX_FRAME): on_usb_frame must not count or fast-check
 * it again. Service-loop-only state. */
static int frames_prechecked;
/* Arrival stamp for a fence-deferred frame (BOX_UPLINK_RX_FRAME_RAW):
 * the reader stamped it at recv; the fast handlers here should judge
 * lateness against that truth, not the (later) processing instant. 0 on
 * the USB path, where arrival genuinely is processing time. */
static uint64_t dispatch_arr_us;

static void on_usb_frame(const uint8_t *frame, void *ud)
{
	ARG_UNUSED(ud);
	dserv_msg_t m;
	if (dserv_msg_parse(frame, &m) != 0) {
		return;
	}

	char leafbuf[112];
	const char *leaf = frame_leaf(&m, leafbuf, sizeof leafbuf);

	if (!frames_prechecked) {
		cmds_rx++;
		/* USB (and any transport without a reader): same fast handlers,
		 * service-loop context, arrival == processing time -- the
		 * historical behaviour, now with the ledger split recording it.
		 * A fence-deferred eth frame instead carries its true arrival. */
		if (fast_msg(&m, leaf, dispatch_arr_us ? dispatch_arr_us
						       : box_gpio_now_us())) {
			return;
		}
	}

#if defined(CONFIG_PTP_CLOCK)
	/* <prefix>/cmd/ptp/offset <us> -- the host's PHC->dserv constant.
	 *
	 * Handled HERE rather than in dserv_cfg__cmd() because src/core/ is shared
	 * verbatim with the Pico and this is Zephyr/PTP-only. Same precedent as
	 * ess/in_obs (now in fast_msg), which is also intercepted before dispatch.
	 *
	 * Anchoring is gated on !in_obs for the reason PORTING.md records: a
	 * re-anchor STEPS the offset, and applying that inside a data-collection
	 * window puts two events of one trial on different mappings. With PTP the
	 * step is sub-us rather than the hundreds of us an obs anchor moves, but
	 * the rule is about correctness, not magnitude.
	 */

	/* <prefix>/cmd/sched/debug 0|1 -- arm the at_abs diagnostics. Runtime, not
	 * persisted: turn it on against a live box, read sched/dbg_*, turn it off.
	 * See sched_dbg_publish() for why it may only run after the timer is armed. */
	{
		if (leaf && strcmp(leaf, "cmd/sched/debug") == 0) {
			sched_dbg = dserv_msg_as_long(&m) ? 1 : 0;

			uint8_t f2[DSERV_MSG_LEN];
			char nm[80];
			dserv_state_name(&cfg, nm, sizeof nm, "sched/debug");
			dserv_msg_int(f2, nm, 0, (int32_t) sched_dbg);
			box_pub_event(f2);
			return;
		}
	}

	{
		if (leaf && strcmp(leaf, "cmd/ptp/offset") == 0) {
			ptp_offset_us    = dserv_msg_as_ll(&m);
			ptp_offset_valid = 1;

			uint32_t win = 0;
			int ok = box_obs_active() ? -1 : ptp_reanchor(&win);

			if (ok == 0) {
				clock_ptp_held = 1;
			}

			/* Event class: sync/* is the RECORD of which mechanism stamped a
			 * datafile's timestamps -- a dropped frame here is a datafile
			 * that cannot explain itself. */
			uint8_t f2[DSERV_MSG_LEN];
			char nm[80];
			dserv_state_name(&cfg, nm, sizeof nm, "ptp/offset_us");
			dserv_msg_int64(f2, nm, 0, ptp_offset_us);
			box_pub_event(f2);

			if (ok == 0) {
				/* A FOURTH sync source beside hw / swc / sw, so a datafile
				 * records which mechanism produced its timestamps. */
				dserv_state_name(&cfg, nm, sizeof nm, "sync/source");
				dserv_msg_string(f2, nm, 0, "ptp");
				box_pub_event(f2);

				dserv_state_name(&cfg, nm, sizeof nm, "sync/ptp_window_us");
				dserv_msg_int(f2, nm, 0, (int32_t) win);
				box_pub_event(f2);

				dserv_state_name(&cfg, nm, sizeof nm, "sync/offset_us");
				dserv_msg_int64(f2, nm, 0, boxclk.offset_us);
				box_pub_event(f2);
			}
			return;
		}
	}
#endif

	/* ---- publisher knobs (box_pub.c; the measurement method is documented
	 * there with the SETTLED result the old per_pass arm produced) ----
	 * OUTSIDE every feature guard: box_pub runs on all boards, so a USB-only
	 * box answers these too (the stage-1 placement left them PTP-gated, which
	 * a teensy40 build flagged as `leaf` going entirely unused). */
	{
		if (leaf && strcmp(leaf, "cmd/pubq/bypass") == 0) {
			box_pub_set_bypass(dserv_msg_as_long(&m) ? 1 : 0);

			uint8_t f4[DSERV_MSG_LEN];
			char nm4[80];
			dserv_state_name(&cfg, nm4, sizeof nm4, "pubq/bypass");
			dserv_msg_int(f4, nm4, 0, (int32_t) box_pub_bypass());
			box_pub_event(f4);       /* knob echoes are replies: event class */
			return;
		}

		if (leaf && strcmp(leaf, "cmd/pubq/gather") == 0) {
			box_pub_set_gather((int) dserv_msg_as_long(&m));

			uint8_t f6[DSERV_MSG_LEN];
			char nm6[80];
			dserv_state_name(&cfg, nm6, sizeof nm6, "pubq/gather");
			dserv_msg_int(f6, nm6, 0, (int32_t) box_pub_gather());
			box_pub_event(f6);
			return;
		}

		/* INERT since the publisher thread: there is no per-pass drain budget
		 * to set any more. Kept because the interleaved-A/B scripts set it and
		 * read the echo; answering keeps them runnable against any firmware. */
		if (leaf && strcmp(leaf, "cmd/pubq/per_pass") == 0) {
			uint8_t f5[DSERV_MSG_LEN];
			char nm5[80];
			dserv_state_name(&cfg, nm5, sizeof nm5, "pubq/per_pass");
			dserv_msg_int(f5, nm5, 0, (int32_t) dserv_msg_as_long(&m));
			box_pub_event(f5);
			return;
		}
	}

#if defined(BOX_HAVE_DAC0)
	/* <prefix>/cmd/dac/<ch> <counts> -- immediate DAC set, 12-bit (0..4095).
	 *
	 * The wire-contract face of the DAC the console's `adccal` already
	 * exercises: with DAC0_OUT (box pin 1 / D1) jumpered to an analog input,
	 * a host can drive KNOWN mid-scale levels through the real acquisition
	 * path -- the region eye/joystick signals actually live in, which a
	 * digital rail-to-rail loopback never touches. Immediate-set only, by
	 * design: amplitude is the axis here; timing certification stays on the
	 * digital edges (and Zephyr's driver has no buffered waveforms anyway).
	 *
	 * Handled HERE, not in dserv_cfg__cmd(): src/core is shared verbatim
	 * with the DAC-less RP2350 fleet -- the same reasoning that keeps
	 * `adccal` in the platform console instead of box_cli.h.
	 *
	 * Replies ride the event class like every knob echo:
	 *   state/dac/<ch>   the APPLIED code (clamped to 0..4095)
	 *   state/dac/err    "no such channel" | "not ready"
	 * Known silicon caveat (PORTING.md): codes below ~700 clamp at ~517 mV;
	 * callers should certify the floor, not pretend it isn't there.
	 */
	{
		int dch, pos = -1;

		if (leaf &&
		    sscanf(leaf, "cmd/dac/%d%n", &dch, &pos) == 1 &&
		    pos > 0 && leaf[pos] == '\0') {
			uint8_t fdac[DSERV_MSG_LEN];
			char    nmd[80];

			if (dch != 0) {  /* dac1 (D0) exists in silicon, disabled in DT */
				dserv_state_name(&cfg, nmd, sizeof nmd, "dac/err");
				dserv_msg_string(fdac, nmd, 0, "no such channel");
				box_pub_event(fdac);
				return;
			}
			long code = dserv_msg_as_long(&m);
			if (code < 0)    code = 0;
			if (code > 4095) code = 4095;
			if (dac_apply((uint16_t) code) != 0) {
				dserv_state_name(&cfg, nmd, sizeof nmd, "dac/err");
				dserv_msg_string(fdac, nmd, 0, "not ready");
				box_pub_event(fdac);
				return;
			}
			dserv_state_name(&cfg, nmd, sizeof nmd, "dac/0");
			dserv_msg_int(fdac, nmd, 0, (int32_t) code);
			box_pub_event(fdac);
			return;
		}
	}
#endif /* BOX_HAVE_DAC0 */

#if defined(BOX_HAVE_OTA_SLOT)
#if defined(CONFIG_BOX_ADC_STREAM)
	/* <prefix>/cmd/ain/streamtest "<trig_hz> <ms> ?<avgs>?" -- Stage-0 probe
	 * of the hardware-paced pipeline (box_adc_stream.h): run CTIMER-triggered,
	 * DMA-drained acquisition for a bounded window and publish the hardware's
	 * own ledgers. The flashtest of the analog world: BLOCKS the service loop
	 * for <ms> (bench instrument, not a service), parks the polled sampler
	 * via the ordinary hold first, and the polled driver reprograms its own
	 * trigger/commands on the next sweep, so nothing needs restoring.
	 * Healthy = rc 0, words == expected, no FIFO-overflow bits in adc_stat. */
	if (leaf && strcmp(leaf, "cmd/ain/streamtest") == 0) {
		char arg[64];
		unsigned hz = 16000, tms = 250, avgs = 4;
		box_adc_stream_result_t sr;

		dserv_msg_copy_cstr(&m, arg, sizeof arg);
		(void) sscanf(arg, "%u %u %u", &hz, &tms, &avgs);
		if (box_obs_active()) {
			box_console_printf("cmd/ain/streamtest -> refused, in_obs\n");
			return;
		}
		box_ain_hold(tms + 3000u);   /* park the polled sampler for the run */
		k_msleep(50);                /* let an in-flight sweep drain out    */
		(void) box_adc_stream_test(&cfg, hz, avgs, tms, &sr);

		pub_periodic("ain/stream/rc",       (uint32_t) sr.rc);
		pub_periodic("ain/stream/stage",    sr.stage);
		pub_periodic("ain/stream/nch",      sr.nch);
		pub_periodic("ain/stream/avgs",     sr.avgs);
		pub_periodic("ain/stream/expected", sr.expected);
		pub_periodic("ain/stream/words",    sr.words);
		pub_periodic("ain/stream/majors",   sr.majors);
		pub_periodic("ain/stream/adc_stat", sr.adc_stat);
		pub_periodic("ain/stream/r0",       sr.ring0[0]);
		pub_periodic("ain/stream/r1",       sr.ring0[1]);
		pub_periodic("ain/stream/r2",       sr.ring0[2]);
		pub_periodic("ain/stream/tc_end",   sr.tc_end);
		pub_periodic("ain/stream/mr3",      sr.mr3);
		pub_periodic("ain/stream/tctrl0",   sr.tctrl0);
		pub_periodic("ain/stream/fcount",   sr.fcount);
		pub_periodic("ain/stream/sw_words", sr.sw_words);
		pub_periodic("ain/stream/sw_stat",  sr.sw_stat);
		pub_periodic("ain/stream/imux0",    sr.imux0);
		pub_periodic("ain/stream/emr",      sr.emr);
		pub_periodic("ain/stream/spill",    sr.spill);
		box_console_printf("cmd/ain/streamtest -> rc %d stage %u: "
				   "%u/%u words (%u ch, %ux avg, %u majors), stat 0x%08x\n",
				   sr.rc, sr.stage, sr.words, sr.expected,
				   sr.nch, sr.avgs, sr.majors, sr.adc_stat);
		return;
	}
#endif /* CONFIG_BOX_ADC_STREAM */

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

	/* <prefix>/cmd/ota/begin "<sha256-hex> <size>" -- open a transfer into slot1
	 * for the CHUNK front-end (the image then arrives as cmd/ota/chunk). */
	if (leaf && strcmp(leaf, "cmd/ota/begin") == 0) {
		char arg[96];

		dserv_msg_copy_cstr(&m, arg, sizeof arg);
#if defined(CONFIG_NETWORKING)
		if (box_fetch_busy()) {
			box_console_printf("cmd/ota/begin -> refused, a fetch is running\n");
			return;
		}
#endif
		(void) ota_open_slot("cmd/ota/begin", arg);
		return;
	}

	/* <prefix>/cmd/ota/fetch "<sha256-hex> <size>" -- open slot1 and PULL the
	 * image ourselves: box_fetch's worker '<'-gets extio/<name>/ota/image
	 * (staged by the host as a binary datapoint, RP2350-style) and streams it
	 * into the same sink. Self-paced by TCP -- the worker not reading during
	 * a sector erase IS the flow control -- so there are no windows, no
	 * probes, and nothing to resend. The host only fires this at boxes that
	 * announced state/ota/fetch_ok; refusals here are the belt to that brace. */
	if (leaf && strcmp(leaf, "cmd/ota/fetch") == 0) {
#if defined(CONFIG_NETWORKING)
		static const uint8_t ip0[4];
		char arg[96];
		char key[64];

		dserv_msg_copy_cstr(&m, arg, sizeof arg);
		if (strcmp(box_uplink_active_name(), "eth") != 0 ||
		    memcmp(cfg.dserv_ip, ip0, 4) == 0) {
			g_ota.state = BOX_OTA_DONE_FAIL;
			g_ota.err   = BOX_OTA_ERR_STATE;
			ota_pub_done();
			box_console_printf("cmd/ota/fetch -> refused (uplink %s, "
					   "dserv ip %s)\n", box_uplink_active_name(),
					   memcmp(cfg.dserv_ip, ip0, 4) ? "set" : "unset");
			return;
		}
		if (box_fetch_busy()) {
			box_console_printf("cmd/ota/fetch -> refused, already running\n");
			return;
		}
		if (ota_open_slot("cmd/ota/fetch", arg) != 0) {
			return;
		}
		int n = dserv_cfg_prefix(&cfg, key, sizeof key);

		snprintf(key + n, sizeof key - (size_t) n, "/ota/image");
		if (box_fetch_start(cfg.dserv_ip, dserv_cfg_port(&cfg), key,
				    ota_fetch_sink, ota_fetch_done, NULL) != 0) {
			g_ota_active = 0;
			g_ota.state  = BOX_OTA_DONE_FAIL;
			g_ota.err    = BOX_OTA_ERR_STATE;
			ota_pub_done();
			box_console_printf("cmd/ota/fetch -> worker refused, aborted\n");
		}
#else
		box_console_printf("cmd/ota/fetch -> no networking on this build\n");
#endif
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
#if defined(CONFIG_NETWORKING)
		/* A pull owns the sink: a chunk landing mid-fetch is a confused
		 * host, and sinking it here would race the worker thread. */
		if (box_fetch_busy()) {
			return;
		}
#endif
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
			ota_ack_str("ota/arm", "refused_in_obs");
			return;
		}
		if (g_ota.state != BOX_OTA_DONE_OK) {
			ota_ack_str("ota/arm", "refused_no_verified_image");
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
			ota_ack_str("ota/arm", "refused_no_image_header");
			ota_ack_int("ota/arm_rc", hrc);
			box_console_printf("cmd/ota/arm -> refused: no MCUboot header in slot1 (%d) "
					   "-- image staged at the wrong offset?\n", hrc);
			return;
		}

		int rc = boot_request_upgrade(BOOT_UPGRADE_TEST);

		if (rc != 0) {
			ota_ack_str("ota/arm", "failed");
			ota_ack_int("ota/arm_rc", rc);
			box_console_printf("cmd/ota/arm -> boot_request_upgrade failed (%d)\n", rc);
			return;
		}
		/* The "armed" ack must be ON THE WIRE before the reset lands -- this is
		 * the one command that makes the box disappear, and an ack dying in a
		 * queue at reboot leaves dserv showing a stale earlier verdict
		 * (observed: "refused_no_verified_image" standing while the box was
		 * demonstrably armed and swapping). The pre-box_pub fix sent these two
		 * frames directly from the loop; now they ride the event class and the
		 * box_pub_flush() below refuses to reboot past them. */
		{
			uint8_t fa2[DSERV_MSG_LEN];
			char nma[80];

			dserv_state_name(&cfg, nma, sizeof nma, "ota/arm");
			dserv_msg_string(fa2, nma, 0, "armed");
			box_pub_event(fa2);

			dserv_state_name(&cfg, nma, sizeof nma, "ota/arm_rc");
			dserv_msg_int(fa2, nma, 0, 0);
			box_pub_event(fa2);
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
		/* Drain the publish queues INTO the socket before resetting -- the
		 * deterministic version of the blind 600 ms "let the wire settle"
		 * sleep this replaces (typically done in ~2 ms; the cap only binds
		 * when the uplink is already dead, where waiting longer buys
		 * nothing). The short sleep after covers the console ring and the
		 * frames' flight time. */
		box_pub_flush(K_MSEC(600));
		k_msleep(100);
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

	/* ess/in_obs and the at/at_abs schedule classes never reach this point:
	 * fast_msg() consumed them -- at arrival on the eth reader, or just
	 * above for USB. The dispatch below still PARSES the at forms (core is
	 * shared), but their GPIO_OP_SCHED_* results are unreachable here. */

	gpio_cmd_t cmd;
	uint8_t xport_was = cfg.transport_mode;   /* for the strand-guard below */
	cfg_result_t r = dserv_dispatch(&cfg, &m, &cmd);
	if (r == CFG_GPIO && cmd.op != GPIO_OP_NONE &&
	    cmd.op != GPIO_OP_SCHED_PULSE && cmd.op != GPIO_OP_SCHED_TIMER) {
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
		box_announce_manifest(&cfg);   /* else ain/group/<label>/* reads stale
		                                * (batch=1 applied live, announced 2) */
#endif
	} else if (r == CFG_CONSOLE) {
		box_console_printf("console=%s -- save+reboot to apply\n",
		       dserv_console_str((uint8_t) dserv_cfg_console_mode(&cfg)));
		box_announce_manifest(&cfg);   /* else state/console reads stale */
	} else if (r == CFG_XPORT) {
		/* STRAND GUARD: refuse a policy that would orphan the box over the
		 * link this very set arrived on. Setting `usb` on a box whose
		 * active uplink is eth (or `eth` on a usb box) means the next boot
		 * comes up on the OTHER transport with no host -- recoverable only
		 * from the physical console (officepi/box02, 2026-08-05: usb set
		 * over eth, lost contact, fixed at the UART). `auto` is always safe
		 * (boots usb, senses the PHY, upgrades). The CLI `mode` verb keeps
		 * usb -- that path IS physical access. Datapoint-set is remote by
		 * definition, so this is a justified single-surface asymmetry, not
		 * a parity regression (documented in PORTING.md). */
		const char *act = box_uplink_active_name();
		uint8_t tgt = cfg.transport_mode;
		int strands = (tgt == XMODE_USB && !strcmp(act, "eth")) ||
			      (tgt == XMODE_ETH && !strcmp(act, "usb"));
		if (strands) {
			cfg.transport_mode = xport_was;   /* revert; nothing persisted */
			box_console_printf("xport.mode=%s REFUSED over %s -- would strand "
				"the box; set it from the console\n",
				dserv_xmode_str(tgt), act);
			box_announce_manifest(&cfg);      /* re-announce the UNCHANGED value */
		} else {
			box_console_printf("xport.mode=%s -- save+reboot to apply\n",
			       dserv_xmode_str(cfg.transport_mode));
			box_announce_manifest(&cfg);
		}
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
	} else if (r == CFG_OBS_MODE) {
		/* +31: reflect the role change live, so a UI's select doesn't sit
		 * stale until the next announce -- same courtesy pin moves get. */
		box_announce_obs_role(&cfg);
	} else if (r == CFG_DSERV_IP || r == CFG_DSERV_PORT) {
		/* Act on it. The matcher has already written the new target into cfg,
		 * and until this existed that was ALL that happened: uplink_service
		 * only redials a session that is already down, so a connected box kept
		 * publishing to its old host forever while state/dserv and the
		 * discovery beacon both reported the new address.
		 *
		 * That made adoption work on a stranded box and silently do nothing to
		 * a healthy one -- the difference being invisible from every status
		 * field. Found on the rig 2026-08-07 while returning an adopted box:
		 * the target read 192.168.88.40 while the box was still streaming to
		 * .17, and only a restart of the OLD host moved it.
		 *
		 * No-op when the target is unchanged, so a host reasserting the address
		 * a box already has costs nothing. */
		box_uplink_retarget(&cfg);
	} else if (r == CFG_NAME) {
		/* The prefix just changed in place: dserv still holds only the OLD
		 * %match patterns (this box is now command-deaf under both names
		 * until the 30 s refresh) and nothing announces the new identity.
		 * A full re-registration fixes both at once: dserv reopens the
		 * connect-back (fresh accept), the new-prefix matches follow, and
		 * the announce hold releases the burst only after they land -- so
		 * the box re-emerges under its new name complete and commandable.
		 * extio_rename (extioconf.tcl) drives this end to end. */
		box_uplink_reregister(&cfg);
		box_console_printf("config/name -> now '%s'; re-registering\n",
				   dserv_cfg_name(&cfg));
	} else if (r == CFG_ANNOUNCE) {
		/* Everything, not just the manifest: identity and the OTA counters
		 * live in the burst, and a UI asking "what are you now" wants the
		 * same picture it gets on connect rather than a subset that happens
		 * to cover today's caller. */
		box_announce_burst(&cfg, groups);
		box_console_printf("cmd/announce -> full re-announce\n");
	} else if (r == CFG_DUMP) {
		/* Publish the config as replayable CLI lines, one datapoint each, then
		 * the count LAST -- a reader that waits for state/cfg/dump/n knows the
		 * set is complete, and never has to guess whether more is coming.
		 *
		 * Indexed rather than one big datapoint because a frame carries 109
		 * bytes of name+data total; a whole dump is far past that, and chunking
		 * into a single leaf would make each write clobber the last. Same burst
		 * shape as the manifest announce, which box_pub_bulk already paces. */
		{
			static char    dtext[BOX_CONFIG_DUMP_MAX];
			static uint8_t vf[BOX_CONFIG_DUMP_MAX + 128];
			char nm[80];

			int n = box_console_config_dump(&cfg, dtext, sizeof dtext);
			dserv_state_name(&cfg, nm, sizeof nm, "cfg/dump");
			int vlen = dserv_msg_var_string(vf, sizeof vf, nm, 0, dtext);

			/* box_uplink_send returns 0 on success, NOT the byte count
			 * (box_uplink.h) -- comparing against vlen reported every
			 * successful send as a failure. */
			if (vlen > 0 && box_uplink_send(vf, vlen) == 0) {
				box_console_printf("cmd/dump -> %d bytes to state/cfg/dump%s\n",
						   n < 0 ? -n : n, n < 0 ? " (TRUNCATED)" : "");
			} else {
				box_console_printf("cmd/dump -> send failed (%d)\n", vlen);
			}
		}
	} else if (r == CFG_STATS_RESET) {
		/* Zero the since-boot diagnostic counters. The periodic publisher
		 * pushes the new values on its next tick, so the host sees zeros
		 * without asking. loop_max_us / disp_max_us are already windowed
		 * and reset themselves, so they are deliberately untouched. */
#if defined(BOX_HAVE_ADC)
		box_ain_stats_reset();
		box_adc_stats_reset();
#endif
#if defined(CONFIG_NETWORKING)
		box_net_eth_send_stats_reset();
#endif
		box_console_printf("cmd/stats/reset -> ain + adc + send counters zeroed\n");
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
		box_pub_flush(K_MSEC(300));           /* queued frames out before we vanish */
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
	box_pub_init();              /* ...then the thread that writes it */
#if defined(BOX_HAVE_CPU1)
	box_cpu1_start();            /* copy the embedded image to SRAMG, release */
#endif

	box_console_init(&cfg);      /* binds CDC or console UART per console_mode */
	dserv_framer_reset(&rx_framer);
	k_msleep(2000);              /* let the host enumerate + open the console */

#if defined(BOX_HAVE_ADC)
	/* Re-arm the analog boot hold ON THE WAY OUT of that sleep. The hold
	 * armed above (BOX_AIN_BOOT_HOLD_STEP_MS, 1500 ms) EXPIRED mid-sleep,
	 * and ain_boot_gate -- the thing that keeps re-arming it until PTP
	 * locks -- lives in the service loop we still have not reached. In that
	 * gap, a box whose configured rate saturates the sampler handed the
	 * whole machine to a cooperative thread before the console said a word
	 * (+59 trial wedge, 2026-08-11; box_ain.c's saturation guard is the
	 * other half of this fix). */
	box_ain_hold(BOX_AIN_BOOT_HOLD_STEP_MS);
#endif

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

		/* AFTER uplink_service so the health it reports is this pass's, and
		 * outside every dserv-target gate on purpose: a box with no target is
		 * precisely the box that most needs to be findable. Self-rate-limited
		 * to 1.5 s and a no-op without a local IP. */
		box_beacon_service(&cfg);

		/* No drain call here any more: queued frames leave through the
		 * publisher thread (box_pub.c), which runs in the gaps this loop
		 * spends parked in box_event_wait(). The loop's job is to ENQUEUE
		 * in memcpy time and get back to inbound commands. */

#if defined(BOX_HAVE_ADC)
		/* Keep analog held until the 1588 clock is disciplined. Re-arms a
		 * short deadline each pass rather than latching a flag, so the hold
		 * dies with the loop instead of outliving it. */
		ain_boot_gate();

#if defined(BOX_HAVE_OTA_SLOT)
		/* Hold analog for the ENTIRE staging phase, refreshed here each pass
		 * (not just per chunk). The per-chunk 2 s yield lapses when an
		 * inter-chunk gap exceeds it -- which the RX-underrun retransmit
		 * stalls during a big transfer routinely do, so analog resumed
		 * mid-download and ADDED to the contention that caused the stall (a
		 * feedback loop; box02 OTA 2026-08-05). Keyed on the state machine,
		 * the hold lasts exactly as long as bytes are actually streaming and
		 * releases the instant staging ends (verify/arm/fail) -- more precise
		 * than any timeout. */
		if (g_ota.state == BOX_OTA_STAGING) {
			box_ain_hold(1000);
		}
#endif

		/* Analog blocks, built on the sampling thread, stamped and ENQUEUED
		 * here. The uplink's single-writer invariant lives one level down:
		 * exactly one thread -- box_pub's -- ever writes the transport (in
		 * bypass mode, producers send inline under box_uplink's lock).
		 * Draining ALL queued blocks per pass is fine now that a block costs
		 * a memcpy, not a 275-864 us send: the old 2-per-pass budget was
		 * send-cost math. */
		for (;;) {
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
				box_pub_bulk(f);
			}
		}
#endif

		/* +29: kind-based inbound. BYTES = raw run for the framer (USB);
		 * FRAME = one whole frame the eth reader already received, stamped
		 * and fast-path-screened -- dispatch it directly, marked so the
		 * handler neither re-counts nor re-screens it. Drain a bounded
		 * burst per pass so a config flood clears in one wake. */
		int rlen = 0;
		uint64_t rarr = 0;
		int kind = box_uplink_poll2(rx, sizeof rx, &rlen, &rarr);
		for (int drain = 0; kind == BOX_UPLINK_RX_FRAME ||
				    kind == BOX_UPLINK_RX_FRAME_RAW; ) {
			uint32_t d0 = k_cycle_get_32();

			/* FRAME = reader already counted + fast-screened it;
			 * FRAME_RAW = deferred by the +32 order fence: run the
			 * full path here, in queue order, with the true arrival
			 * stamp carried along for the fast handlers. */
			frames_prechecked = (kind == BOX_UPLINK_RX_FRAME);
			dispatch_arr_us = rarr;
			on_usb_frame(rx, NULL);
			dispatch_arr_us = 0;
			frames_prechecked = 0;
			uint32_t dd = k_cyc_to_us_floor32(k_cycle_get_32() - d0);
			disp_last_us = dd;
			if (dd > disp_max_us) {
				disp_max_us = dd;
			}
			if (++drain >= 16) {
				break;           /* stay fair to the rest of the pass */
			}
			kind = box_uplink_poll2(rx, sizeof rx, &rlen, &rarr);
		}
		if (kind == BOX_UPLINK_RX_RESET) {
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
				/* Event class: this is the truth-correction that keeps a
				 * rebooted box from reading as synced (see above). The
				 * announce burst that precedes it is filling the BULK queue
				 * right now -- this frame must not be displaced by it. */
				box_pub_event(f0);
			}
#endif
		} else if (kind == BOX_UPLINK_RX_BYTES && rlen > 0) {
			/* Cost of turning received bytes into an executed command
			 * (framer + dispatch + GPIO write). Completes the inbound
			 * split alongside wake_us / recv_us in box_net_eth.c. */
			uint32_t d0 = k_cycle_get_32();
			dserv_framer_feed(&rx_framer, rx, (uint32_t) rlen, on_usb_frame, NULL);
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

		/* +30: the box-authoritative obs onset (see abs_fire). state/in_obs
		 * is stamped with the ACTUAL assertion instant on the dserv
		 * timeline; obs_fire_err_us = actual - requested is the per-obs
		 * fire error, the number the certification bounds at 120 us --
		 * published every obs now instead of measured only on the bench. */
		if (obs_fire_pending) {
			obs_fire_pending = 0;
			uint64_t actual = obs_fire_actual_us;
			uint8_t of[DSERV_MSG_LEN];

			dserv_state_name(&cfg, name, sizeof name, "in_obs");
			dserv_msg_int(of, name, event_stamp(actual), 1);
			box_pub_event(of);
			dserv_state_name(&cfg, name, sizeof name,
					 "sched/obs_fire_err_us");
			dserv_msg_int64(of, name, 0,
					(int64_t) event_stamp(actual) -
					(int64_t) abs_target_us[cfg.obs_pin]);
			box_pub_event(of);
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
			box_pub_event(f);
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

				sched_fired_n++;
				snprintf(leaf, sizeof leaf, "timer/%u", sf.tid);
				dserv_state_name(&cfg, name, sizeof name, leaf);
				dserv_msg_int(f, name, event_stamp(sf.fire_us), 1);
				box_pub_event(f);
			}
		}

#if defined(CONFIG_BT)
		/* BLE ingress: each peripheral's frame is already source-stamped
		 * (extio/<client>/...); relay it out the active uplink verbatim. */
		uint8_t bframe[DSERV_MSG_LEN];
		while (box_ble_poll(bframe)) {
			box_pub_event(bframe);   /* remote boxes' events: same class as ours */
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
			 * the watchdog value, so a stall is easier to see, not harder.
			 *
			 * (The 40-deep loop-drained queue above is history -- bulk frames
			 * now go to box_pub's 96-deep class with a thread draining at
			 * wire speed -- but the policy stays: the flash burst blocks THIS
			 * loop either way, and one honest burst beats N stale ones.) */
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
			 * trailing box_pub_bulk(f) far below to actually send it -- with each
			 * intervening sub-block "borrowing" f and then RESTORING the watchdog
			 * frame into it on the way out. That idiom published state/watchdog
			 * TWICE PER TICK, with the same value, ~20-40 ms apart (observed on a
			 * clean subscriber 2026-07-30, David spotted it): once at the enqueue
			 * that used to sit here, and again at the trailing one. One of the two
			 * restores was also dead code -- cmds_rx overwrote f before any
			 * enqueue could see it.
			 *
			 * Worse, the trailing box_pub_bulk(f) sat AFTER an #endif, so on a board
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
				box_pub_bulk(wf);
			}
			{
				/* USB caller cost + TX-ring drops. Published on EVERY
				 * board: the comparison against the Ethernet numbers is
				 * what turns "how many datapoints can I push" into a
				 * budget rather than a guess. */
				uint32_t ul = 0, um = 0, ud = 0;
				box_net_usb_send_stats(&ul, &um, &ud);
				pub_dbg("dbg/usb_send_us",     ul);
				pub_dbg("dbg/usb_send_max_us", um);
				pub_dbg("dbg/usb_drops",       ud);
				{
					/* The discovery beacon's transmit ledger. A
					 * fire-and-forget broadcast has no ack, so
					 * without this a box that never transmitted
					 * is indistinguishable from one nobody is
					 * listening for -- which is how the first
					 * cut's dead SO_BROADCAST path passed
					 * review and failed on the wire. */
					uint32_t bs = 0, bf = 0;
					int be = 0;
					box_beacon_stats(&bs, &bf, &be);
					pub_dbg("dbg/beacon_sent",  bs);
					pub_dbg("dbg/beacon_fail",  bf);
					pub_dbg("dbg/beacon_errno", be);
				}
				/* the at-schedule ledger (+23): cumulative, so a host
				 * can delta them over any obs window and classify a
				 * missing pulse from the datafile alone */
				pub_dbg("sched/accepted", sched_acc);
				pub_dbg("sched/fired",    sched_fired_n);
				pub_dbg("sched/refused",  sched_ref);
				/* +29 late split: late_arr = the frame ARRIVED after
				 * its target (host/network); late_proc = arrived in
				 * time but armed late -- pinned ~0 on eth by
				 * construction, so growth here is the fast path
				 * regressing. */
				pub_dbg("sched/late_arr",  sched_late_arr);
				pub_dbg("sched/late_proc", sched_late_proc);
				pub_periodic("dbg/di_fifo_drop", box_gpio_di_fifo_drops());
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
					uint32_t ain_gap_l = 0, ain_gap_m = 0;
					box_ain_late_gaps(&ain_gap_l, &ain_gap_m);
					box_adc_stats(&amx, &an);
					box_adc_pm_stats(&asp, &arm);
					uint32_t s_trig = 0, s_fails = 0, s_resync = 0;
					uint16_t s_spacing = 0;
					uint8_t  s_avgs = 0, s_clamped = 0;
					box_ain_stream_info(&s_trig, &s_spacing, &s_avgs,
							    &s_clamped, &s_fails, &s_resync);
					uint32_t s_deliv = 0, s_ovr = 0, s_slips = 0,
						 s_fovf = 0, s_spill = 0;
					uint32_t s_tns = 0, s_dus = 0, s_res = 0, s_resmax = 0;
					int32_t  s_ppm = 0;
#if defined(CONFIG_BOX_ADC_STREAM)
					box_adc_stream_stats(&s_deliv, &s_ovr, &s_slips,
							     &s_fovf, &s_spill);
					box_adc_stream_timing(&s_tns, &s_dus, &s_res, &s_resmax, &s_ppm);
#endif
					struct { const char *leaf; uint32_t v; } as[] = {
						{ "ain/dbg/sweeps",    asw },
						{ "ain/dbg/late_gap_us",     ain_gap_l },
						{ "ain/dbg/late_gap_max_us", ain_gap_m },
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
						/* PACING (v25). `pace` is what the hardware is
						 * doing, `pace_want` what the config asked
						 * for; they differ exactly when a stream start
						 * was refused and the sampler fell back, which
						 * is otherwise only detectable as jitter
						 * nobody is measuring. The geometry beside
						 * them says what the requested oversample
						 * actually BECAME -- and `clamped` says when
						 * it became less than was asked for. */
						{ "ain/dbg/pace",      (uint32_t) box_ain_streaming() },
						{ "ain/dbg/pace_want", (uint32_t) box_ain_pace_want() },
						{ "ain/dbg/trig_hz",   s_trig },
						{ "ain/dbg/spacing",   s_spacing },
						{ "ain/dbg/avgs",      1u << s_avgs },
						{ "ain/dbg/clamped",   s_clamped },
						{ "ain/dbg/pace_fails", s_fails },
						/* Stream health. `overruns` is the honest twin
						 * of `late`: samples the hardware took on time
						 * and the CPU failed to collect -- a strictly
						 * better failure than the polled path's, where
						 * a late pass means the sample never existed.
						 * Any `slips` or `spill` at all wants a look.
						 * `delivered` is deliberately NOT here: in
						 * stream mode it is `sweeps` by another name,
						 * and the 1 Hz block already learned once
						 * what redundant per-second leaves cost. */
						{ "ain/dbg/overruns",  s_ovr },
						{ "ain/dbg/slips",     s_slips },
						{ "ain/dbg/resyncs",   s_resync },
						{ "ain/dbg/fifo_ovf",  s_fovf },
						{ "ain/dbg/spill",     s_spill },
						/* TIMING MODEL (stage 3). trig_ns is the
						 * period actually programmed, so it against
						 * the requested rate shows the rounding the
						 * model absorbs. resid_max_us is the headline
						 * number: how late the interrupt ran against
						 * the trigger clock, i.e. exactly the jitter
						 * that used to BE the sample time on the
						 * polled path. */
						{ "ain/dbg/trig_ns",    s_tns },
						{ "ain/dbg/deliver_us", s_dus },
						{ "ain/dbg/resid_us",   s_res },
						{ "ain/dbg/resid_max_us", s_resmax },
						{ "ain/dbg/rate_ppm",   (uint32_t) s_ppm },
					};
					for (unsigned ai2 = 0; ai2 < ARRAY_SIZE(as); ai2++) {
						if (ain_dbg_is_health(as[ai2].leaf)) {
							pub_periodic(as[ai2].leaf, as[ai2].v);
						} else {
							pub_dbg(as[ai2].leaf, as[ai2].v);
						}
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
				pub_dbg("dbg/pub_suppressed", pub_suppressed);
			}

			/* What the publisher dropped, delayed, and batched. Published
			 * rather than left silent: a queue that quietly discards data is
			 * exactly the failure shape this project keeps getting caught by.
			 * wait_max is enqueue->wire for the slowest frame -- the number
			 * the bypass/gather A/B arms exist to compare. hwm grazing the
			 * queue depth (box_pub.c) is the "grow it" signal. */
			{
				box_pub_stats_t ps;

				box_pub_get_stats(&ps);
				pub_dbg("dbg/pub_bulk_drop",   ps.bulk_dropped);
				pub_dbg("dbg/pub_ev_drop",     ps.ev_dropped);
				pub_dbg("dbg/pub_wire_drop",   ps.wire_dropped);
				pub_dbg("dbg/pub_wait_max_us", ps.wait_max_us);
				pub_dbg("dbg/pub_bulk_hwm",    ps.bulk_hwm);
				pub_dbg("dbg/pub_gathers",     ps.gathers);
				pub_dbg("dbg/pub_frames",      ps.gather_frames);
			}

#if defined(BOX_HAVE_CPU1)
			/* The second core's pulse. hb frozen with alive=0 is "cpu1
			 * wedged or never booted"; both leaves changed-only, so a
			 * healthy core costs one frame a second (the counter moves)
			 * and a dead one costs nothing after the first report. */
			{
				uint32_t c1;
				int c1_alive;

				box_cpu1_stats(&c1, &c1_alive);
				pub_dbg("dbg/cpu1_hb",    c1);
				pub_dbg("dbg/cpu1_alive", (uint32_t) c1_alive);
			}
#endif

			/* Cumulative 1 Hz beats the loop was too stalled to emit. Nonzero
			 * means something held the loop for >1 s -- an OTA burst does this
			 * legitimately; anything else wants explaining. */
			dserv_state_name(&cfg, name, sizeof name, "dbg/wd_skipped");
			dserv_msg_int(f, name, 0, (int32_t) wd_skipped);
			box_pub_bulk(f);

#if defined(BOX_HAVE_OTA_SLOT)
			/* Re-announce the last flashtest result for a few ticks --
			 * see the ota_dbg_* declarations for why once is not enough. */
			if (ota_res_until && k_uptime_get() < ota_res_until) {
				ota_pub_str("ota/state",  box_ota_state_str(g_ota.state));
				ota_pub_str("ota/result", box_ota_err_str(g_ota.err));
				ota_pub_int("ota/progress", box_ota_progress_pct(&g_ota));
				ota_pub_int("ota/ack", (int32_t) g_ota.received);
			}

			/* THE TRIAL STATE IS A STANDING SIGNAL, not a single shot.
			 * Between arm and confirm, any reset silently loses the
			 * update -- and until now that state was announced exactly
			 * once, in a burst that is allowed to drop frames, so the
			 * page whose whole job is "make the operator confirm" could
			 * simply never hear about it. While unconfirmed, say so every
			 * pass; cmd/ota/confirm publishes the closing trial=0. */
			if (box_boot_on_trial()) {
				pub_periodic("ota/trial", 1);
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
					box_pub_bulk(f);
				}
				dserv_state_name(&cfg, name, sizeof name, "ota/dbg/verify");
				dserv_msg_int(f, name, 0, ota_dbg_verify);
				box_pub_bulk(f);
				dserv_state_name(&cfg, name, sizeof name, "ota/dbg/rc");
				dserv_msg_int(f, name, 0, ota_dbg_rc);
				box_pub_bulk(f);
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
				box_pub_bulk(f);
				dserv_state_name(&cfg, name, sizeof name, "dbg/send_max_us");
				dserv_msg_int(f, name, 0, (int32_t) sm);
				box_pub_bulk(f);
				dserv_state_name(&cfg, name, sizeof name, "dbg/loop_us");
				dserv_msg_int(f, name, 0, (int32_t) loop_last_us);
				box_pub_bulk(f);
				dserv_state_name(&cfg, name, sizeof name, "dbg/loop_max_us");
				dserv_msg_int(f, name, 0, (int32_t) loop_max_us);
				/* WINDOWED: reset after publishing, so this reads "worst pass
				 * in the last second" rather than "worst pass since boot".
				 * The since-boot form cannot distinguish a one-off from a
				 * recurring stall, and reading recurrence into it produced a
				 * confidently wrong diagnosis on 2026-07-26. */
				loop_max_us = 0;
				box_pub_bulk(f);
				{
					/* wake_us is -1 for a window with NO consumed wake --
					 * idle link OR dead wake thread; dbg/ethrx_age_ms below
					 * is what separates those two (grows only for dead). */
					uint32_t wu = 0, wm = 0, ru = 0, rm = 0;
					box_net_eth_rx_stats(&wu, &wm, &ru, &rm);
					dserv_state_name(&cfg, name, sizeof name, "dbg/wake_us");
					dserv_msg_int(f, name, 0, (int32_t) wu);
					box_pub_bulk(f);
					dserv_state_name(&cfg, name, sizeof name, "dbg/recv_us");
					dserv_msg_int(f, name, 0, (int32_t) ru);
					box_pub_bulk(f);

					/* RX-wake thread liveness -- the numbers that were
					 * missing while it died silently (2/4 boots, box02,
					 * 2026-08-01): age growing past ~6 s = wedged (the
					 * self-heal respawns at 5 s and that shows in
					 * respawns=1); stack_free is the measured headroom
					 * behind the 2048 sizing. */
					uint32_t ea = 0, er = 0, es = 0;
					box_net_eth_rx_health(&ea, &er, &es);
					dserv_state_name(&cfg, name, sizeof name, "dbg/ethrx_age_ms");
					dserv_msg_int(f, name, 0, (int32_t) ea);
					box_pub_bulk(f);
					dserv_state_name(&cfg, name, sizeof name, "dbg/ethrx_respawns");
					dserv_msg_int(f, name, 0, (int32_t) er);
					box_pub_bulk(f);
					dserv_state_name(&cfg, name, sizeof name, "dbg/ethrx_stack_free");
					dserv_msg_int(f, name, 0, (int32_t) es);
					box_pub_bulk(f);

					/* +29 reader inbound queue: drops are HOST
					 * COMMANDS lost to a full queue (nonzero =
					 * real problem); the watermark sizes it. */
					uint32_t qd = 0, qm = 0;
					box_net_eth_inq_stats(&qd, &qm);
					dserv_state_name(&cfg, name, sizeof name, "dbg/ethin_q_drop");
					dserv_msg_int(f, name, 0, (int32_t) qd);
					box_pub_bulk(f);
					dserv_state_name(&cfg, name, sizeof name, "dbg/ethin_q_max");
					dserv_msg_int(f, name, 0, (int32_t) qm);
					box_pub_bulk(f);
					dserv_state_name(&cfg, name, sizeof name, "dbg/ethin_fenced");
					dserv_msg_int(f, name, 0,
						      (int32_t) box_net_eth_inq_fenced());
					box_pub_bulk(f);
					/* v34: uplink sessions torn down by the
					 * give-up path. The connect counter only
					 * sees full resets (announce bursts);
					 * soft heals were invisible until this. */
					dserv_state_name(&cfg, name, sizeof name, "dbg/uplink_cycles");
					dserv_msg_int(f, name, 0,
						      (int32_t) box_net_eth_uplink_cycles());
					box_pub_bulk(f);
					dserv_state_name(&cfg, name, sizeof name, "dbg/disp_us");
					dserv_msg_int(f, name, 0, (int32_t) disp_last_us);
					box_pub_bulk(f);
					dserv_state_name(&cfg, name, sizeof name, "dbg/disp_max_us");
					dserv_msg_int(f, name, 0, (int32_t) disp_max_us);
					disp_max_us = 0;          /* windowed, see loop_max_us */
					box_pub_bulk(f);
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
							box_pub_bulk(f);
							dserv_state_name(&cfg, name, sizeof name, "dbg/rxstack_n");
							dserv_msg_int(f, name, 0, (int32_t) ss.rx_count);
							box_pub_bulk(f);
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
							box_pub_bulk(f);
						}
						if (ss.tx_count) {
							dserv_state_name(&cfg, name, sizeof name, "dbg/txstack_us");
							dserv_msg_int(f, name, 0, (int32_t) ss.tx_avg_us);
							box_pub_bulk(f);
							dserv_state_name(&cfg, name, sizeof name, "dbg/txstack_n");
							dserv_msg_int(f, name, 0, (int32_t) ss.tx_count);
							box_pub_bulk(f);
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
							box_pub_bulk(f);
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
					box_pub_bulk(f);
				}
#endif
			}
#endif

			/* box status as datapoints -- observable any time over the active
			 * uplink, not just at boot: active transport, and (where present)
			 * the Ethernet link/lease and the PTP hardware clock. */
			dserv_state_name(&cfg, name, sizeof name, "uplink");
			dserv_msg_string(f, name, 0, box_uplink_active_name());
			box_pub_bulk(f);
#if defined(CONFIG_NETWORKING)
			dserv_state_name(&cfg, name, sizeof name, "net/link");
			dserv_msg_int(f, name, 0, box_net_eth_link());
			box_pub_bulk(f);

			uint8_t ip[4];
			if (box_net_eth_get_ip(ip)) {
				char ips[16];
				snprintf(ips, sizeof ips, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
				dserv_state_name(&cfg, name, sizeof name, "net/ip");
				dserv_msg_string(f, name, 0, ips);
				box_pub_bulk(f);
			}
			dserv_state_name(&cfg, name, sizeof name, "ptp/ns");
			dserv_msg_int64(f, name, 0, (int64_t) box_ptp_now_ns());
			box_pub_bulk(f);

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
					clock_ptp_held = 1;
					dserv_state_name(&cfg, name, sizeof name,
							 "sync/ptp_window_us");
					dserv_msg_int(f, name, 0, (int32_t) win);
					box_pub_bulk(f);

					dserv_state_name(&cfg, name, sizeof name,
							 "sync/offset_us");
					dserv_msg_int64(f, name, 0, boxclk.offset_us);
					box_pub_bulk(f);
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
