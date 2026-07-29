# extio-zephyr — porting punch list

What the bare-metal RP2350 box (`wiznet-io/`) does that this Zephyr port does not
yet. Written 2026-07-23 from the code, not from memory.

The scale, in one measurement: **the Pico publishes 39 `state/*` datapoint keys;
this port publishes 6** (`watchdog`, `uplink`, `net/link`, `net/ip`, `ptp/ns`,
`in_obs`). Four core modules already forked into `src/core/` have **zero**
platform callers.

The encouraging half: the portable core is already here and is shared verbatim
(same wire contract, same CLI grammar, same config codec). Almost everything
below is platform glue plus wiring into the service loop — not redesign.

Verify a claim before trusting it; this list was accurate the day it was written.

---

## Open bugs (not ports — these are broken now)

- [ ] **Config set while a box is unreachable is silently lost.** Filed first as
      "`config/obs/pin` doesn't apply via datapoint", but the parse and dispatch
      are fine (`config/obs/pin` → `dserv_cfg__config("obs/pin")` →
      `obs_mirror_set`), and the host wires `config/*` and `cmd/*` through the
      same proc. Two real gaps, neither a firmware bug:
      * **Post-reboot/flash race.** `extio_discover` wires forwards only once a
        box's watchdog ADVANCES, and the box sleeps 2 s at boot before it
        publishes anything. Anything set in that window is dropped on the floor.
      * **Reboot divergence.** A datapoint is a COMMAND CHANNEL, not a status
        mirror. dserv never deletes datapoints, so `extio/<box>/config/*` still
        reads back the old value after the box has rebooted and lost it — the
        host looks configured while the box is not. Acute on Teensy, which has no
        persistence at all, so EVERY reboot wipes the box's config.
      Useful fact: `Dataserver::trigger()` fires on every set with no value
      comparison, so re-pushing the SAME value does reach the box — manual
      recovery is just setting it again.
      The principled fix is **manifest announce** (Tier 1): a box that announces
      its live config on connect makes the divergence visible instead of silent.
      A host-side auto-re-push when `extio_discover` newly wires a box would
      auto-heal this, but it is a POLICY change — for a box with working
      persistence it would clobber a `save`d config with dserv's stale values —
      and it touches the deployed module shared with production Pico boxes.
- [ ] **Data pipe: loss-free or merely loss-tolerant?** The framer resyncs on `>`
      and discards junk, so dropped frames would be invisible. The watchdog
      counter is a sequence marker — capture a few minutes with
      `dservctl listen --jsonl --for 5m "extio/box/state/watchdog"` and check for
      gaps before leaning on the Teensy RTT numbers.
- [ ] **CLI overstates the box.** `help`/`show` advertise `mcp`, `oled`, `ble`,
      `wdt` because the grammar is shared with the Pico, but nothing implements
      them here — they accept a command and do nothing.

## PTP — SOLVED on teensy41; a real risk on frdm_rw612

**`ptp_ready=0` was a missing `pinctrl-0`, not silicon and not a clock source.**
`ptp_clock_nxp_enet_init()` has exactly one failure path:

```c
ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
if (ret) return ret;          /* -> device_is_ready() == false */
```

Enabling the node with a bare `status = "okay"` leaves no default state, so init
fails — which is why it presented as "device not ready" rather than "clock reads
zero". The RT1060 EVK (same SoC family as Teensy 4.1) supplies one; copying that
pattern into `boards/teensy41.overlay` fixed it.

**Fixed and verified 2026-07-23 on teensy41:** boot banner reports
`PTP hw clock: ready=1`, and `extio/box/state/ptp/ns` advances ~1 s per second.
Pads are `AD_B1_02`/`AD_B1_03` = Teensy pins 14/15 (otherwise LPUART2, unused
here — our hardware console is lpuart6 on pins 0/1).

Caveat on accuracy: `ptp/ns` is published only once per second, so comparing two
samples against a host clock cannot resolve ppm-level rate error — the
quantization dwarfs it. All that is established is that the clock runs at roughly
the right speed. Real rate measurement needs on-box sampling (the echo/sync
machinery), not 1 Hz datapoints.

- [x] **frdm_rw612 DID hit the same failure — SOLVED 2026-07-25, no patch
      needed.** The prediction was right; the stated reason was wrong. **1588
      pinmux entries DO exist**: `IO_MUX_ENET_TIMER0..3` in
      `modules/hal/nxp/dts/nxp/rw/RW612-pinctrl.h` (pads IO27/IO61/IO24/IO26).
      So `pinctrl-0` has something valid to point at and neither the driver
      patch nor the reference-manual dig was necessary. A second, worse defect
      was hiding behind it. See "2026-07-25 — RW612 first boot" below.
- [ ] `CONFIG_PTP_CLOCK_SHELL=y` (from `samples/net/ptp`) adds shell commands to
      read/adjust the clock directly — a useful diagnostic now that the console
      works.
- [ ] `CONFIG_PTP=y` is the full PTP protocol stack. Not needed for the local
      free-running clock; needed for actual sync against the i.MX95 partner.

Keep the distinction that matters: a local free-running clock needs no peer and
*should* tick (it now does); sync/discipline needs a grandmaster.

---

## Tier 1 — box functionality rigs depend on

- [x] **Manifest / self-description announce — DONE 2026-07-23** (commit
      ae7c9c5). `src/box_announce.{h,c}`: identity (`transport`, `board`,
      `build`, `fw`, `boot` from hwinfo reset cause, `channel`, `ip`), manifest
      (`desc`, `pins/in`, `pins/out`, `obs_pin`, `sync_pin`, `mcp_en`,
      `oled_en`, `label/<n>`, `group/<name>/{pins,settle_ms,quiet,idx}`), and
      current DI + chord levels. **6 state keys → 31 on teensy40.**
      Fires on `BOX_NET_RESET` (a host opening the pipe); the manifest half
      re-fires on any label/desc/group or pin-map edit.
      Ghost avoidance is part of the contract, since dserv retains datapoints
      forever: obs/sync pin publish `-1` when off, a cleared label re-publishes
      `""` exactly once (published-mask), a disabled pin drops out of
      `pins/in|out`.
      Still absent vs the Pico's 39: the `sync/*` and `echo/*` clock keys (need
      clock sync), `ain/*` (needs analog), `ota/*`, and the BLE/battery keys.
      `uptime_us` and a `dserv` target key are easy follow-ons.
- [x] **DI groups (chords) — DONE 2026-07-23** (commit f7d914d). `box_group.h`
      wired into the service loop: `group_feed` per debounced edge, `group_poll`
      every pass (settle windows expire BETWEEN edges), publishing
      `state/group/<label>` stamped at the episode ONSET. `quiet` groups replace
      the per-pin event instead of doubling it. New platform hooks
      `box_gpio_now_us()` and `box_gpio_read_di_levels()` (the latter seeds
      `group_reset`, so a switch held at configure time is the baseline, not a
      phantom edge). Validated on teensy40 with a phys13→phys12 jumper: 6 edges →
      6 chords sharing their DI timestamps; `quiet 1` → 0 DI events, 6 chords.
      Fixed on the way: `main.c` published the RAW DI level, so `active_low` was
      a no-op on the wire; now publishes `di_logical()` like the Pico.
      **FULLY validated on hardware 2026-07-23** (teensy40, two independent
      channels: phys13→phys12 = pin3→pin1, phys10→phys11 = pin0→pin2):
      * independent bits — `pin1`-only=1, `pin2`-only=2, both=3 (unambiguous
        ordering: bit0=pin1, bit1=pin2)
      * `quiet` suppression — 0 DI events, chords only
      * roll-gap MERGE — two rises 0.17 ms apart (< 20 ms settle) → ONE
        `group=3`; two rises 180 ms apart (> settle) → `group=1` then `group=3`.
        This is the whole point of the settle machine and it behaves to contract.
- [x] **Clock sync — DONE 2026-07-23** (commit d5f8811). Every `ess/in_obs` edge
      anchors `box_clock`; all published event times go through `event_stamp()`.
      Hardware TTL edge preferred over frame arrival (250 ms recency gate), and
      only hw anchors are trusted to teach the crystal rate — a box with no TTL
      degrades to offset-only, as designed. All six `sync/*` keys publish.
      Validated on teensy40: sw path 7 anchors (`source="sw"`, no rate learned —
      correct); hw path 8 anchors with a TTL leading the frame (`source="hw"`,
      `transport_us` published, rate learning engaged).
      **Rate accuracy NOT verified.** The synthetic TTL is a separate `dservctl`
      call, so `transport_us` swings 9.6–15.6 ms and the learned `rate_ppb` is
      ~1000 ppm of test noise bounded only by the ±500 ppm clamp. Needs a real
      rig TTL (deterministic relative to the obs event) to measure properly.
      Gotcha: `dservctl listen --jsonl` renders negative ints as unsigned;
      `dservGet` is correct.
- [x] **Scheduled events — DONE 2026-07-23** (k_timer design, exactly as the
      guidance below prescribed). `src/platform/box_sched.{c,h}`: 8-slot table
      mirroring the Pico's `g_sched[]` contract — `cmd/do/<n>/at <us>` pulses
      pin n (width = `config/pin/<n>/pulse_us`, default 1000) at beginobs+us
      and posts `state/timer/<n>`; `cmd/timer/<t>/at <us>` is notify-only.
      Each slot rides its own k_timer; the expiry (ISR) drives the pulse at the
      intended instant, the service loop drains the publish stamped at the
      INTENDED fire time. No beginobs → console ignore; table full → console
      message. The pulse path itself moved off the hardware counter onto
      per-pin k_timers at the same time (see the autopsy below — the counter
      was the whole problem). Builds clean on all three boards.
      **RIG-VERIFIED 2026-07-23 on teensy40** (`tools/sched_verify.sh`):
      * `pulse_us 2000` → the di/1 pair that never existed on the counter
        path, widths 2065/2070 µs (k_timer tick round-up, within the accepted
        100 µs resolution);
      * `pulse_us 4000000` → rise/fall 4.000329 s apart with the watchdog at a
        perfect 1 Hz throughout (nothing blocks; the +329 µs = tick rounding
        plus sw-anchor crystal drift over 4 s — no hw TTL on the bench);
      * sched: `state/timer/3` and `state/timer/7` stamped **exactly**
        500000 µs apart (the +500 ms / +1000 ms deltas, 0 µs bookkeeping
        error), and the scheduled pulse fell 2060 µs after its stamp
        (`config/pin/3/pulse_us 2000` honored).
      * clean re-run with the obs mirror off: the scheduled pulse's PHYSICAL
        rising edge (DI-latched through the jumper) landed **175 µs after the
        intended instant** — end-to-end scheduled actuation on the 100 µs
        k_timer tick, sw anchors only. (Pico Tier C with hw anchors + µs
        alarms was 34 µs; this is the expected tick-resolution cost, well
        inside the accepted budget.) Width that run: 1976 µs — the ~24 µs
        asymmetry is GPIO-ISR latch latency behind the tick ISR that drives
        both edges, sub-tick noise.
      Reading the di trace around the sched test needs one fact: the box's
      **obs mirror is (persistently) on pin 3** (`state/obs_pin=3`), the same
      pin the loopback jumper drives — so beginobs itself raised di/1 (26 µs
      after the frame anchor — an accidental live demo of the hw-anchor
      margin), the scheduled raise was invisible (already high), and its fall
      is the di/1=0 that trails `state/timer/3`. For clean pulse loopbacks,
      `config/obs/pin off` (and `cmd/save`) or mirror on a non-jumpered pin.
      Note the host-side `config/obs/pin` datapoint read back "off" while the
      box announced 3 — the reboot-divergence gotcha from "Open bugs" in the
      flesh; the manifest announce is what made it visible, as designed.

### AUTOPSY: the "pulse produces no DI event" blocker — SOLVED 2026-07-23

The 2026-07-23 attempt was reverted over a mystery: `cmd/do/3 1|0` (SET)
published `state/di/1` every time, while `cmd/do/3/pulse_us <any width>`
published **nothing**, 2 ms through 4 s, and the working theory was "the DI
interrupt is not latching the pulse-driven edge; cause unknown."

**That theory was wrong, and the mystery is solved — live, on the bench.**
Reproduced over dserv against the still-instrumented firmware, then
discriminated with one experiment: park the pin HIGH first (`cmd/do/3 1`, event
arrives), THEN `pulse_us 2000` → **exactly one `state/di/1 = 0` arrives,
timestamped at the pulse**. The DI interrupt latches pulse-driven edges just
fine. What actually happens:

* **Every pulse was a microsecond-wide sliver, not its requested width.**
  Zephyr's `counter_mcux_gpt` driver arms an alarm (`mcux_gpt_set_alarm`)
  by enabling the compare interrupt **without clearing a stale compare status
  flag (OF1)** — so if OF1 is already latched, the "falling edge" callback runs
  the instant the alarm is armed, right after `box_gpio_exec` raised the pin.
* **OF1 was ALWAYS stale, courtesy of restart mode.** Our devicetree set no
  `run-mode`, and the `nxp,imx-gpt` binding defaults to `restart`: the counter
  resets at the compare value — and *re-crosses the old compare value forever*,
  every couple of seconds, re-latching OF1 while idle (the binding even warns
  about alarm side effects in restart mode). So from the first boot-heartbeat
  pulse onward, every later arm found a stale flag and fired instantly.
  (Restart mode also resets CNT **on any OCR1 write** — the driver's own
  `mcux_gpt_reset()` exploits exactly that — so even with the flag cleared,
  every relative alarm would fire LATE by CNT-at-arm-time, up to seconds.
  The counter path was unsalvageable on this driver without both a driver
  patch and `run-mode = "free-run"`.)
* **The sliver explains the "no event" perfectly.** Both DI edges land between
  two poller passes; `box_gpio_poll_di` samples the settled level, finds it
  equal to the published level, and reports nothing — the same both-edges-cancel
  mechanism we had (correctly) attributed to the k_busy_wait fallback, only via
  timing rather than blocking. `rb1=1` in the instrumentation was read inside
  the sliver. From parked-HIGH the sliver ends at a *different* level than
  published — hence exactly one fall event, which is what nailed it.
* **Nothing ever blocked.** `state/watchdog` held a perfect 1 Hz cadence
  through armed pulses — the "maybe we accidentally block during the timed
  pulse" hypothesis is retired for the armed path (the busy-wait fallback was
  real but rarely engaged; it is now deleted entirely).

Corrections to the 2026-07-23 consequences, now that the cause is known:
* Loopback tests driven with `pulse_us` are trustworthy again once the k_timer
  path lands (rig-verify first, of course).
* The recorded RTT numbers were driven by SET, not pulse — a pulse produced
  **no** reply frame at all, so there was nothing to time. CONFIRMED: `pulse_us`
  loopback gives clean rise+fall edges on the k_timer path (200 ms pulse →
  di/1=1 then di/1=0 exactly 200 ms apart).

### RTT re-measured on the full-box build (2026-07-23)

Re-ran `tools/rtt_bench.py` (host → `cmd/do` SET pin 3 → wire → DI pin 1 →
publish → host) on the current HEAD firmware, teensy40, dserv stopped, n=300 ×2:

    min 0.49–0.56   median 0.56–0.57   p99 0.61–0.64   sd 0.04–0.13 ms

That is ~2× the **0.286 ms median** recorded at commit baa81cb — and the FLOOR
moved with it (min 0.273 → ~0.49 ms), so it is systematic added latency, not
jitter. **Not a regression to fix blindly: baa81cb was a MINIMAL build (blocks
#2–4 only).** The current loop runs, every pass, group feed/poll (4 groups),
clock stamping, `box_sched` service (8 slots), the console-shell marshal check,
and manifest bookkeeping — the cost of being a complete box. Still comfortably
sub-ms and the measurement is clean (0 misses).
> **CORRECTION 2026-07-23 — the "it is `CONFIG_SHELL`" conclusion below is
> WRONG.** The "Shell OFF" row was measured with a fully STUBBED console
> (`box_console_init()` a no-op, the console CDC never claimed,
> `box_console_printf` a no-op) — so it removed *the console*, not just the
> Shell. Replacing the Shell with the small hand-rolled console (`CONFIG_SHELL`
> absent, verified) still measures **median 0.553–0.576**. Full picture:
>
> | build | floor | median |
> |---|---|---|
> | **no console at all** (stub) | 0.286 | **0.313** |
> | hand-rolled console on CDC | ~0.29 | ~0.56 |
> | Shell on CDC | 0.49 | 0.565 |
> | Shell on lpuart6 | 0.293 | 0.553 |
>
> * **FLOOR:** Shell-on-CDC is uniquely bad (0.49). Everything else ~0.29 — so
>   the floor penalty is the CDC-ACM *shell backend* specifically; the
>   hand-rolled console on the *same* CDC does not pay it.
> * **MEDIAN:** only *no console at all* is fast. **Any working console costs
>   ~0.24 ms**, Shell or not. So the median cost is having a console on the box,
>   not the Shell subsystem — and the XIP-cache-pressure hypothesis below is
>   correspondingly weaker (the hand-rolled console is 28 KB SMALLER and still
>   pays it).
>
> Mechanism still unknown. What is now ruled out: the Shell subsystem, the shell
> thread priority, the backend transport, per-pass service overhead, console-TX
> starvation, and workqueue sharing.

**(superseded — see the correction above)** Same board, same harness, n=300 each:

| build | floor | median |
|---|---|---|
| minimal baseline (baa81cb, recorded) | 0.273 | 0.286 |
| **Shell OFF** (console stubbed, `CONFIG_SHELL=n`) | **0.286** | **0.313** |
| Shell ON (as shipped) | 0.49 | 0.565 |
| Shell ON + `SHELL_THREAD_PRIORITY=14` | 0.405 | 0.585 |
| Shell ON + console port actively drained | 0.371 | 0.582 |
| Shell ON + `USBD_CDC_ACM_WORKQUEUE=y` | 0.352 | 0.574 |

Dropping the Shell recovers essentially all of it (0.565 → 0.313 ≈ the 0.286
baseline). Nothing else moves the median at all. **Hypotheses tested and
REFUTED — do not re-walk these:**
* *per-pass service overhead* — gating `box_uplink_service` +
  `box_console_service` to 1-in-64 left the median unchanged (and worsened p99);
* *shell thread preempting the loop* — dropping it to priority 14 (main is 0)
  changed nothing, so it is not CPU stolen by that thread;
* *console-CDC TX starved and retrying on the shared workqueue every 1 ms* —
  draining the console port throughout the run changed nothing;
* *CDC work sharing the cooperative system workqueue* — the dedicated CDC-ACM
  queue changed nothing. Note it is started at `CONFIG_SYSTEM_WORKQUEUE_PRIORITY`
  anyway, so it separates the queue but not the priority.

**The cost SPLITS IN TWO** (David's test: bind the shell to the hardware UART
instead of the console CDC — the Teensy board DTS already defaults
`zephyr,shell-uart = &lpuart6`, our overlay is what overrides it):

| build | floor | median |
|---|---|---|
| Shell OFF | 0.286 | 0.313 |
| Shell ON, **CDC-ACM** backend (as shipped) | 0.49 | 0.565 |
| Shell ON, **lpuart6** backend | **0.293** | **0.553** |

* **The FLOOR penalty is entirely the CDC-ACM shell backend** — moving the shell
  to a hardware UART recovers it completely (0.49 → 0.293 ≈ Shell-OFF's 0.286).
* **The MEDIAN penalty (~0.24 ms) is the Shell SUBSYSTEM itself** — unchanged at
  ~0.55 whichever backend it uses, and already shown to be independent of thread
  priority. So the USB path is ruled out as the main cost.

**Leading hypothesis for the median (UNTESTED):** the RT1062 XIP-executes from
external QSPI NOR, and `CONFIG_SHELL` adds ~40 KB of code (129 KB → 89 KB with it
removed). More code = more instruction-cache pressure, and a hot-loop miss costs
an external QSPI fetch. That would be independent of transport AND priority,
which is exactly the observed signature. Testable via Zephyr code relocation to
ITCM (the Teensy build already links `libcode_relocation_source_lib.a`), and it
would predict the effect is *much* smaller on a part that executes from internal
flash/RAM — worth re-checking on the RW612 before assuming it transfers.

**Config decision:** the shipped build keeps the shell on the console CDC. The
median is identical either way, and a USB console is worth far more than a
0.2 ms floor improvement that costs an FTDI dangling off pins 0/1.

Worth knowing for any future attempt: the workqueue sits INSIDE the round trip
twice (CDC RX inbound, CDC TX outbound), so *lowering* its priority would slow
our own reply frame — "deprioritize the workqueue" is the wrong instinct here.
`CONFIG_SYSTEM_WORKQUEUE_PRIORITY=-1` (cooperative) does outrank the main loop
unconditionally, which is why no amount of shell-thread tuning helps; the
remaining unknown is what the Shell puts on that path. Deferred logging is a
non-issue on this board — the teensy40 build has no `CONFIG_LOG` at all.

**Practical answer for now:** both figures are sub-ms and the cost buys an
interactive console. A latency-critical deployment can simply build without the
Shell (~0.25 ms back) — every config the CLI offers is also reachable over the
frame pipe as `config/...` datapoints. Re-measure on the RW612 before assuming
any of this transfers; it is different silicon and a different UDC.

Harness note: after the console-first CDC reorder the data pipe is the
HIGHER-numbered `cu.usbmodem*`; `rtt_bench.py` now takes `[-1]`, not `[0]`.

### Path BACK to the hardware counter (and why the RW612 likely doesn't need one)

`k_timer` costs us the Pico's 34 µs-class actuation (100 µs kernel tick here).
Recovering it looks feasible; checked against the drivers 2026-07-23:

* **`run-mode = "free-run"` is a one-line devicetree fix we never applied.** The
  `nxp,imx-gpt` binding exposes it (`enum: restart | free-run`, default
  `restart`) and the driver honors it (`enable_free_run` →
  `gptConfig.enableFreeRun`). It removes the CNT-reset-on-OCR1-write fault
  outright, and shrinks the stale-OF1 window from *every compare period* to
  *once per 32-bit wrap* (~172 s at 25 MHz `gptfreq`).
* **The stale-OF1 bug itself remains** — `mcux_gpt_set_alarm()` still does
  `SetOutputCompareValue` → `EnableInterrupts` with no `ClearStatusFlags`
  between.
* **BOTH FIXES TRIED ON HARDWARE 2026-07-23 — STILL NOT USABLE.** Reinstated the
  counter-armed falling edge on teensy40 with `run-mode = "free-run"`, then
  additionally hand-patched `counter_mcux_gpt.c` to `GPT_ClearStatusFlags(OF1)`
  before `GPT_EnableInterrupts`. Measured real widths through the phys13→phys12
  loopback (6 trials each):

  | requested | free-run only | free-run + OF1 patch |
  |---|---|---|
  | 2 000 µs | no edge pair | 2 123 µs (1 of 6) |
  | 20 000 µs | 20 095 µs (1 of 6) | 20 065 µs (2 of 6) |
  | 200 000 µs | no edge pair | no edge pair |

  Widths are ACCURATE when an edge pair appears, but most trials still produce
  no pair (i.e. a sliver). And the few successes cannot be attributed to the
  counter with confidence — `counter_set_channel_alarm()` returning `-EBUSY`
  falls back to `k_timer`, which would also measure correctly. **So the original
  "unsalvageable on this driver" verdict stands for the Teensy GPT**; free-run +
  the OF1 clear are necessary but not sufficient, and the residual fault was not
  isolated. Do not spend more on the GPT unless 34 µs actuation is needed *on a
  Teensy specifically*.
  (Both experiments fully reverted, including the shared `~/zephyrproject`
  driver patch — verify with `grep "extio patch" $ZEPHYR_BASE/drivers/counter/`
  before trusting a later build.)
* **The RW612's CTIMER is a DIFFERENT and healthier driver.** `nxp,lpc-ctimer` /
  `counter_mcux_ctimer.c`: **4 match channels** (vs the GPT's **1**), and
  `CTIMER_SetupMatch()` is called with `enableCounterReset = false`, so the
  restart-mode fault cannot occur by construction. **`BOX_PULSE_NCH 4` was never
  wrong — it was right for the target and wrong only for the Teensy stand-in.**
  Expect the hardware-counter path to work on the RW612 as originally designed;
  verify on silicon before relying on it.

**Upstream note:** the stale-flag bug bites free-run mode too (the counter
re-crosses an old compare value on every 32-bit wrap, ~172 s at the RT1062's
25 MHz `gptfreq`). Candidate one-line fix: `GPT_ClearStatusFlags(base,
kGPT_OutputCompare1Flag)` before `GPT_EnableInterrupts` in
`mcux_gpt_set_alarm()`. Worth filing against Zephyr; we no longer depend on it
(k_timer also dissolves the Teensy single-channel limit — the second 07-23
finding — since every pin/slot owns its own timer).

Two incidental fixes that rode along in `box_gpio_exec`: a SET now cancels the
pin's pending pulse falling edge (a stale fall timer could previously clobber a
later SET), and the ensure-output reconfigure is skipped for pins already
configured as outputs (it drives the pin low first — a real glitch on the wire
for SET-1-while-high, visible to whatever external hardware the pin drives).
- [ ] **Hardware obs-sync input** — half-present. `box_gpio_apply_config` already
      claims the pin and latches edges, but nothing consumes the latch; it only
      becomes meaningful once clock sync exists (it is the hardware anchor that
      takes transport jitter out of the error budget).

**Done:** obs-mirror output (`config/obs/pin` via console + `ess/in_obs` drives
the pin, publishes `state/in_obs`).

## PROPOSAL: selectable console — convenience (CDC) vs timing (UART)

**Status: proposed, not implemented.** Measurements behind it are in "RTT
re-measured" below; the short version:

| console binding | median RTT |
|---|---|
| none at all | 0.313 ms |
| **hardware UART (lpuart6, pins 0/1)** | **0.346 ms** |
| CDC-ACM (as shipped) | ~0.56 ms |
| + `CONFIG_SHELL` (either transport) | ~0.55–0.57 ms |

So the console *code* is nearly free (~0.03 ms); the ~0.21 ms is the cost of a
**second actively-claimed CDC-ACM instance**, and `CONFIG_SHELL` costs a further
~0.24 ms that does NOT stack with it (both saturate the same bottleneck — almost
certainly recurring work on the cooperative system workqueue at
`CONFIG_SYSTEM_WORKQUEUE_PRIORITY=-1`, which preempts the service loop; that also
explains why the dedicated CDC-ACM queue didn't help, being started at the same
priority).

### The two options have different natures

* **Console device (CDC vs UART) is RUNTIME-selectable, cheaply.** Both devices
  are already in every board's devicetree and enabled; `box_console_init()` only
  has to choose which pointer to bind. And the cost comes from *claiming* the CDC
  — bind the UART instead and the console CDC stays enumerated-but-unclaimed,
  which the stub build showed is free.
* **`CONFIG_SHELL` is COMPILE-time only.** It cannot be a runtime option; it has
  to be a separate build variant.

### Recommendation: a persisted config key, not a strap

`console cdc|uart` in `box_config_t` (additive at the struct tail, so old blobs
load as 0 = `cdc` = today's behaviour), applied at `box_console_init()`,
`save`+`reboot` to take effect — the same shape as the Pico's `mode auto|usb|eth`.

**Why a persisted key beats a strap here**, even though the Pico used a strap:
the Pico's GP28 strap guarded the *transport*, so a bad persisted value could
kill the only link to the box and the strap was the recovery path (see the
GP28 boot-hang lesson). That does not apply here — the console is **not** the
control channel. `config/console` is reachable over the frame pipe like any other
datapoint, so a box left on `uart` with no FTDI attached is still fully
configurable from dserv and can be flipped back. No pin consumed, no hardware
required, nothing to get locked out of.

Add a strap **only** if a board wants a hardware guarantee — as an optional DT
alias (`box-console-strap`), read at boot, **open = CDC** (the safe default that
works with no extra hardware), pulled low = UART. If the alias is absent the
feature compiles out. It would need `box_gpio_reserved()` to exclude the pin.

### Also worth doing

* **Announce it:** `state/console` = `"cdc"` | `"uart"` in the manifest, so a
  host can see which mode a box booted in without guessing.
* **Shell as a build variant:** keep a `shell.conf` (`CONFIG_SHELL=y` + the
  `zephyr,shell-uart` chosen) for a "convenience build", documented as costing
  ~0.24 ms median. Not the default.
* **Per-board testing:** the RW612 must be re-measured before assuming any of
  this transfers — different silicon, different UDC, and its CTIMER/console
  situation differs from the Teensy's.

## PROPOSAL: `cmd/announce`, and what it would take to auto-purge dead boxes

**Status: proposed, not implemented.** Prompted by "does `extio_clear` also clear
datapoints, and should it run automatically?"

### What the existing purge actually does

`config/extioconf.tcl`:
* `extio_clear <name>` — drops the box's forwards, then `dservClear`s every key
  matching `extio/<name>/state/*` **and** `extio/<name>/decoded/*`, and unsets
  the `::extio_known` / `::extio_wd` / `::extio_stale` tracking.
* `extio_clear_dead` — runs that for every box that has `state/` keys but no
  live forwards.

So **yes, it clears datapoints** — but note two things:
* **`config/*` and `cmd/*` are NOT cleared** (only those two patterns match). A
  purged-and-returned box can therefore still show host-side config it no longer
  holds — the same divergence as the "config set while unreachable" bug above.
* **A dserv restart clears everything anyway**, so ghosts are bounded by dserv's
  lifetime. (Observed 2026-07-23: after a restart the table held only
  `extio/box/` — 32 state + 1 decoded, zero `config/*`, and an unrelated
  `extio/office/*` had vanished entirely.)

### Why automating it is not safe as-is

**The manifest is 31 of those ~32 state keys** (`board`, `fw`, `build`,
`pins/in|out`, `label/<n>`, `group/<name>/*`, …) and `box_announce_burst()` only
fires on **`BOX_NET_RESET`** — the pass a host (re)opens the pipe. Therefore:

* *Box genuinely vanished, then returned* → **safe**. `extio_service` reopens the
  port, DTR rises, `BOX_NET_RESET` fires, the box re-announces. Purging lost
  nothing.
* *Box alive but briefly quiet* → **not safe**. `extio_discover` prunes forwards
  after >2 stale ticks (~6 s of frozen watchdog). Auto-purging on that wipes the
  manifest of a box that never left, and because there is no reconnect **nothing
  re-announces it** — the box sits there looking half-configured until something
  forces a reconnect. A `cmd/save` writing NVS is a plausible way to stall the
  loop past the threshold, so this would bite intermittently and confusingly.

### The proposal

1. **Add `cmd/announce`** — a few lines; `box_announce_burst()` already exists
   and is wired in `main.c`. Gives an on-demand "resync this box", useful on its
   own (a manual button in extio-setup / the fleet page), and it makes any purge
   recoverable without waiting for a reconnect.
2. **Only then consider auto-purge**, and with a *much* longer grace than the 6 s
   prune threshold — 30–60 s of frozen watchdog, so it means "gone", not "busy".
3. Have `extio_discover` fire `cmd/announce` whenever it **newly wires** a box.
   That closes the loop: purge freely, and anything still alive re-describes
   itself on the next discovery pass.
4. Decide separately whether a purge should also clear `config/*`/`cmd/*`. It
   would be more honest ("the ghost fully disappears") but it is a policy call —
   those are the command channel, not status.

Keep it manual until at least (1) is in. Doing (2) without (1) is the unsafe
ordering.

## PROPOSAL: a devicetree pin map (box pin → any GPIO), 2026-07-23

**Status: proposed, not implemented.** Worth deciding BEFORE block #7 — analog
needs specific SPI pins, and picking them under today's one-port constraint would
bake the limitation in.

### The problem

`box_gpio.c` maps `box pin n → <box-gpio-port>.n` via
`off_of(n) { return (gpio_pin_t) n; }` against a single aliased port. On
teensy40's top edge that gives:

| port | physical pins |
|---|---|
| **gpio2** (the alias) | 6, 7, 8, 9, 10, 11, 12, 13 |
| gpio1 | 0, 1 |
| gpio4 | 2, 3, 4, 5 |

1. **Six of fourteen top-edge pins are unreachable.** phys 0–5 live on gpio1 and
   gpio4; nothing in the current scheme can address them.
2. **The numbering is scrambled vs the silkscreen.** box 0→phys 10, box 1→phys
   12, box 2→phys 11, box 3→phys 13, box 10→phys 6, box 11→phys 9, box 16→phys
   8. This is not cosmetic — it cost real time on the bench repeatedly, and every
   "which pins do I jumper?" question needs a doc lookup.

### The proposal

Replace the single-port alias with a **phandle array of GPIO specs** — the
Zephyr-native primitive for exactly this, and consistent with
[[feedback_rw612_idiomatic_not_mirror]]:

```dts
/ {
	box_pins: box-pins {
		gpios = <&gpio2 3 0>,      /* box pin 0 */
			<&gpio4 4 0>,      /* box pin 1 -- unreachable today */
			<&gpio1 2 0>;      /* box pin 2 */
	};
};
```

resolved with `GPIO_DT_SPEC_GET_BY_IDX(DT_NODELABEL(box_pins), gpios, i)` into a
`struct gpio_dt_spec specs[]`. Buys: any port; per-pin flags (active-low, pulls)
declared in devicetree instead of config; and **box pin number chosen per board**
— on Teensy, make it the silkscreen number so box pin 13 IS the LED.

**The frozen wire contract is untouched**: `config/pin/<n>` and `cmd/do/<n>`
stay a flat index, it just resolves somewhere sane.

### What changes

* `box_gpio.c`: `port` + `off_of(n)` → `specs[n]`; the `gpio_pin_*()` calls
  become `gpio_pin_*_dt()`. Mechanical.
* **The DI callback is the real work.** `gpio_callback` is per-PORT, so a
  multi-port map needs one callback per distinct port plus a reverse
  (port,pin) → box-pin lookup in the ISR. Everything else is a rename.
* Board overlays gain the `box-pins` node; `box-gpio-port` retires.
* **Manifest:** add `state/pinmap/<n>` (e.g. `"T13"`) so extio-setup and the
  fleet page can show physical names instead of making users consult a table.

### Migration

Confirmed with David: no critical saved configs, so no persist migration is
needed — but a stored NVS blob's `pin 3 mode out` WOULD point at a different
physical pin after a remap, so bump `BOX_PERSIST_VERSION` (or just `factory`
each bench box) rather than silently reinterpreting old blobs.

Tests/tools that hardcode pin numbers and will need updating:
* `tools/sched_verify.sh` — `do/3`, `pin/3`, `pin/1` throughout, plus its
  "jumpered phys13 → phys12 (box pin 3 out → pin 1 in)" header comment;
* `tools/rtt_bench.py` — `OUT_PIN, IN_PIN = 3, 1`;
* `tools/box_sim.c` — `config/pin/5/...` fixtures (host-side only, no hardware,
  so these just need to stay self-consistent);
* `src/main.c` — `LED_PIN` / `BTN_PIN` demo defines, per board.

**Deployed Pico boxes are unaffected** — this is per-board devicetree; their
`box pin n = GP n` semantics are unchanged.

## PROPOSAL: zero-touch dserv discovery — who advertises to whom, 2026-07-28

**Status: proposed, not implemented.** Prompted by the MCXN947 bring-up, where
pointing a box at dserv by hand went wrong in a way a name lookup cannot fix.

### The motivating failure, concretely

`raspberrypi.local` resolves to **192.168.0.26** — the Pi's house-LAN side. The
address dserv is actually reachable on from the rig is **192.168.88.46**. A box
configured from the mDNS answer points somewhere it cannot route, and says
`dserv.live=n/a` while looking otherwise healthy.

**This is structural, not a misconfiguration.** A multi-homed host has no way to
know which of its addresses the asker meant, so a NAME can always resolve to the
wrong interface. **An advert sent out of the interface dserv is listening on
carries the right address by construction.** That is the real argument for
discovery here — not convenience.

### Half of it already exists, pointing the OTHER way

Verified in the source, not remembered:

* `wiznet-io/pico/wizchip_dserv_config.c:361` — every **Ethernet** RP2350 box
  already broadcasts a UDP beacon on **port 5011** every 1500 ms:

  ```json
  {"t":"extio","v":1,"name":"box","ip":"...","fw":"...","board":"...",
   "build":"...","target":"0.0.0.0:4620"}
  ```

  It is a no-op on USB (`local_ip` 0.0.0.0 → early return), and it deliberately
  **fires BEFORE the dserv-target gate** so a brand-new box announces itself.
* **`target: 0.0.0.0` is already the "unconfigured" marker on the wire.** The
  one field a discovery scheme needs most is in the protocol today.
* `tools/extio-setup/discover.go` already listens (12 s TTL, keyed by box IP)
  and exposes `/api/discover`; its own comment says assigning the target is "a
  follow-up".
* Boxes additionally run a **config server on TCP :5010**.

So the model today is *box advertises, human adopts via extio-setup*. The
proposal is to close the loop automatically.

### Two shapes

**A — host-side adoption. No new protocol, no firmware.** Something on the
dserv host listens for beacons carrying `target 0.0.0.0` and pushes its own
address to the box over the config server the box already runs on :5010.

**B — dserv advertises, box adopts.** dserv broadcasts
`{"t":"dserv","v":1,"ip":...,"port":...,"host":...,"site":...}`; an unconfigured
box adopts it. Symmetric with the box beacon, and the only option if a box
cannot run a config server.

**Recommendation: do A first.** It is a TOOL change rather than a CONTRACT
change — nothing on the wire moves, deployed Pico boxes need no reflash, and it
can be withdrawn without leaving a protocol behind. B stays open and is the
better long-term shape; nothing in A blocks it.

### Adoption must be BOUNDED — this is the part to get right

There is more than one dserv on more than one subnet here (rig, office, dev
laptops). **A box that attaches to the first dserv it hears is the juicer wedge
again** — grab the first thing that answers, get the wrong device, fail
silently. Constraints:

* **Opt-in per dserv** (`extio_adopt on`), OFF by default. A dserv that has not
  been told to adopt must never claim anything.
* **Match on a `site` string**, so a box only adopts an advert it was told to
  expect. Name-based allowlists work too but do not scale to a fresh box, which
  is exactly the case discovery is for.
* **Learn it, do NOT persist it.** An adopted target should live in RAM until
  someone explicitly `save`s. A reboot then returns an ambiguous box to
  unconfigured instead of welding a wrong assignment in — and it keeps this
  clear of the reboot-divergence bug in "Open bugs" above.
* **Never adopt over an existing target.** Adoption applies only to
  `dserv 0.0.0.0`.

### What this costs on the Zephyr boxes

**Neither piece exists here.** Tier 3 lists both "LAN discovery beacon (UDP
:5011)" and the config server as *no counterpart*, which is precisely why the
MCXN947 had to be told an IP over a serial console during bring-up. So the
Zephyr port needs the beacon and the :5010 config server ported before it can
participate in EITHER shape. That is a port of something already written and
proven, not a design — but it is real work, and it is a prerequisite, not a
consequence.

### Open decisions

1. Broadcast or multicast for B, and whether the beacon port is shared with
   5011 or separate (a shared port with a `"t"` discriminator is tempting and
   makes one listener serve both directions).
2. Whether adoption is announced — a box that adopted should say so
   (`state/dserv/adopted` = the source of the target), or the whole thing is
   invisible when it goes wrong. Same principle as the manifest announce.
3. Whether extio-setup's existing `/api/discover` becomes the adoption UI (a
   button per unconfigured box) before anything automatic exists at all. That
   may be enough on its own, and it is the smallest possible step.

## Tier 2 — peripherals

- [ ] **Analog: MCP3204 + analog groups** — `src/core/box_ain_group.h`, zero
      callers; no platform driver. Block #7. On RW612 this also decides on-chip
      GAU GPADC vs the external MCP3204 (BENCH_NXP D10 — watch RF coupling into
      on-chip analog on a tri-radio part).
      **Status 2026-07-28:** everything ABOVE the converter is proven on
      hardware; the MCP3204 itself never answered and is PARKED (see the
      2026-07-28 sections at the end). The live path is now the MCXN947's
      on-chip `lpadc0`, whose open item is a **channel -> `input_positive` map**
      — `box_adc.c` assumes channel *n* IS input *n*, which is true of the
      MCP3204 and false of the LPADC. That map is board wiring, so decide it
      together with the devicetree pin map proposed below, not separately.
- [ ] **OLED status display** (`pico_oled.h`, SSD1306 SPI) — no counterpart.
- [ ] **WS2812 status LED** (`pico_status_led.h`) — no counterpart.

## Tier 3 — fleet / ops

- [ ] **OTA.** The Pico's A/B + TBYB scheme does not port; RW612 wants
      MCUboot + mcumgr over the live link. Different mechanism, not a translation.
- [x] **Persistence — SD-FAT backend WORKS on teensy41 (2026-07-23).** Previously
      written off as "the usdhc/SD stack hard-faults at driver init = immature
      Teensy peripheral support". **That was wrong.** With a boot log finally
      visible, the crash is a plain **main-thread stack overflow**: MPU fault,
      `pc`/`xpsr` full of Zephyr's `0xaa` stack-fill sentinel.
      `box_flash_init()` mounts FATFS from `main()`, and FATFS + disk + SDMMC put
      sector buffers and filesystem structs on the caller's stack — far past the
      4096 default. `CONFIG_MAIN_STACK_SIZE=8192` fixes it.
      Verified end to end: `cmd/save` writes `/SD:/extio.cfg`, and the next boot
      loads it (the first-boot `E: file open error (-2)` = ENOENT disappears).
      Two config fixes were needed together — the stack size AND
      `CONFIG_DISK_DRIVERS=y` (see below), which is why earlier attempts failed
      in two different ways at once.
- [x] **Persistence — NVS/FlexSPI backend ALSO WORKS on teensy41 (2026-07-23).**
      Re-tested with the boot log attached: **no crash, USB enumerates, and
      `obs.pin=7` survives a reboot** — i.e. NVS writes the same QSPI flash the
      chip XIP-executes from, successfully. The old "re-inits the controller and
      hard-faults at boot" verdict does not reproduce.
      **Both persistence verdicts were the SAME bug**: NVS also mounts from
      `main()`, so it overflowed the 4096 stack exactly like FATFS did. This test
      inherited `CONFIG_MAIN_STACK_SIZE=8192` from `boards/teensy41.conf` (board
      confs are auto-merged) and simply worked. `CONFIG_FLASH_MCUX_FLEXSPI_XIP=y`
      is enabled automatically by the RT10xx SoC defconfig, so the XIP-safe path
      was never missing.
      Needs a `storage_partition`, which teensy4 does not define — the test
      overlay carved the last 64 KB of the 8 MB W25Q64:
      `partition@7f0000 { label = "storage"; reg = <0x007f0000 DT_SIZE_K(64)>; }`
      under a `compatible = "fixed-partitions"` parent (box_flash.c uses
      `DT_MTD_FROM_FIXED_PARTITION`).
      **CHECKED 2026-07-23 (teensy40, incidentally): `teensy_loader_cli`/
      HalfKay does NOT wipe the storage partition.** The k_timer-pulse reflash
      came back with the full saved config intact — pin modes, labels, the joy
      group, `pin3 pulse=2000us`, and (the tell) `obs_pin=3` from earlier
      obs-mirror testing, none of it re-entered by hand. So saved config
      survives REFLASHES, not just reboots. Corollary: a stale persisted
      config outlives new firmware — if a test needs a clean slate, clear it
      explicitly (`cmd/factory`, or set+`cmd/save`).
      **Open choice:** NVS needs no SD card and would work on **teensy40** too,
      and it exercises the same `box_flash.c` the RW612 will use — a real bench
      advantage. SD is architecturally safer (separate device, no XIP write).
      Teensy currently ships the SD backend; NVS was validated from a scratch
      config and is NOT committed.

### Boot-log channel (how the SD crash was finally seen)

A pre-USB fault is invisible over the box's own CDC by definition, and Zephyr's
fatal handler prints through `LOG_ERR` — so with `CONFIG_LOG=n` a hard fault
produces **nothing at all**. That combination is what made this look like a
mysterious "board stops enumerating".

Wiring (Teensy 4.1, `zephyr,console = &lpuart6`, 115200 8N1):

| USB-serial adapter | Teensy 4.1 |
|---|---|
| RX (**green** on the cable used here — verify yours) | **pin 1** (TX, `AD_B0_02`) |
| GND | GND |
| TX, VCC | leave disconnected |

* Read-only needs just those two wires; skipping TX also sidesteps the 3.3 V
  question (the RT1062 is **not** 5 V tolerant).
* **Verify the adapter first** with a loopback (short its RX+TX, echo a string).
  A PL2303**HXA** (`0x067B:0x2303`, `bcdDevice 0x0300`) enumerates and opens on
  macOS but moves no data — it cost an hour here. A genuine FTDI FT232R
  (`0x0403:0x6001`) works.
* Add `CONFIG_LOG=y` + `CONFIG_LOG_MODE_MINIMAL=y` to see faults at all. Note
  `CONFIG_LOG=y` also makes CDC-ACM extremely chatty at INFO level (~86 KB in
  55 s) — fine for debugging, turn it down otherwise.
      On the Teensy 4.1 SD attempt specifically — devicetree is NOT the problem:
      `teensy41.dts` already supplies `usdhc1` with four pinctrl states and a
      `zephyr,sdmmc-disk` child (`disk-name = "SD"`). But our commented-out
      config in `boards/teensy41.conf` is **doubly wrong**: `DISK_DRIVER_SDMMC`
      lives in `drivers/disk/Kconfig.sdmmc`, which is sourced INSIDE
      `if DISK_DRIVERS`, so `CONFIG_DISK_DRIVER_SDMMC=y` without
      `CONFIG_DISK_DRIVERS=y` (which `samples/subsys/fs/fatfs_fstab` sets first)
      is inert — the SD disk driver was never built. That does not explain the
      hard fault (`CONFIG_SDHC=y` still builds the usdhc controller, where init
      died), so fix both before retrying. And add `CONFIG_LOG=y`: that crash was
      debugged blind, and a pre-USB fault is only visible on **lpuart6 (pins
      0/1)** via a USB-serial adapter — the one job that adapter is worth having
      for (needs header pins soldered on this Teensy).
      **Ruled out — do not re-chase:** `teensy41.dts` sets no card-detect method
      (`cd-gpios` / `detect-cd` / `detect-dat3` all absent), which looks
      suspicious but is benign — `imx_usdhc_get_card_present()` falls through to
      `data->card_present = true`, i.e. "assume a card is there".
      Also note Zephyr's `nxp,imx-usdhc` binding is NOT the Linux/NXP-SDK one:
      `cd-gpios`, `pwr-gpios`, `sd-gpios`, `no-1-8-v`, `detect-cd`,
      `detect-dat3` are valid; **`wp-gpios` and `bus-width` are not**, the label
      is `pinmux_usdhc1` (not `pinctrl_usdhc1`), it wants FOUR speed-dependent
      pinctrl states, and the `mmc { compatible = "zephyr,sdmmc-disk"; }` child
      is what the filesystem actually mounts. Pasting an SDK-style usdhc node
      here will not work.
      **Cause of the hard fault: still unknown.**
- [ ] **Watchdog.** `wdt 0|1|test` is advertised in CLI help with no platform
      handler behind it. Zephyr has its own WDT subsystem — do not transliterate
      the Pico's dual-core scheme.
- [ ] **UDP fast-path DO** (`wizchip_udp_do.c`) — no counterpart.
- [ ] **LAN discovery beacon** (UDP :5011, found by extio-setup) — no counterpart.

## Tier 4 — radio (RW612 only)

- [ ] **BLE.** `box_ble.c` exists (multi-peripheral central, frozen `d5e7000x`
      UUIDs) but has **never run on silicon** and does not use the
      `src/core/dserv_ble.h` helpers. Entirely unported: bonding (Just Works +
      LE Secure Connections, allowlist), the pipe relay, the peripheral-latency
      policy, and echo-sync (the 2026-07-18 sub-ms handheld work).
- [ ] **BLE peripheral role** (`box_ble_periph.h`) — no counterpart.
- [ ] **Wi-Fi** — RW612 only; Zephyr's RW612 radio support leans on `hal_nxp`
      blobs, and NXP's own SDK stacks are more mature here.

---

## Suggested order

1. The `config/*` datapoint bug — it silently undermines everything remote.
2. Manifest announce — cheap, unlocks the host tooling.
3. DI groups — unlocks experiment input.
4. Clock sync → scheduled events → obs-sync input, as one unit.
5. Analog (block #7), then RW612-only work once that board is on the bench.

## NEXT: Ethernet RX — DTCM DMA buffers, and the errata mitigation RT10xx is missing

**Status 2026-07-24.** Ethernet publish latency was traced to instruction fetch
from external QSPI flash and largely fixed by relocating the network stack to
ITCM (commit 6072776): `zsock_send` 275 → **61 µs**, loopback RTT 2365 → **783
µs**, manifest-announce loop stall 11.9 → **4.7 ms**. What follows is the
receive-side equivalent, which is *not* done.

### Where the remaining time is

With TX fixed, the loopback splits as `cmd → do_echo ≈ 720 µs` and
`do_echo → di ≈ 3–96 µs` — i.e. ~90 % of what is left is the **inbound** leg.
The box's own share of that is instrumented (`state/dbg/*`) and small:

| stage | µs |
| --- | --- |
| `wake_us` — RX thread saw readability → loop reached `recv` | 30 |
| `recv_us` — the `zsock_recv` that returned the frame | 94 |
| `disp_us` — framer + dispatch + GPIO write (+ the `state/do` publish) | 170 (max 313) |
| **box-internal total** | **~294** |

That leaves **~430 µs unaccounted**: dserv's send path, transit, and the box's
net RX thread processing *before* the socket becomes readable. One inference
narrows it — USB's entire loopback on the same host is 574 µs and USB's outbound
goes through a *Tcl callback in a subprocess*, strictly heavier than dserv's
native `SendClient` write; if the native path cost 430 µs the USB total could not
be 574. So suspicion falls on the box's RX stack, not the host.

### The DTCM opportunity

`drivers/ethernet/eth_nxp_enet.c` selects buffer placement three ways. We
currently take the middle branch, because `CONFIG_NOCACHE_MEMORY=y` and
`ETH_NXP_ENET_USE_DTCM_FOR_DMA_BUFFER` is off:

```c
#elif defined(CONFIG_NOCACHE_MEMORY)
#define _nxp_enet_dma_desc_section   __nocache   /* descriptors only */
#define _nxp_enet_dma_buffer_section             /* buffers: CACHEABLE */
#define driver_cache_maintain        true        /* clean/invalidate every transfer */
```

Only the descriptors avoid the cache. **The DMA buffers are cacheable and the
driver performs cache maintenance on every RX and every TX.** Enabling DTCM sets
`driver_cache_maintain = false`, removing that work entirely rather than reducing
it. DTCM has room: 128 KB, **0 currently used**, against ~10 KB of buffers
(6 RX + 1 TX × 1518).

### Why it is disabled, and what that actually means

```
config ETH_NXP_ENET_USE_DTCM_FOR_DMA_BUFFER
	# ERR050396: ENET writes to TCM require CACHE_ENET to be cleared, while
	# ENET_1G writes to TCM can still corrupt data on Cortex-M7.
	default n if CPU_CORTEX_M7
```

Read precisely: the **fatal** case is `ENET_1G`, which the RT1062 does not have —
it has plain 10/100 ENET. For plain ENET the requirement is conditional:
**CACHE_ENET must be cleared.** The `default n if CPU_CORTEX_M7` is a blanket
that cannot distinguish which ENET instance a given M7 part carries, so it
protects RT1170-class silicon.

**But the precondition is unmet on our SoC.** `CACHE_ENET` here is
`IOMUXC_GPR_GPR13_CACHE_ENET` (GPR13 bit 7), and Zephyr clears it **only for
imxrt11xx**:

```c
/* soc/nxp/imxrt/imxrt11xx/soc.c:858 */
/*  For ENET, clear CACHE_ENET if TCM is used as the write destination. */
IOMUXC_GPR->GPR28 &= (~IOMUXC_GPR_GPR28_CACHE_ENET_MASK);
```

`imxrt10xx/soc.c` has no equivalent. So enabling the option today would put ENET
DMA writes into TCM **without the mitigation NXP requires** — which is why the
blanket default has never been refined per-instance.

### The work

1. Clear `IOMUXC_GPR->GPR13 &= ~IOMUXC_GPR_GPR13_CACHE_ENET_MASK` **before the
   ENET driver initialises** — a `SYS_INIT` ordered ahead of `ETH_INIT_PRIORITY`,
   mirroring imxrt11xx. Getting the ordering wrong is the whole risk: the write
   must land before any ENET DMA starts.
2. Enable `CONFIG_ETH_NXP_ENET_USE_DTCM_FOR_DMA_BUFFER` in `boards/teensy41.conf`.
3. Re-measure `dbg/recv_us`, `dbg/wake_us` and the loopback.

### Verification — latency is NOT sufficient evidence

This is a **silicon errata mitigation**, and a missed mitigation corrupts data
*occasionally*. "The numbers improved" says nothing about whether it is correct.
Before this goes near a rig it needs a sustained soak that checks frame
**integrity**, not timing — the box already emits a monotonic watchdog counter
and self-describing frames, so a long run verifying no gaps, no malformed frames
and no framer resyncs is the bar. For an acquisition box, silent rare corruption
is a worse failure than 400 µs of latency.

### Also pending, cheaper and lower risk

- **Relocate the remaining net libraries to ITCM.** The sockets layer was *not*
  in commit 6072776. NAMING GOTCHA (verified 2026-07-25 against the build's .a
  files): there is **no `subsys__net__lib__sockets` target** —
  `subsys/net/lib/sockets/CMakeLists.txt` never calls `zephyr_library()`, so
  `sockets_inet.c` lands in the **`subsys__net`** library opened by
  `subsys/net/CMakeLists.txt`. Relocate THAT. Also not yet relocated:
  `modules__hal_nxp`'s `fsl_enet.c` (runs in the ISR under irq_lock and does
  `ENET_ReadFrame` per frame — use the FILES form; the whole hal_nxp lib is too
  big) and the per-event kernel files (condvar/queue/poll/sem/work — the wake
  chain between the stack and the service loop). ITCM has ~41 KB free after the
  phase-0 stats build.
- **Preemptive network threads.** We run `CONFIG_NET_TC_THREAD_COOPERATIVE=y`, so
  the RX traffic-class thread runs to completion and the preemptible service loop
  cannot get in. Test `CONFIG_NET_TC_THREAD_PREEMPTIVE=y`, but measure **under
  traffic** — the mirror risk is a starved RX thread dropping packets.
- **Do not confuse packet priority with thread priority.**
  `NET_TX_DEFAULT_PRIORITY` / `NET_RX_DEFAULT_PRIORITY` are *traffic class*
  priorities. The driver's thread knob is `ETH_NXP_ENET_RX_THREAD_PRIORITY`.
- **Instrument `SendClient::send_dpoint` in dserv** if the ~430 µs needs splitting
  definitively rather than by inference.
- **Move sends off the service loop** (own TX queue + thread). Will not make 61 µs
  cheaper, but stops the RT path serializing behind the stack and kills the
  remaining announce stall.

### Upstream angle

`imxrt10xx` is missing a mitigation its sibling SoC implements, which is why a
legitimate optimisation is disabled for a whole family of parts. If the GPR13
clear proves out under integrity soak, it is worth offering upstream — the fix is
small and the reference implementation is already in-tree.

### What is retracted

Every W6300-vs-native-stack number from 2026-07-24 was measured against a
cold-cache path. The recorded conclusion — "hardware TCP offload beats the native
IP stack by ~240 µs per frame, structurally" — **is withdrawn**, not merely
refined: a send now costs 61 µs against the W6300's ~240 µs for a whole frame.
The comparison must be redone (one-way `host_gpio_rtt` delivery on pi5dev, and
the four-way transport table) before any hub decision leans on it.

---

## 2026-07-25 — a measurement audit: what the numbers above are worth

Short version: **treat every latency figure earlier in this document as
provisional.** They were measured through a dserv timebase that changed on
2026-07-24, on a host whose load was not controlled, with a harness that had no
way to detect its own error. None of that means they are wrong; it means nothing
has yet checked them. This section records what was checked, what it changed,
and what still is not trustworthy.

### The timebase changed under us

dserv commit `227315e` ("monotonic timebase for datapoint timestamps", 2026-07-24
16:09) replaced `Dataserver::now()`'s `high_resolution_clock` with `steady_clock`
plus a fixed epoch offset. On Linux — the deployment target and the host for
every number here — `high_resolution_clock` aliased to `CLOCK_REALTIME`. So the
old timestamps rode a clock NTP can step, and the new ones do not.

Every latency figure above predates the dserv build carrying that fix. The
observed effect was not subtle: on the same box, wire, and firmware, updating
dserv moved the measured loop from 1160 µs to 954 µs, and moved the `cmd →
do_echo` leg the *other* way, 655 → 923 µs. Both harnesses were wrong, in
opposite directions, which is exactly the failure mode a single harness cannot
see.

### The cross-check that should have existed

Two harnesses compute the identical quantity, so **they must agree, and their
disagreement is a free correctness check**:

- `wiznet-io/host/loopback_rtt.sh` — arrival(`state/di`) − timestamp(`cmd/do`)
- `lb_split.sh` — the same span as `L1 + L2`, split at the `state/do` echo
  (`lb_tot` column, added 2026-07-25)

Run both. If they disagree, *stop* — the measurement apparatus is broken and no
conclusion drawn from it survives. Tonight that check was the whole story:

| dserv | box | transport | `loopback_rtt` | `lb_tot` | gap |
|---|---|---|---|---|---|
| pre-fix | teensy41 | eth | 1160 | 696 | ~465 µs |
| 0.48.10 | teensy41 | eth | 954 | 964 | ~0 |
| 0.48.10 | teensy41 | **USB** | 1143 | 717 | **~426 µs** |
| 0.48.10 | office (W6300) | eth | 676 | 654 | ~0 |

Two boxes, two MCUs, two firmware lines agree over Ethernet under 0.48.10. **The
USB path still does not.** Those frames do not arrive through dserv's client
socket; they come up through the `extio` subprocess and `modules/usbio/usbio.c`.
One asymmetry there worth a look: `usbio.c:122` *preserves* a box-supplied
timestamp (`if (!dpoint->timestamp)`), so USB datapoints can carry box-clock
times where native ones carry host time. That should not reach these numbers —
both harnesses stamp with `[now]` inside the callback rather than reading
`dservTimestamp` on a box datapoint — so the cause is **unidentified**. Until it
is, **no USB latency number here is usable.**

### What was tested and ruled out

Recorded because each cost real time and none should be re-run:

- **The phase-0 statistics build is free.** A/B against a pristine build with
  `NET_PKT_RXTIME/TXTIME_STATS[_DETAIL]`, `NET_STATISTICS[_USER_API]` all `=n`:
  min 844 vs 843, median 1163 vs 1123 — identical floor, medians inside run
  spread. Leave the instrumentation on.
- **dserv's script dispatch has no cold-wakeup cost.** Measured with no box and
  no transport (set a datapoint, time until its callback runs): 116 µs cold vs
  130 µs warm. Warm is marginally *slower*, as two dispatches should be. An
  earlier hypothesis that ~450 µs of cold dispatch was hiding in the callback
  path is **refuted**.
- **Not harness aliasing.** `SETTLE` at 0.02 / 0.037 / 0.071 → 1160 / 1163 /
  1161 µs. The effect is insensitive to iteration period across a 3.5× spread.
- **Not differing box behaviour.** The box publishes `state/do/<n>` whether or
  not anything is subscribed to it (verified: 47 ms fresh after a run with no
  match and no script). Frame output is identical in both harness
  configurations.
- **Host load scales everything.** On a busy Mac (load 3.04, WindowServer 28 %,
  `stim2`, Roon) both transports inflated ~1.4× versus the same rig quiet, while
  the Eth ÷ USB ratio held at 1.36 → 1.38. Absolute µs are not portable across
  sessions; ratios largely are. **Measure both transports in the same session or
  report neither.**

### Corrected numbers, such as they are

pi4dev (Pi 4B, governor `performance` @ 1.8 GHz, no throttling, load ~0.0;
`eth0` = 192.168.11.10/24, box at .41), dserv 0.48.10, phase-0 stats build:

| leg | teensy41 (Zephyr, eth) | office (W6300) |
|---|---|---|
| total loop | 954–964 | **654–676** |
| L1 `cmd → do_echo` | 923 | **581** |
| L2 `do_echo → di` | 37 | 73 |
| box-internal (`recv`+`disp`) | ~258 | — |
| unaccounted in L1 | **~665** | — |

The unaccounted figure is now ~665 µs, not the ~430 µs recorded above. That
number is the entire premise of the DTCM/ERR050396 work, and it was derived from
an L1 measurement the timestamp fix moved by 270 µs. **Recompute it before
touching the errata mitigation** — that item is the riskiest on the list (a
silicon workaround needing a frame-integrity soak) and it should not be
justified by a number measured through a since-fixed clock.

### The W6300 comparison, and the retraction above

Measured the same evening, same host, same dserv, same harness: the W6300 box's
inbound leg is **~342 µs faster** (581 vs 923). This is the redo the retraction
above demanded, and it points back toward the withdrawn conclusion rather than
away from it — now at ~342 µs rather than the original ~240 µs.

Hold that loosely. It is one pair of runs, and it is **not a transport-only
comparison**: pico2 vs RT1062, bare-metal vs Zephyr, and the ICMP floor already
differs (0.077 ms vs 0.197 ms). It is sufficient to say the retraction is no
longer supported by evidence; it is *not* sufficient to reinstate the original
claim. The four-way table still needs running.

Also logged: `loopback_rtt` on the W6300 hit a single **25,966 µs** outlier in 59
samples (p99 957, so one event rather than a fat tail). A 26 ms stall on an
acquisition box is worth knowing whether it recurs.

### Does the DO echo delay the DI it causes? Yes — by 62 µs, and it does not matter

Asked because the loopback's `L2` leg carries an extra frame the pure input path
would not. Both publishes are **blocking `box_net_client_send()` calls on the one
service loop**: `publish_do()` fires inline in the command handler right after
`pico_gpio_exec()` (`wizchip_dserv_config.c:1427`), while the DI edge can only go
out on a *later* pass, when `pico_gpio_poll_di()` drains it (`:1859`). So the DI
frame's transmission queues behind the echo's. The Zephyr box has the same
structure — this document's own `disp_us` row is "framer + dispatch + GPIO write
(+ the `state/do` publish)".

Both frames are nonetheless **edge-stamped**: `publish_do` uses
`event_stamp(t_act)` taken at the pin write, `publish_di` uses
`event_stamp(e.t_us)` taken at the IRQ. So the box's *record* of when things
happened is independent of when it managed to send them. `host/lb_queue.sh`
separates the two (office box, GP10→GP11, n=60):

| quantity | median |
| --- | --- |
| `q_box` = `dservTimestamp(di) − dservTimestamp(do)` (both box clock) | **−1 µs** |
| `q_arr` = arrival(di) − arrival(do) (both host clock) | 62 µs |
| `q_que` = `q_arr − q_box` — the serialization | **62 µs** (p90 126, p99 214) |

Each term is a difference *within* one clock, so the box-clock offset and its
~98 µs sync jitter cancel; only rate error over ~100 µs survives, which is
nothing. That is what makes `q_que` trustworthy despite mixing two clocks.

`q_box` at −1 µs is also a **positive control**: the jumper and the IRQ cost
nothing measurable, and the slight negative is correct rather than noise —
`publish_do` reads `time_us_64()` *after* `pico_gpio_exec()` returns, while the
DI IRQ fires at the transition itself, so the edge stamp legitimately precedes
the write stamp by a microsecond. Both are real edge stamps, not drain-time
artifacts.

Three conclusions:

1. **The DI timestamp is unaffected, which is the part that matters.** Queueing
   delays when dserv *learns* of an edge, not the time recorded for it. Event
   logging, the obs timeline, and RT computed from a box onset stamp are all
   edge-accurate regardless. Only notification latency moves.
2. **The loopback figure is inflated ~62 µs on ~654 µs, about 9 %** — carrying
   the extra frame does not badly distort it, but `host_gpio_rtt.sh` remains the
   correct harness for pure input latency, because the host generates the edge
   and no echo is in the path.
3. **The marginal cost of a second frame out the W6300 on the RT path is 62 µs,
   not ~240 µs.** This is independent evidence against the figure behind the
   retracted claim above, and it explains why `L2` at 73 µs never squared with a
   supposed 240 µs send. It does not establish what the original 240 µs
   measured — one more reason the four-way table needs rerunning.

Note the echo exists **only for `GPIO_OP_SET`**. A scheduled pulse (`do/<n>/at`)
goes through `sched_arm()` and publishes nothing, so `lb_split` and `lb_queue`
return no samples on that path.

### Still missing: a hardware reference

Both harnesses stamp inside a Tcl callback via `tclserver_now()` — they share a
mechanism, so their agreement is evidence but not proof. Nothing measured so far
is referenced to a real electrical edge. Now that the boxes are on a Pi,
`host_gpio_rtt.sh` with its native-GPIO baseline can close that gap, and it is
the one measurement a software timestamp bug cannot fool. Wire it per the
`box_out_rtt.sh` header diagram — **not** the one in `host_gpio_rtt.sh`, whose
header puts the native baseline on GPIO22 while `box_out_rtt.sh` drives GPIO22
from the box (baseline belongs on GPIO23 / phys 16). Following the wrong diagram
puts two drivers on one pin.

### Operational notes

- **Flashing a Teensy from a dev host needs the soft-reboot flag `west flash`
  omits:** `teensy_loader_cli --mcu=TEENSY41 -s -v build-teensy41/zephyr/zephyr.hex`.
  Without `-s` it fails with "Unable to open device". There is no remote path:
  `main.c:294` — `cmd/bootsel` is unsupported on Teensy because the bootloader is
  a separate chip watching the Program button. HalfKay does not wipe NVS; pin
  config survived every reflash tonight.
- **A box that vanishes leaves stale datapoints.** `dservctl extio
  "extio_clear_dead"` (config/extioconf.tcl:141) clears `state/*` and `decoded/*`
  for boxes no longer in `::extio_known`.
- **Harnesses live in `~/extio-bench/` on pi4dev**: `loopback_rtt.sh`,
  `lb_split.sh`, `host_gpio_rtt.sh`, `box_out_rtt.sh`, plus `dispatch_test.sh`
  (dserv-only dispatch latency) and small Tcl state-dump helpers.
- **Office box loopback** is wired GP10 → GP11 (adjacent header pins 14/15), both
  free per `wiznet-io/PINMAP.md` with the MCP3204 and OLED enabled.

### What not to depend on yet

1. Any USB latency number, until the harness disagreement is explained.
2. The ~430 µs unaccounted figure and the DTCM case resting on it.
3. Any absolute µs measured on an uncontrolled host.
4. The W6300-vs-native margin as a *transport* result — it is currently a
   whole-box result.

---

## 2026-07-26 — THE WIZNET REFERENCE SET (electrically referenced)

**These are the numbers to compare against. Everything earlier in this document
is superseded** — see "What this supersedes" at the end of the section.

### The requirement, stated

**Within 1 ms end-to-end is fine for this work.** Recorded because it changes
which items are worth doing: it is the difference between "the DTCM errata
mitigation is the next task" and "the DTCM errata mitigation can wait
indefinitely". Latency work below this line is optimisation; timestamp accuracy
and tail behaviour are not, because neither is bounded by the same budget.

### Setup

Host `rpi500` (Pi 5, governor `performance`, load ~1.2 with the ess_control
Firefox page CLOSED — see the load caveat below), dserv **0.48.10** (has the
`227315e` monotonic timebase fix). Box `upstairs`: pico2, `build = dual`,
`transport = w6300`, fw `0.48.7-2-ge9b1e1a`, 192.168.88.28 → 192.168.88.29:4620.

```
Pi phys 13 (GPIO27, out) ──┬── Pi phys 16 (GPIO23, in)    [native baseline = P]
                           └── box phys 14 (GP10, in)     "from_pi"
Pi phys 15 (GPIO22, in) ───── box phys 15 (GP11, out)     "loop_out"
                                     └───────────────────  box phys 16 (GP12, in) "loop_in"
Pi phys 14 (GND) ──────────── box GND
```

The baseline jumper is **Pi-to-Pi**, not to the box. It measures the host's own
input path off the *identical* electrical edge, which is the only way to remove
the host from the box's number.

### Measured

| measurement | med | min | p99 | max |
| --- | --- | --- | --- | --- |
| **Input: edge → dserv knows** (via box) | **322** | 278 | 358 | 365 |
| Same edge, Pi sensing directly (**P**) | 72 | 50 | 99 | 110 |
| **Output: dserv cmd → Pi sees box pin move** | 329 | 288 | 393 | 393 |
| Loopback total (`loopback_rtt` / `lb_split`) | 676 / 657 | 632 / 610 | 717 / 718 | 717 / 718 |
| `L1` cmd → do_echo | 554 | 517 | 679 | 679 |
| `L2` do_echo → di | 100 | 39 | 139 | 139 |

### Derived, and the budget closes

- **Outbound** (dserv command → box pin physically moves) = 329 − 72 = **~257 µs**
- **Return** (box event → dserv knows) = 554 − 257 = **~297 µs**
- Cost of routing an edge through the box vs the Pi sensing it directly
  = 322 − 72 = **~250 µs**

257 + 297 = 554, which is `L1` measured independently by a different harness.
That closure is the point: the split was unavailable last night (the box-clock
route gave ±64 ms of accumulated drift) and the electrical reference recovers it.

### Verdict against the requirement

**Input latency 322 µs median, 358 µs p99** — the number that governs "subject
acts → dserv knows". ~2.8× margin at p99, no outliers in 149 samples. This box
is comfortably inside spec and its tail is the tightest measured anywhere in this
exercise.

### Caveats attached to these specific runs

- **The box was UNSYNCED.** It power-cycled during wiring (`uptime_us` reset,
  `watchdog` 1275 → 942) and had no obs anchor after. So `box_clock_stamp()`
  returns 0 and **dserv arrival-stamps** those frames — the harness's "box stamp
  error" column is therefore *delivery measured from the arrival stamp*, NOT
  clock accuracy. The `sync/*` datapoints visible at the time were 48 minutes
  stale and sticky in dserv's table, which is precisely the trap
  `host_gpio_rtt.sh`'s header warns about. Read that column as clock accuracy
  ONLY after confirming a fresh anchor.
- The 41 µs between that column (281) and the delivery column (322) is dserv's
  **arrival-stamp → callback-execution** gap. Free measurement, worth knowing.
- **A power cycle reverts unsaved config.** Pin modes and labels survive (NVS);
  anything set at runtime and not followed by `cmd/save` does not.
- `host_gpio_rtt.sh` labels the box column "(USB)" — hardcoded, wrong for a
  w6300 box, cosmetic.
- **Host load dominates everything.** The ess_control page open in Firefox on
  rpi500 put the box at load 3.36 (Firefox 38.9 % + web content 29.1 %). Close it
  before measuring. On a loaded Mac the same effect inflated *both* transports
  ~1.4× while leaving their ratio intact.

### The same box on USB-FS — and why Ethernet wins here

Same host, same dserv, same jumpers; Ethernet unplugged so the arbiter falls back
(`transport = usb`, `ip = 0.0.0.0`). The box power-cycles when the cable moves, so
debounce was re-applied.

| | Ethernet (W6300) | USB-FS |
| --- | --- | --- |
| **Input: edge → dserv knows**, med | **322** | 525 |
| — min | 278 | **214** |
| — p99 | **358** | 1008 |
| **Output: cmd → pin moves**, med | **329** | 393 |
| — p99 | **393** | 1147 |
| Loopback, med | **676** | 1043 |
| — p99 | **718** | 1430 |
| Native baseline `P` | 72 | 72 |

**USB's floor is 64 µs FASTER** (min 214 vs 278) — `box_net_usb_client_send` is a
memcpy into a ring plus a TX interrupt enable, so the wire time happens in the ISR
and the cost is *off the RT loop by construction*, whereas the Ethernet send IS
the stack traversal, inline on it. What ruins USB-FS is **frame quantization**:
1 ms slots, so the median pays ~267 µs of waiting and p99 pays ~650 µs. At p99
USB-FS **breaches the 1 ms requirement** (1008 µs input, 1430 µs loopback) while
Ethernet keeps 2.8× margin. Ethernet is correct for this box, for a structural
reason rather than a measured preference.

`P` came back at **72 µs median in both runs**, across a transport change and a
dserv restart. The host's input path is a stable constant; that is what makes
subtracting it legitimate.

**Projection for USB-HS (RT1062), stated as arithmetic, not a result.** On this
box USB's non-quantization path is ~64 µs cheaper than Ethernet's. High-Speed
cuts the slot interval 8× (1 ms → 125 µs microframes), so the quantization term
should fall to ~33 µs median / ~80 µs p99, leaving USB-HS net faster than
Ethernet with a far tighter tail. Two things that arithmetic does not capture:
the host-side `extio`/`usbio` hop is unchanged by HS and is already inside these
numbers, and the Teensy's Ethernet is Zephyr's *software* stack rather than
hardware TCP offload, so the baseline it must beat is a slower one. Both push the
same way. **Measure it; do not cite this paragraph as a finding.**

### Hardware obs-sync: anchor quality, measured against a real edge

Wired **Pi phys 37 (GPIO26, out) → box phys 31 (GP26)** and set `sync/pin 26`
(persisted). Note the config key is **`config/sync/pin`**, not `config/sync_pin`
— the state datapoint and the config key differ, and setting the wrong one
silently does nothing (`state/sync_pin` stays `-1`).

The sync pin is **latch-only** (`pico_gpio.h:133`: "TTL obs-sync: latch, don't
report") — it publishes no DI events, so a sync line costs zero wire traffic.
On an `ess/in_obs` frame the box pairs that frame's *dserv* timestamp with the
**latched edge time** if the edge is fresh (`SYNC_EDGE_WINDOW_US` = 250 ms),
giving `source = hw`; otherwise it degrades to frame-arrival time (`sw`) and says
so. `box_clock_sync` is `offset_us = dserv_us - box_us` with `box_us` = the
latched edge — **no transport term**, which is the whole point.

#### Anchor repeatability, and the crystal

Five successive anchors, same transport, ~2.5 s apart:

```
offset ...926431 / ...926368 / ...926309 / ...926246 / ...926186
deltas      -63       -59       -63       -60   us
```

**Repeatability ~±2 µs.** The offset does not scatter; it marches monotonically
at ~60 µs per ~2.5 s = **~25 ppm**, which is the box's oscillator, in the
documented +27→+43 ppm band. The anchor is precise; the crystal is what moves —
and `hw` anchors teach the rate, so this gets corrected rather than accumulating
into the 64 ms-after-31-minutes seen on `office`.

Per-event **sync quality (spread) is ~90–125 µs**, matching the recorded 98 µs
Tier A figure.

#### Standing bias is a HOST-side property, not the box's

The bias moved from **+3114 µs to −243 µs** by changing nothing but how tightly
the edge and the `ess/in_obs` publish were coupled on the host — two separate
`dservctl` invocations versus both in one interp call. The hw anchor removes the
box-ward *transport* delay; it does **not** remove host-side skew between driving
the pin and stamping the datapoint. Whatever gap exists there lands directly in
the offset. Couple them, or characterise the skew and calibrate it out.

The box reports the frame-behind-edge delay per anchor as
`state/sync/transport_us` — 2355 µs with the loose two-call sequence, **727 µs**
on USB-FS and **206 µs** on Ethernet once coupled. Free per-anchor transport
telemetry, and a direct readout of what a `sw` anchor would have absorbed as
error.

#### Two things left open, recorded rather than tidied away

- **A 564 µs bias difference between a USB-anchored and an Ethernet-anchored
  run** is unexplained. It is not anchor noise (that is ±2 µs), but a **reboot
  intervened** between them, resetting the rate estimator — so they are not a
  clean pair, and the code has no transport term that could explain it. A clean
  transport A/B is awkward here because **switching transport requires a
  reboot** (see below).
- **Do not copy this section's anchor spacing into a test.** Two anchors 1 s
  apart clears `BOX_CLOCK_PAIR_MIN_US` (0.5 s) and therefore teaches the rate
  from a *one-second baseline* — and the first valid sample is taken as the rate
  outright (`if (!c->rate_valid) c->rate_ppb = sample`). Tens of µs of edge noise
  over 1 s implies tens of ppm. Real obs anchors are spaced by trial durations
  and are a far better baseline; the rate quality induced here is worse than a
  running experiment would produce.

#### Uncertainty budget WITHOUT a hardware sync line

The decisive detail is in `box_clock_sync`: **a `sw` anchor never teaches the
rate.** `if (!trusted) return;` sits above the rate estimator, so only `hw`
anchors reach it. Without a sync line the box has no rate correction at all and
its ~25 ppm crystal error accumulates freely between anchors — the mechanism
behind the 64 ms found on `office` after 31 idle minutes.

A `sw` anchor pairs the frame's *send* timestamp with the box's *receipt* time,
i.e. it assumes delivery is instantaneous. Every stamp then reads **early by
roughly the one-way delivery**, which `transport_us` measures directly.

| source of error | Ethernet | USB-FS |
| --- | --- | --- |
| Standing bias (= one-way delivery) | ~200 µs | ~730 µs |
| Anchor-to-anchor jitter | ~±50–100 µs | up to ±400 µs (frame quantization) |
| Drift between anchors, 25 ppm | 125 µs per 5 s | same |
| Per-event sync jitter | ~90–125 µs | ~90–125 µs |
| **Total, anchors every ~5–10 s** | **~400–600 µs** | **~1.0–1.3 ms** |

**Verdict against the 1 ms requirement: Ethernet without a sync line is fine**
(about half the budget) *provided obs anchors keep coming*. **USB-FS without a
sync line is marginal to over budget**, because the 1 ms frame quantization lands
directly in the anchor.

**The operating rule:** at 25 ppm, drift alone eats 1 ms in **40 seconds**, so
re-anchor at least every ~10 s. Normal `beginobs`/`endobs` cadence satisfies this
easily — `ess/in_obs` toggles twice per trial. The danger case is not a running
experiment but an **idle box between obs**, where the offset silently rots.

**What often makes this moot:** the standing bias is *systematic*, so it cancels
wherever both endpoints are box-stamped — button press → release, DI edge →
scheduled pulse, response latency from a box onset stamp. Those see only the
~90–125 µs jitter and are well inside budget with no sync line at all. The bias
applies in full only to **box-to-host** intervals (a box event against a stimulus
onset stamped by dserv or stim2), which is exactly the case the sync line fixes.

#### Why the wire beats every packet transport, and what that implies

Latency for one 128-byte dserv frame decomposes into **waiting for a slot** plus
**serialising the bits**. Different transports fail in different halves:

| transport | wait for a slot | serialise 128 B | total |
| --- | --- | --- | --- |
| UART 115200 | 0 | **11.1 ms** | 11.1 ms |
| UART 1 Mbaud | 0 | 1.28 ms | 1.28 ms |
| UART 3 Mbaud | 0 | 0.43 ms | 0.43 ms |
| UART 12 Mbaud | 0 | 0.11 ms | 0.11 ms |
| USB-FS | 0–1000 µs | 85 µs | ~0.6 ms avg |
| USB-HS | 0–125 µs | 2 µs | ~64 µs avg |
| **Ethernet 100M** | **~0** | **~10 µs** | **~10 µs** |

A true UART has **no bus schedule** — the shift register starts when it is free —
which is the property USB lacks. But it pays in serialisation, so classic 115200
is far *worse* than USB-FS. **Ethernet already provides the "unpaced" property
with 100× less serialisation**, which is exactly what the measurements show
(Ethernet 322/358 µs vs USB-FS 525/1008). The enemy is USB's polled
architecture, not modernity — and this vindicates the 2026-07-09 decision in
BLE.md to supersede the UART sidecar once rigs went wired-Ethernet.

Two traps if serial is ever revisited: a UART behind a **USB-serial adapter
reintroduces USB pacing** (FTDI latency timer defaults to 16 ms of batching), and
a real UART's RX interrupt typically fires on a FIFO threshold or a ~4-character
idle timeout, so a short message can sit in the FIFO. "No pacing" is a property
of the wire, not of the driver stack above it.

**The synthesis: the packet carries the data, the wire carries the time.** The
TTL sync line is the one-bit limit case of a parallel interface — no framing, no
arbitration, no serialisation — which is why it anchors to ~±2 µs while every
packetised path sits in the hundreds of µs. Those are two different jobs and it
is correct for them to use different media.

#### Software-only anchoring, for hosts with no GPIO

The standard method is PTP/NTP's four-timestamp exchange — host sends at `T1`,
box receives at `T2`, replies at `T3`, host receives at `T4`; then
`RTT = (T4-T1) - (T3-T2)` and `offset = T2 - (T1 + RTT/2)` — with **min-RTT
filtering** across probes, keeping the least-queued sample where the symmetry
assumption is least wrong.

**Most of this already exists in tree.** The BLE echo-sync work built exactly
this pattern (min-RTT estimator feeding `box_clock`) and took the handheld from
**+22.6 ms to +0.37 ms using software timestamps only**. Porting it to the wired
path is reuse, not invention. It also closes the rate gap: `sw` anchors never
teach the rate, an echo estimator naturally can, and **drift is the failure mode
that produces 64 ms surprises** while bias is merely calibratable.

**Certify it against `upstairs`.** That box has a hardware sync line *and* can run
the software estimator, so the software answer can be measured against the
hardware one on the same box at the same time. That converts "probably good
enough" into a number, after which it can be deployed on GPIO-less hosts knowing
exactly what was given up. Same validate-the-method-once pattern used throughout
this document.

#### The `sw` anchor bias IS the one-way delivery — measured, not assumed

Alternating `hw` and `sw` anchors on the same box, bracketing each `sw` sample
between two `hw` samples to interpolate out the ~25 ppm drift:

```
sw - hw = -202 us        (predicted -206 us from state/sync/transport_us)
```

Exact agreement, in the predicted direction. A `sw` anchor pairs the frame's
*send* timestamp with the box's *receipt* time, i.e. assumes delivery is
instantaneous, so every stamp reads early by exactly `d`. Script:
`host/sw_bias.sh`.

#### Stage 1: host-only bias correction, certified

`dservSetData <var> <ts> <type> <bytes>` honours an explicit timestamp
(`Dataserver.cpp`: `if (!ts) ts = ds->now()`). So publishing the sync datapoint
**forward-dated by `d`** makes the naive `sw` anchor correct **with no firmware
change** — the box computes `offset = dserv_us - receipt_box`, and `receipt_box`
genuinely corresponds to dserv time `T_send + d`.

`d` is estimated without any wire by halving the **minimum** RTT of the existing
`cmd/do → state/do` echo (least-queued sample = where the symmetry assumption is
least wrong). Certified against the hardware line on the same box
(`host/stage1_certify.sh`):

```
echo RTT: min 500  med 536 us   ->  d_est = min/2 = 250 us
  sw UNCORRECTED  vs hw:  -190 us
  sw CORRECTED    vs hw:   -34 us
```

**Bias ~190 µs → ~34 µs, host-side only.** 34 µs is below the anchor's own
jitter floor (~90–125 µs), so at this resolution the bias is gone.

**Do not over-trust the residual.** It is the difference of two errors that
happened to partially cancel: `min-RTT/2` = 250 µs *over*-estimates the true
`d` = 206 µs by ~44 µs (the echo includes the box's dispatch and GPIO write,
which the anchor delay does not), while the forward-dating *under*-applies
because `[now]` is evaluated some tens of µs before the frame actually leaves
dserv. On another host or transport they will not cancel the same way. The
dominant term (`d` itself) is measured per-deployment, so the method adapts; the
residual does not. **Re-certify per deployment**, and note bracket interpolation
contributes its own ~±50 µs, so read this as "indistinguishable from zero at this
resolution", not "exactly 34".

**Suppress the correction whenever `state/sync/source = hw`.** Forward-dating a
frame whose anchor comes from a latched edge would *introduce* precisely the
error this removes.

`d` is **per-box and per-transport** (206 µs Ethernet, ~730 µs USB-FS), so a
multi-box rig needs per-box values.

#### Stage 2: implemented, deployed, certified (2026-07-26)

Built and OTA'd to `upstairs`; see `config/extio_sync.tcl` (host) and
`CFG_PROBE`/`CFG_SYNC` in `common/dserv_config.h` +
`pico/wizchip_dserv_config.c` (box).

**Protocol.** Two keys under the box's existing `%match <pfx>/cmd/*`, so **no
registration change**:

```
cmd/probe <seq>   -> state/probe = the box's OWN turnaround (t_send - t_recv).
                     Both reads are box-clock, so the box's unknown offset
                     cancels and the host can subtract box time from the RTT.
cmd/sync  <d_us>  -> offset = (frame_ts + d_us) - receipt_box, trusted=0.
                     Skipped when a fresh latched TTL edge exists.
```

**NAMED `probe`, NOT `echo`.** `state/echo/*` is already the BLE echo-sync
telemetry (`echo/synced`, `echo/rtt_us`, `echo/offset_us`, `echo/rate_ppb`). The
leaf `state/echo` would not literally collide, but two unrelated sync mechanisms
sharing that word on a `BOX_BLE` build is a trap.

**Certified against the hardware line, same box, same session:**

| method | stamp error (median) | spread | vs `hw` |
| --- | --- | --- | --- |
| `sw` uncorrected | −202 µs | — | 274 µs |
| **`sw` + correction** | **−100 µs** | ~70 µs | **172 µs** |
| `hw` TTL edge | +72 µs | ~83 µs | — |

The correction **halves the bias**, and all three are inside the 1 ms
requirement. The durable win is not the bias but the **cadence**: anchoring every
2 s bounds drift at ~50 µs regardless of obs spacing, which is the failure mode
that produced 64 ms on an idle box.

**The 172 µs residual is a modelling error, not noise.** `d = net/2` halves a
round trip whose legs are not comparable: the return leg includes **dserv's
~116 µs Tcl dispatch** (measured independently, `host/dispatch_test.sh`) while
the outbound leg does not. Note a textbook four-timestamp NTP solve does **not**
fix this — that formula assumes symmetry too. The actual fix is host-side: take
`T1` from `dservTimestamp(cmd/probe)` and `T4` from
`dservTimestamp(state/probe)` (dserv arrival-stamps it, since the box sends
timestamp 0) instead of bracketing with `[now]` in Tcl. That excludes host
*processing* from both ends and leaves something much closer to path delay.

#### The box arithmetic is EXACT — the open question is host-side

Sending `cmd/sync 200` and reading the anchor inputs the box already publishes:

```
ds + d - bx = 1785002151935731 + 200 - 1265180110 = 1785000886755821
off (state/sync/offset_us)                        = 1785000886755821   ✓
```

Exact to the microsecond. `publish_sync` has always emitted all three inputs
(`sync/dserv_us` = `m.timestamp`, `sync/box_us` = `now_box`,
`sync/offset_us`, plus `transport_us` = applied `d`), so the anchor was auditable
the whole time — it just was not read. **Check those four datapoints before
theorising about a sync discrepancy.**

Consequence: the unexplained result below is NOT in the firmware. Changing the
host estimator moved `d` by 38 µs but the measured bias by 176 µs, and since the
box demonstrably applies `offset += d` at 1:1, the inconsistency lives in the
host-side `d` estimate or in the harness. Still open, but a much smaller haystack.

#### Never anchor mid-obs

Re-anchoring **steps** the offset. Applying that inside a data-collection window
puts two events of one trial on different mappings and silently corrupts any
interval computed across the step — precisely the failure dserv's own monotonic
timebase commit (`227315e`) exists to prevent on the host side. **Obs boundaries
are the only moments where a clock discontinuity is provably harmless**, and they
already anchor; they are also frequent during real experiments and always outside
real data.

So both `cmd/probe` and `cmd/sync` are gated on `!g_in_obs` **in the box** (it
owns the flag, closing the race where an obs begins while a frame is in flight),
and the host skips too so the frames are not even generated. The probe matters
less but is gated anyway: a reply is a blocking send on the one service loop, so
it delays a concurrent DI event's *delivery* by ~62 µs (p99 214) — timestamps are
edge-stamped and unaffected, but there is no reason to spend it during data
collection when `d` is stable enough that an estimate one trial old is fine.

**The ticker's job is the IDLE GAP between obs, not the experiment.** That makes
it strictly additive to a mechanism already correctly placed, and it means an
imperfect `d` can only ever act while no data is being collected — the next obs
boundary re-anchors off the better path regardless.

Verified on silicon: `cmd/sync 200` outside obs → `src=swc d=200`; `cmd/sync 333`
during obs → ignored (`d` stays 200); `cmd/probe` during obs → no `state/probe`
at all.

#### `sync/source` has three values, not two

`hw` (latched TTL edge) / `swc` (arrival-anchored but delay-corrected via
`cmd/sync`) / `sw` (naive arrival, wrong by exactly the one-way delay). A
datafile now records **which** method produced a timestamp instead of leaving it
to be inferred after the fact.

#### Two wrong guard policies, recorded so they are not re-derived

The host must decide which boxes to sw-correct. Two plausible answers both fail:

1. **`state/sync/source == "hw"`.** That datapoint is **STICKY** in dserv's
   table — it survives box reboots and means "synced at some point", not "synced
   now". A 31-minute-old `hw` suppressed correction forever on a box whose line
   had since been unplugged. (Same trap `host_gpio_rtt.sh`'s header warns about.)
2. **...plus a freshness window.** Fails the other way: `hw` anchors arrive at
   obs boundaries, minutes apart between blocks, so *any* window eventually lets
   the host stomp a good `hw` anchor with a worse `sw` one. Observed directly — a
   100-sample run took 140 s, aged the anchor past a 30 s window, and the host
   took over mid-experiment. **Alternating two methods that disagree by 172 µs is
   worse than committing to either.**

**Correct signal: `state/sync_pin >= 0`** — announced in the manifest, a stable
property of the box, and it answers the question actually being asked ("does this
box have a wired anchor?"). Probe *unconditionally* regardless, so `d` stays warm
and losing the line degrades instantly rather than after a warm-up.

#### The OTA path, which worked cleanly

`sh build.sh dual --tbyb --push` (needs `DSERV_AGENT_FIRMWARE_TOKEN`) publishes
bin=TBYB trial + uf2=hashed slot-A base to the `dev` shelf, then
`dservctl extio "extio_ota_push_shelf <box> dev"`. New firmware was running in
~5 s, `ota/state = committed`, and NVS config (pins, labels, `sync_pin`)
survived. This is the flash path when the box is on a remote host and the
toolchain is not.

#### A hardware reference at last — and what it revealed (2026-07-26, later)

dserv now stamps GPIO edges with the kernel's `gpio_v2_line_event.timestamp_ns`
instead of `tclserver_now()` (commit `b0b1f8d`). That makes the tee'd baseline
input a genuine hardware time reference, so `host/clock_err.sh` can difference
**one physical edge stamped independently at both ends** — Pi kernel IRQ vs box
IRQ — with no software stamp, no delivery time and no Tcl callback in the path.

| sync mode | clock error (median) | spread |
| --- | --- | --- |
| **`hw` (TTL edge)** | **+35 µs** | **19 µs** |
| `swc` (delay-corrected) | −167 µs | ~4 µs (bulk) |
| `sw` (naive arrival) | −181 µs | ~4 µs (bulk) |

**The hardware anchor is far better than this document previously claimed.** The
+72 µs / ~83 µs spread recorded above was mostly the HOST's own software input
path (~72 µs median, p99 ~100) being attributed to the box clock. **Supersede
that figure with 35 µs / 19 µs.** Every earlier GPIO-referenced number carried
the same contamination.

**Incidental, and a genuine positive:** in `sw` mode the clock ran **130 s with
no anchor at all** and held a ~4 µs spread. The rate correction — taught by
earlier `hw` anchors — works properly. It is the *bias* half of the mechanism
that does not.

#### The correction applies ~195 µs and delivers ~14 µs

`swc` vs `sw` differ by 14 µs while `d = 195` demonstrably reaches the box and is
applied (`swc` is published only from that branch, and `offset = ds + d - bx` was
verified exact to the microsecond). The offset moves; the stamps do not follow.

**This is a violated identity, i.e. a bug — not the estimation difficulty below.**
Keep the two separate. Remaining suspects: the anchor in force during the run is
not the one assumed, or `box_clock_stamp`'s rate term absorbs the change (`corr`
scales with time-since-anchor, and anchor cadence differs between the modes
compared). Both are settled by logging `offset_us` and `anchor_box_us` alongside
a stamped event. Do that rather than guess again.

#### Why the ~200 µs has been so hard to pin down and subtract

Recorded because the reasons are structural and will recur on the NXP port:

- **One-way delay estimated from a round trip over an asymmetric path.**
  `d = RTT/2` is valid only if the legs match. Ours do not: the return leg
  carries dserv's ~116 µs dispatch, the outbound leg never touches it.
  Min-filtering removes queueing noise, **not** systematic bias. This is the
  canonical limit in clock sync and is exactly why PTP exists with hardware
  timestamps at both ends rather than as a better software estimator.
- **The target is the same size as the instruments' own latencies:** host
  dispatch 116 µs, host input path 72 µs, box turnaround ~60 µs, USB
  quantization up to 1000 µs, loose obs-anchor coupling 3100 µs. Each had to be
  found and removed before the target became visible, and three times a "result"
  turned out to be one of them.
- **`d` is not a scalar.** It differs by transport (206 vs ~730 µs), by
  direction, plausibly by frame type, and inflates ~1.4× under host load.
- **Two variables kept moving at once** — reboots between comparisons, transport
  switches that *require* a reboot, dserv restarts, config changes. Most
  comparisons carried a confound, which is why conclusions kept reversing.

**The hardware line does not solve this; it dissolves it.** A wire has no
asymmetry to estimate, which is the whole of why 35 µs / 19 µs falls out.

**The shortcut not yet used:** `state/sync/transport_us` on a hw-equipped box
**is** `d`, measured hardware-to-hardware. So a box with a TTL line can act as a
**delay calibrator for the fleet** — measure `d` once there, hand that constant
to boxes without a line on the same host and transport. One careful measurement
beats a permanently-running estimator that cannot see its own asymmetry.

**Recommendation.** Use the TTL line wherever any trigger source exists. On a
GPIO-less host expect ~180 µs of *systematic* bias — inside the 1 ms
requirement, calibratable as a constant, and it **cancels outright for
box-to-box intervals**. Do not rely on `cmd/sync` to remove it as built; either
send `d = 0` (keeping the cadence, which does work) or leave it labelled `swc` so
the data records that a not-fully-effective correction was applied.

#### Building dserv on a fresh Pi (rpi500 notes)

The `dserv` target resolves tcl/jansson/uv/OpenSSL via `find_library` against
**system** packages — nothing pulls `deps/fltk` or `deps/libharu`, so the full
recursive submodule init is avoidable. Needed: `libjansson-dev libuv1-dev
libssl-dev sqlite3` (the `sqlite3` *executable* is a hard `find_program`
requirement).

**Tcl is the exception and the `deps/tcl` submodule build is the right answer.**
Debian's `tcl9.0-dev` ships `tcl.h` under `/usr/include/tcl9.0/` and the stub
library as `libtclstub9.0.a`, while the build expects `tcl.h` and `libtclstub.a`
at standard prefixes — so the distro package needs per-host `-I` flags and a
symlink, which is exactly the divergence the submodule exists to avoid.
`deps/tcl/unix: ./configure --prefix=/usr/local && make && sudo make install`
takes ~2 min on a Pi 5 and puts everything where CMake expects it. Verify with
`ldd dserv | grep tcl` that headers and library come from the same place.

Module/binary must be installed **together**: `ENABLE_EXPORTS ON`
(CMakeLists.txt:247) means modules resolve symbols from the dserv binary at load
time, and `gpio_input.c` now calls `tclserver_clock_epoch_offset_us`, which
exists only in the rebuilt binary.

#### PTP applicability, per board

| end | IEEE-1588 hardware timestamping |
| --- | --- |
| RT1062 / Teensy | **Yes** — `CONFIG_PTP_CLOCK_NXP_ENET` on, `box_ptp.c` reads the counter today (`state/ptp/ns`) |
| W6300 / RP2350 | **Almost certainly not** — a hardwired TCP/IP offload chip, not a 1588-capable MAC (verify against the datasheet) |
| Host | Varies. Pi 5 and Intel server NICs are the credible targets; **USB Ethernet adapters essentially never have it** |

With hardware timestamps at *both* ends PTP reaches sub-µs, which would **beat
the TTL line** — because our "hardware sync" is only hardware on the box side
(the host stamps `ess/in_obs` in software, which is why anchor quality tops out
at ~90–125 µs). With software timestamping at either end, expect tens of µs on a
quiet LAN — still far better than today's naive `sw` anchor, which assumes zero
delay. Note `box_ptp.c` currently reads only the **local free-running counter**;
disciplining it to a grandmaster needs a peer and is not implemented.

Caveat: PTP accuracy depends on **path symmetry**, so non-PTP-aware switches
inject asymmetric queuing. A direct cable is ideal; transparent/boundary clocks
fix it but are real infrastructure.

**Reframe worth keeping:** "dserv host with no GPIO" is not "no TTL anywhere in
the rig". The sync edge need not originate from the dserv host — a stimulus
computer's sync output, a shared trigger distribution, or a photodiode anchors
the box just as well, since all the box needs is an edge it can latch paired with
a datapoint carrying a host timestamp.

#### Transport selection is boot-time only (dual image)

`box_net_dual_impl.c`: the transport comes from the **GP28 strap read in main()**,
not from flash ("a stale `mode eth` in flash on a box with no cable used to hang
W6300 init forever; a strap can't go stale"), and there is deliberately no
boot-time PHY auto-detect. With the strap open the policy is auto: boot USB, then
sense `CW_GET_PHYLINK` debounced and swap up to Ethernet — **but outside the 10 s
boot window it refuses to switch while a USB host is mounted** ("an active USB
session is never yanked out from under dserv"), and it never auto-downgrades.

Practical consequence: plugging Ethernet into a box already serving a USB host
does nothing. `cmd/reboot` with the cable already in works — inside the boot
window the USB-host veto is bypassed, and it came up on Ethernet at ~17 s with
`sync_pin` and pin config intact (they had been `cmd/save`d).

### The USB harness disagreement is NOT generic — it was Teensy-specific

The two-harness agreement check on this box over USB: `loopback_rtt` 1043 vs
`lb_split` total 1119 — **76 µs apart, i.e. they agree**. The ~426 µs
disagreement seen on the Teensy's USB path does **not** reproduce here, on the
same ingest path (`extio` subprocess → `usbio.c` → `tclserver_set_point`). So
that anomaly is not a property of the USB ingest path in general, and the hunt
narrows to the Teensy/Zephyr side. Item 1 of "What not to depend on yet" is
correspondingly narrowed, not cleared.

### Wiring hazard found the hard way

GPIO26 was already held as an **output** (`consumer="dserv output"` — the
intended sync line), so teeing it to GPIO27 wired two driven outputs together and
clamped the node low. Symptom: `gpio/output/27` reads back correctly in dserv
while `pinctrl get` shows `27: op dh … | lo`, and no observer sees an edge.
**`pinctrl get <lines>` and `gpioinfo` are the diagnostic** — dserv cannot see
this, because from its side the write succeeded. The baseline belongs on an
unclaimed line (GPIO23).

### What this supersedes

- The whole-loop figures in "NEXT: Ethernet RX" (2365 → 783 µs etc.) — measured
  pre-timestamp-fix.
- The `720 / 294 / 430` L1 split and the DTCM case resting on it.
- Any USB number for either box, still unresolved (see the harness matrix above).
- The retracted W6300-vs-native claim stays retracted; this section does not
  reinstate it, because `upstairs` and the Teensy have not yet been measured
  under identical conditions with the electrical reference.

---

## 2026-07-25 — RW612 first boot: PTP instantiates, and a silent clock bug

First silicon session on the FRDM-RW612. The headline: **the 1588 clock works,
and `status = "okay"` alone is not enough to get it.** Two independent defects
sat behind it, the second far nastier than the first.

Everything below was established against **stock `zephyr/samples/net/ptp`**,
nothing of ours in the picture, before touching the app — the "run something
independent first" approach, and it was the right call: both defects are in
board/driver plumbing, and finding them inside our own build would have mixed
them with our bugs.

### Toolchain and probe, for the record

The MCU-Link on this board is **already reflashed with SEGGER J-Link firmware**
(VID `0x1366` / PID `0x1024`), so `west flash -r jlink` works with no LinkServer
and no NXP tooling. `/dev/cu.usbmodem0010698133301` is the J-Link VCOM carrying
the **board UART console** (flexcomm3) — that is where Zephyr's `printk`/shell
live. Our app's own CLI is on the **USB CDC console**, a second cable to the
RW612's own USB-C port. Both are needed for a full session.

BT links without ceremony — `west blobs fetch hal_nxp` has evidently already
been run on this machine, so the NBU blob is not a blocker for goal 3.

### Defect 1: the predicted pinctrl failure — real, but the recorded cause was wrong

`ptp_clock_nxp_enet_init()` calls `pinctrl_apply_state(PINCTRL_STATE_DEFAULT)`
unconditionally. With no `pinctrl-0` the state table is empty,
`pinctrl_lookup_state()` returns `-ENOENT`, and the device reports **`DISABLED`**
in the `device list` shell command — i.e. it presents as *absent hardware*
rather than as a config error. Confirmed on silicon.

**But the fix needed no Zephyr patch and no reference-manual dig.** The PTP
section above claimed "no `1588` pinmux entries exist anywhere in the RW612 dts
or board pinctrl". They exist: `IO_MUX_ENET_TIMER0..3` in
`modules/hal/nxp/dts/nxp/rw/RW612-pinctrl.h`, pads **IO27 / IO61 / IO24 / IO26**.
A three-line `&pinctrl` group takes the device to **`READY`**.

**Pad choice is a trap, and it cost real time.** Pads **24 and 26 are flexcomm3
USART — the console**. Muxing `ENET_TIMER2` onto pad 24 boots a completely
healthy system with **no console at all**: silent, and it reads exactly like a
boot hang. J-Link said otherwise — `PC` sat in `arch_cpu_idle`, `IPSR` = 0, i.e.
the idle thread, running fine.

The reason a grep misses it is worth keeping: **the board dts names those pads
only through the macro `IO_MUX_FC3_USART_DATA`**, never as literal numbers, so
`grep 24` over the dts finds nothing. The reliable check is the *expanded*
devicetree — decode `pinmux` cells out of `build/zephyr/zephyr.dts`, where
`pad = value & 0x7F`, and cross-reference against which groups are actually
referenced by an enabled node's `pinctrl-0`:

```
pinmux_flexcomm3_usart  APPLIED  pads [24, 26]      <- the console
pinmux_enet             APPLIED  pads [21,22,23,25,55,58,59,60,62,63]
pad 27 (TIMER0, Arduino D5)  FREE
pad 61 (TIMER1)              FREE
```

We use **TIMER1 / IO61** — free, off the Arduino header, costs no box pin.
**TIMER0 / IO27 is deliberately left free**: it is Arduino D5, i.e. the
header-*reachable* 1588 event pad, and therefore the candidate for a hardware
**capture** input. That would put a TTL edge directly onto the PTP timebase —
the TTL sync line and PTP unified in one mechanism, rather than two clocks to
reconcile. Worth remembering when the sync story reaches this board.

### Defect 2: the counter ran at 0.14996x real time, silently

With pinctrl fixed the device is `READY` and `ptp_clock get` advances. It was
also **wrong by a factor of 6.67**, rock-steady.

Root cause, and it is a chain of three:

1. The node's stock `clocks = <&clkctl1 MCUX_ENET_PLL>` names a clock ID that
   **`clock_control_mcux_syscon.c` has no `case` for**. There is a
   `MCUX_ENET_CLK` case (used by the MAC) and `MCUX_ENET_QOS_PTP_CLK`, but
   nothing for `MCUX_ENET_PLL`.
2. That `get_rate()` falls straight out of the `switch` and **`return 0`** —
   i.e. reports **success** — without ever writing `*rate`.
3. `ptp_clock_nxp_enet.c:165` casts the return to `(void)`, and
   `uint32_t enet_ref_pll_rate;` is an **uninitialized stack variable**. It goes
   directly into `ENET_Ptp1588StartTimer()`, which derives the tick increment
   from it.

Fix, in our overlay: `clocks = <&clkctl1 MCUX_ENET_CLK>`
(`CLOCK_GetTddrMciEnetClkFreq()` = **50 MHz**, the ENET module clock). Rate goes
to 1.00 immediately.

**The lesson is the shape of this failure, not the fix.** A clock that advances
smoothly, monotonically, and at a perfectly stable wrong rate passes every
"is the PTP clock alive?" check anyone would naturally write — including
`main.c`'s own `PTP hw clock: ready=%d now=%llu`. **Check the rate, not the
motion.** This is the same class as the `sw`-anchor bias above: not noise, not
intermittent, just quietly wrong.

Both fixes are in `boards/frdm_rw612.overlay` with the reasoning inline.

### What the rate is NOT yet known to be

Two harnesses were run and **they disagreed**, so per the standing rule neither
is trusted:

| harness | figure |
| --- | --- |
| shell `ptp_clock get`, 121 s | −14 ppm |
| `state/ptp/ns` off the frame pipe, endpoints | +1924 ppm |
| same, least-squares over 139 samples | +48 ppm |

The +1924 ppm was an artifact of a single late first sample — the DTR-open
**announce burst** floods the pipe, so the first arrival timestamp lands ~190 ms
late and compresses the host span. Endpoint arithmetic is the wrong estimator
against a bursty transport; least-squares over all samples is not.

But the real limit is instrument resolution: **max residual ±104 ms**, because
arrival times come from a host read loop with a 0.2 s timeout. Estimates swing
across ~130 ppm depending on which samples are included, which is the tell.

**Established: the rate is 1.000 within about ±100 ppm** — which settles the
`MCUX_ENET_CLK` fix beyond doubt, since the defect it replaced was 850,000 ppm.
**Not established: any ppm-level figure.** Do not quote the −14 ppm as a crystal
characterization. This is exactly what the PTP section above already warned
about 1 Hz `ptp/ns` datapoints — "the quantization dwarfs it" — and it applies
to the shell path equally. Real rate measurement needs the PTP stack against a
grandmaster, or on-box sampling. Which leads to:

### The host side is already capable — measured, not assumed

`ethtool -T eth0` on **rpi500** (Pi 5, RP1 MAC):

```
hardware-transmit  hardware-receive  hardware-raw-clock
Hardware timestamp provider qualifier: Precise (IEEE 1588 quality)
Hardware Transmit Timestamp Modes: off / on / onestep-sync
```

So the Pi 5 is a **full hardware-timestamping PTP endpoint** with a `/dev/ptp*`
and one-step sync. Combined with the RW612's now-working 1588 clock, **PTP can
be closed end-to-end on hardware already owned** — direct cable, to avoid the
switch asymmetry the PTP section flags. That is the credible route to beating
the TTL line's 35 µs / 19 µs, and it is the correct next measurement of the RW612
clock, replacing both harnesses above.

Consequence for the **i.MX93** evaluation: its main differentiator over the Pi 5
was going to be exactly this capability, and the Pi 5 already has it. It has no
3D GPU (unlike the i.MX95) so it cannot host stim2, and 2xA55 is slower than the
Pi 5's A76s, so it is a downgrade as an experiment host. **Parked**, absent a
specific need. Its one remaining merit is as a second, independent host for
cross-checks — non-trivial given how many conclusions in this document were
confounded by single-host measurement.

### Our own app on this board

Builds and runs. Boot banner over the USB CDC console:

```
gpio: pin 12=out (LED), pin 11=in_pullup
eth: no lease (link=0)
PTP hw clock: ready=1  now=8368999580 ns
console: cdc (config/console cdc|uart; save+reboot)
active uplink: usb
BLE central up; scanning for d5e7000x peripherals (max 8)
```

- **`CONFIG_MAIN_STACK_SIZE=4096` from the shared `prj.conf` overflows on this
  board** — usage fault in `main`'s prologue, since `main()` here initialises BT
  + net + USB. Set to **16384** in `boards/frdm_rw612.conf`. That is a working
  value, **not a measured minimum** — trim it with `CONFIG_THREAD_ANALYZER`
  rather than leaving a guess in the tree.
- **The frame pipe is clean on first contact.** 1613 frames decoded straight off
  the CDC data endpoint with **zero resync-discarded bytes**, 28 distinct
  `state/*` keys, announce working. Note this is the *raw pipe*, no dserv — a
  useful harness in its own right (`host/` candidate), because it removes dserv,
  Tcl dispatch and the `usbio` module from the path.
- **`lsof` the port, not `pgrep dserv`.** A stale reader produced a mid-run
  "multiple access on port" failure. `extio-setup` was running and its serial
  driver probes box ports, which fits the transient far better than dserv does.
  Same family as the 2026-07-23 lesson ("dserv's own extio subprocess was
  opening the console port").

### Not yet touched

Ethernet on this board is unproven — `link=0`, no cable during this session, and
the KSZ8081 autonegotiation timed out in the stock sample for the same reason.
Goal 1 (rock-solid HS USB) has had no latency measurement at all; the numbers
above are rate checks, not latency. **No RW612 latency figure exists yet**, and
when one is taken it must run the two-harness agreement check before it is
believed.

---

## 2026-07-25 (later) — Ethernet up, and the `save` hunt

### Ethernet works end-to-end

Against dserv on a Pi 5 (`raspberrypi`, eth0 192.168.11.10, direct cable, box .41):

```
transport = eth   board = frdm_rw612   ip = 192.168.11.41   extio/boxes = box
ESTAB 192.168.11.10:4620  <- 192.168.11.41:38118   (box -> dserv frames)
ESTAB 192.168.11.10:37502 -> 192.168.11.41:5010    (dserv connect-back)
```

Both halves of the handshake: the box self-registered (`%reg`/`%match`) and dserv
connected back to its config server. ICMP **0.283 ms min / 0.304 avg** (W6300
0.077, Teensy 0.197 — different hosts, indicative only).

**No latency number was obtained** — see the pin-map blocker below. Nothing here
is yet comparable to the wiznet reference set.

### `cmd/save` — TWO stacked bugs, and a bad diagnosis in between

Symptom: `save FAILED`, no detail. Resolved; both fixes are in tree.

**Bug 1 (Zephyr-side, real):** stock `samples/subsys/kvss/nvs` failed with
`nvs_mount -> -45`. **-45 is `EDEADLK`, not `ENOTSUP`** — `nvs_startup`'s "all
sectors are closed, this is not a nvs fs or irreparably corrupted" branch. The
64 MB FlexSPI part ships **non-blank** at `storage_partition` (0x620000), so NVS
sees no valid filesystem and refuses to clobber a region it does not recognise.
Fix: `CONFIG_NVS_INIT_BAD_MEMORY_REGION=y` in `boards/frdm_rw612.conf`.

**Bug 2 (ours, the actual cause of `save FAILED`):** `box_flash.c` used
`DT_REG_ADDR(storage_partition)`. This board declares its partitions
`compatible = "zephyr,mapped-partition"`, so that resolves through the FlexSPI
controller's `ranges` and yields the **memory-mapped XIP address `0x18620000`**,
while the flash API wants the **offset within the device, `0x620000`**.
`flash_get_page_info_by_offs()` therefore returned `-EINVAL`, `sector_size`
stayed 0, and mount failed. Fix: `PARTITION_OFFSET()` / `PARTITION_SIZE()` /
`PARTITION_DEVICE()` (the label-token macros the stock sample uses).

**Bug 3 (latent, would have bitten next):** `sector_count` was
`partition_size / sector_size` — "fill the partition". That partition is 58 MB =
**14816 sectors**, and with bad-region recovery enabled NVS erases an
unrecognised region *whole*: ~45 ms per 4 kB sector ≈ **11 minutes**, presenting
as a hung boot rather than a size error. Now capped at 8 sectors; the blob is
1080 bytes.

Verified on silicon: `page_info(off=0x620000) rc=0 size=4096`, `NVS 8x4096`,
`saved (1080 bytes)`, and **`pin18=out` survived a reboot**.

**The diagnostic lesson, which cost the most time here.** Every failure path
returned a bare `-1`. That single fact produced a wrong root cause held for
hours: because stock Zephyr also failed, "NVS is broken on this board" looked
confirmed, and the hunt went to the FlexSPI driver and the XIP-write hazard —
both irrelevant. The two failures were **different bugs with different errnos**
that a `-1` made indistinguishable. `box_flash_{init,save}` now return the real
errno, `box_flash_last_error()`/`box_flash_geometry()`/`box_flash_debug()` expose
the mount inputs, and the boot banner prints geometry and shouts if the mount
failed. **Print the errno.** Two independent bugs presenting identically is not
rare; it is what a discarded return value guarantees.

Corollary worth generalising: *"stock code reproduces it, so it is not our bug"*
is only valid if the stock failure is the SAME failure. Compare error codes, not
just outcomes.

### The pin map blocked all I/O measurement — SOLVED

`pinmux_hsgpio0` muxes **only pads [0, 1, 11, 12, 18, 21]** to GPIO. Of those:

| pad | what it is |
| --- | --- |
| 11 | **User SW2 button** (`gpio_keys`, active low + pull-up) — also Arduino D2 |
| 12 | user LED |
| 21 | **ENET PHY interrupt** (KSZ8081 `int-gpios`) — do not touch |
| 18 | Arduino D4 — genuinely free |
| 0 | Arduino D6, but **rejected by the config path** (`pins/in` unchanged, no `state/di/0`) — 0 is a sentinel somewhere |

Every other header pin is not muxed to GPIO at all, and configuring one
*silently succeeds* while doing nothing electrically — the same silent-failure
shape as the PTP clock-rate bug. **Fixed** by adding pads **15 (D3), 20 (D7),
27 (D5)** to `hsgpio0`'s pinctrl in `boards/frdm_rw612.overlay`; usable box pins
are now **0, 15, 18, 20, 27** (plus 11 = SW2 input, 12 = LED output).

**Retracted: "pin 0 is rejected by the config path / 0 is a sentinel."** Wrong.
`dserv_cfg__config` accepts `n >= 0 && n < BOX_NPINS`. That test was run against
a box that was already wedged — the same frozen-`watchdog` corpse described
below — so the null result meant nothing. Pad 0 (Arduino D6) is muxed by the
board and works.

**Hazard:** pin 11 was briefly set to `out` while probing this. That is the SW2
pin — driving it high while the switch is pressed shorts it to ground. Reverted
before any press. Treat pads 11/12/21 as reserved on this board.

### DI works — there was never any DI silence

Fully explained by the pin map. With pad 15 muxed and D4 -> D3 jumpered,
`state/di/15` tracks `state/do/18` exactly on every toggle. No RT10xx-style
hunt was needed; "DI has never been observed on this board" was an artifact of
driving and sensing pads that were not connected to the GPIO peripheral.

**First loopback, and the two-harness agreement check PASSES:**

| harness | min | med | p90 | p99 | max |
| --- | --- | --- | --- | --- | --- |
| `loopback_rtt` | 835 | **1056** | 1192 | 1252 | 3113 |
| `lb_split` total | 819 | **1078** | 1177 | 4056 | 4056 |
| — `L1` cmd -> do_echo | 727 | 869 | 940 | 3793 | 3793 |
| — `L2` do_echo -> di | 23 | 210 | 259 | 318 | 318 |

Medians 22 us apart, i.e. they agree (the recorded failure case was 426 us), so
the apparatus is sound.

**These are NOT comparable to the wiznet reference set yet**, and must not be
quoted as such. Three differences remain: the host was on the **`ondemand`**
governor (the reference set is `performance` — also the likely source of the
single 3793 us outlier, which is what frequency ramping looks like); it is a
different Pi from rpi500; and **no electrical reference is wired**, so both ends
are still software-timestamped. Re-take under `performance` and with
`host_gpio_rtt.sh` / `box_out_rtt.sh` before drawing any conclusion.

One thing worth watching once conditions match: `L2` at 210 us against the
W6300's 100 us is the leg carrying the DI event alone, and it is the least
governor-sensitive of the two.

### RETRACTED: "Ethernet needs the cable present at BOOT"

That was recorded here, and it was wrong. The real bug was **`box_net_eth_init()`
never brought the interface admin-up.** Zephyr's net_config subsystem
(`CONFIG_NET_CONFIG_SETTINGS` / `AUTO_INIT`) normally does that; we do not enable
it, because we own our own addressing. With the interface down the ENET driver
never starts the PHY, autonegotiation never runs, and **both** ends report no
carrier. Fixed by an explicit `net_if_up()` (idempotent) in `box_net_eth_init`.

**How it presented, and why it cost an evening.** Identical to a dead cable: box
`net/link=0`, host `NO-CARRIER`, no link LEDs. So the hunt went to cables, hub
power, ports, and NetworkManager — none of which could ever have been the cause.
Two things kept it alive: it worked *intermittently* early on (whatever brought
the iface up in those boots was incidental), and every host-side observation was
consistent with a physical fault.

**The test that actually settled it, in one step: run KNOWN-GOOD STOCK FIRMWARE
on the same hardware.** `samples/net/ptp` linked immediately on the same board,
cable, port and host — which localises the fault to our firmware and exonerates
the entire physical layer at once. Reach for that *before* swapping cables. It is
the same lever that isolated both PTP bugs and the NVS bug.

Second lesson: **`CONFIG_LOG` was off.** The KSZ8081 driver announces "PHY (2) is
entering autonegotiation sequence" and "PHY 2 is up ... 100 Mb, full duplex" —
it was reporting the truth all along and nobody was listening. Logging is now
enabled on the board UART for this board (`boards/frdm_rw612.conf`), separate
from the box console on USB CDC. On a networked board this is not optional: an
interface that never came up is otherwise indistinguishable from a bad cable.

### Host-side traps

- **NetworkManager drops the interface address on carrier loss.** When the link
  flapped, `eth0` went DOWN and **lost 192.168.11.10 entirely**, so the Pi had no
  address even once carrier returned. Fixed by taking it out of NM:
  `nmcli dev set eth0 managed no` + `ip addr add 192.168.11.10/24 dev eth0`.
  Does not survive a Pi reboot — make it a systemd unit if this rig persists.
- **A frozen counter reads exactly like a healthy one if sampled once.** A wedged
  box was declared alive off a single `watchdog` = 973 that happened to exceed an
  earlier reading. It was the *last value it ever sent*. dserv never deletes
  datapoints, so `transport=eth`, `link=1` and an IP were all being served from a
  corpse. **Always sample a counter twice.** Same sticky-datapoint family as the
  `sync/source` trap above.
- **Bench hygiene.** A USB hub carrying the board, the J-Link probe *and* the
  Ethernet adapter dropped three times, each time taking the console, the debugger
  and the network at once — once mid-flash. Several "board failures" chased that
  day were this. **Never let one hub carry both the thing under test and the means
  of observing it.**

### Bring-up scaffold now retirable

`BOX_BRINGUP_NET_IP` (CMakeLists + main.c) existed only because `save` was
broken, making `net/ip` unconfigurable (it applies at boot; changing it needs a
reboot; a reboot without persistence forgets it). **Persistence works now**, so
this can go — set the address at runtime and `save`. It is inert while a saved
config exists, but it hardcodes an IP in firmware and should not become load-
bearing.

### State at end of session

Working: Ethernet transport + registration (link brought up by the box itself),
PTP clock at correct rate, USB-HS frame pipe (zero resync discards over thousands
of frames), BLE central scanning, GPIO output, **persistent config surviving
reflash** (`pins/out=5,12,18` restored from NVS). ICMP 0.235 ms min.
DI proven, loopback closed, both harnesses agreeing. Unproven: any latency
figure comparable to the wiznet set (host not in benchmark configuration, no
electrical reference).

---

## 2026-07-25 (later still) — first RW612 loopback, and the noise floor

### The loopback closes

D4 (pin 18, out) -> D3 (pin 15, in), host `raspberrypi` on `performance` @ 2.4
GHz, load ~0.0, dserv over Ethernet:

| harness | min | med | p90 | p99 | max |
| --- | --- | --- | --- | --- | --- |
| `loopback_rtt` | 945 | **1051** | 1120 | 1157 | 1164 |
| `lb_split` total | 808 | **1029** | 1102 | 1190 | 1190 |
| — `L1` cmd -> do_echo | 719 | 813 | 878 | 975 | 975 |
| — `L2` do_echo -> di | 32 | 209 | 248 | 276 | 276 |

**Two-harness agreement: 22 us.** The apparatus is sound.

Against the wiznet reference set (rpi500/W6300 — a DIFFERENT Pi, so read the
ratio, not the absolute): loopback 676, `L1` 554, `L2` 100, ICMP floor 0.077 ms
vs the RW612's 0.210 ms. **Roughly 1.5x slower on every leg.**

**This is NOT the number the 1 ms requirement is about.** That budget is on
one-way input latency ("subject acts -> dserv knows"), measured for the W6300 as
322 us median with `host_gpio_rtt.sh` against a real electrical edge. The
loopback is a round trip including the outbound command. No electrical reference
is wired on this rig yet, so the comparable figure does not exist.

### The budget closes — and it is NOT mostly the wire

| term | us | instrument |
| --- | --- | --- |
| host dserv dispatch | ~116 | `dispatch_test.sh` |
| wire + box IP stack, RTT | 210 | ICMP floor |
| box service-loop wake | 53 | `dbg/wake_us` |
| box socket recv | 99 | `dbg/recv_us` |
| box dispatch + GPIO + publish | 300 | `dbg/disp_us` (includes the ~142 us send) |
| **sum** | **~778** | vs **`L1` = 813 measured** |

~450 us of the 813 is INSIDE THE BOX; only ~210 us is wire-plus-IP-stack round
trip. "Zephyr's stack is slow" is true but is not the dominant term.

### Where the stack time actually goes (`CONFIG_NET_PKT_*TIME_STATS_DETAIL`)

```
Avg TX net_pkt (3399) time  69 us   [0->28->41=69 us]
Avg RX net_pkt  (449) time 212 us   [0->28->13->96->73=210 us]
```

Tick points: `net_core.c:520` (driver -> `net_recv_data`), `net_tc.c:91` (TC
thread pickup), `sockets_inet.c:281` (socket layer), `:931` (app read returns).

| RX stage | us |
| --- | --- |
| driver -> `net_recv_data` | 28 |
| -> traffic-class thread | 13 |
| **-> socket layer (IP + TCP)** | **96** |
| **-> app read returns** | **73** |
| TX, whole path | 69 |

**RX is 3x TX.** The target is the 96 us of TCP/IP processing, then the 73 us
socket->app handoff. **Do not optimise TX** — the entire TX path is 69 us.

### RULED OUT: `CONFIG_ETH_NXP_ENET_TX_BUFFERS`

The driver defaults to 1 TX descriptor and `dbg/send_max_us` was 412, so a
descriptor stall on the RT path looked plausible. Raised to 6:

| | TX_BUFFERS=1 | TX_BUFFERS=6 |
| --- | --- | --- |
| loopback med | 1051 | 1068 |
| `L1` / `L2` med | 813 / 209 | 820 / 218 |
| `dbg/send_us` med | 142 | 161 |
| `dbg/send_max_us` | 412 | **283** |

**No resolvable effect on median latency.** Kept for the send worst case only.
The packet stats explain why in hindsight: TX is 69 us end to end, so there was
never 100+ us there to win. **The experiment was picked on a plausible mechanism
instead of on evidence about where the time goes — instrument first.**

### THE NOISE FLOOR, which invalidates single-run A/B

Same build, four consecutive `loopback_rtt` runs:

```
med 987 / 999 / 982 / 948      min 910 / 904 / 897 / 892
```

**Median spread ~50 us within one build**, plus a **warm-up trend of ~100 us**:
the first run after a reflash is consistently slowest and settles over
subsequent runs (1051 -> 1068 -> 1005 -> 987 -> 999 -> 982 -> 948).

Consequence: **no single-run A/B can resolve anything below ~100 us**, and every
optimisation worth chasing here is in that range. Both of this session's A/B
results dissolved under this: the TX-buffer "regression" was noise, and the
stats build's apparent 60 us *improvement* was warm-up — instrumentation cannot
speed up the data path.

**Protocol for any future tuning on this platform:** several runs per build,
DISCARD THE FIRST after each flash, and interleave A/B/A rather than comparing
one run to one run. Otherwise the same change can be "confirmed" as a win or a
regression depending only on which run is read.

Silver lining, and the question that was actually being asked:
`CONFIG_NET_PKT_RXTIME/TXTIME_STATS[_DETAIL]` + `NET_STATISTICS` + the Zephyr
shell cost nothing measurable here either — the Teensy finding replicates, so
leave the instrumentation on.

### A Zephyr shell now lives on the board UART

`CONFIG_SHELL` + `NET_SHELL` + `DEVICE_SHELL` + `PTP_CLOCK_SHELL` on flexcomm3
(the J-Link VCOM), entirely separate from the box CLI on USB CDC, so the two
never contend. That is where `net stats` above came from, and `device list` /
`ptp_clock get` are the commands that isolated the PTP bugs.

### OPEN: `dbg/loop_max_us` = 67,791

A **68 ms** worst-case service-loop pass. If it can occur mid-experiment it
breaches the 1 ms requirement by ~68x, and no amount of shaving 96 us off the RX
path touches it. Tail behaviour is not bounded by the same budget as the median.
Unknown whether it is boot-only (announce burst, NVS write) or recurrent — that
is the next thing to establish, ahead of any further median tuning.

### RESOLVED: the 68 ms `loop_max_us` — NVS garbage collection

Root-caused by staged sampling (`loop_max_us` is a running max since boot, never
reset, and excludes the blocking wait):

| stage | `loop_max_us` |
| --- | --- |
| boot + registration + announce, no save | **5,957** |
| after one `cmd/save` (blob unchanged) | 10,583 |
| forcing 8 distinct saves | 10,583 / **67,970** / … / **72,150** |

Saves 1, 3, 4, 6, 7, 8 were cheap; saves 2 and 5 cost ~68 and ~72 ms. That is
the NVS **garbage-collection** signature: most writes append cheaply, and every
~3rd one rotates a sector and pays a ~4 kB erase (8 sectors x 4096 with a
1080-byte blob = ~3 writes per sector). Not boot, not the announce burst.

**Already correctly gated, so this is a documented cost rather than a bug.**
BOTH save paths check `box_obs_active()` and `box_obs_defer(BOX_DEFER_SAVE)` —
the datapoint path (`main.c` `CFG_SAVE`) and the console path (`box_console.c`
`CLI_SAVE`). Flash programming therefore cannot land inside a trial; it runs at
the obs boundary, which this document already establishes as the one place a
discontinuity is provably harmless.

**The precise residual, because it is not what you would guess.** DI is not a
queue that can overflow during a stall. It is a per-pin COALESCING debouncer:
the ISR sets an unsettled flag and records `di_first_edge_us`/`di_last_edge_us`,
and `box_gpio_poll_di` reads the pin's CURRENT level and publishes only when it
differs from the last published level. So a long stall does not delay a backlog
and does not drop a queue — it **coalesces**: a pin that toggles an even number
of times and returns to its starting level publishes **nothing at all**. Events
that do publish are stamped at the onset edge, so timing stays correct. Right
semantic for debounced switch inputs, and it happens outside obs regardless.

If flash writes ever need to be safe at arbitrary times, the fix is to move
`box_flash_save` off the service loop (workqueue or low-priority thread) rather
than to deepen any buffer — an erase should never sit on the RT path.

---

## 2026-07-26 — USB-HS on the RW612, and a self-inflicted 12 ms

### THE BUG: `CONFIG_LOG` at default level costs ~12 ms PER USB FRAME

Enabling `CONFIG_LOG=y` + `CONFIG_LOG_MODE_MINIMAL=y` (added hours earlier to
surface the KSZ8081 PHY messages) silently destroyed USB performance.

`LOG_MODE_MINIMAL` is **synchronous** — it formats and writes inline, to a
115200 baud UART. Zephyr's default log level is INFO, and
`subsys/usb/device_next/class/usbd_cdc_acm.c` contains:

```c
static void cdc_acm_irq_tx_enable(const struct device *dev)
{
	atomic_set_bit(&data->state, CDC_ACM_IRQ_TX_ENABLED);
	if (ring_buf_space_get(data->tx_fifo.rb)) {
		LOG_INF("tx_en: trigger irq_cb_work");     /* EVERY FRAME */
		cdc_acm_work_submit(&data->irq_cb_work);
	}
}
```

So every frame the box sent paid a blocking UART write. ~12 ms is very close to
the time to shift that string out at 115200.

| | with default log level | `CONFIG_LOG_DEFAULT_LEVEL=1` |
| --- | --- | --- |
| `dbg/usb_send_us` | 12,044 | **53** |
| `dbg/usb_send_max_us` | 3,084,644 (3.1 s) | **258** |
| `dbg/disp_us` | 12,254 | **778** |
| `dbg/loop_max_us` | 11,243,054 (11.2 s) | **2,453** |
| raw inter-frame gap, med | 12,152 | **256** |
| raw gaps < 1 ms | **0 %** | **94 %** |
| loopback RTT | ~86,000 | ~870 |

**Fix:** `CONFIG_LOG_DEFAULT_LEVEL=1` (errors), raising individual modules
explicitly (`CONFIG_PHY_LOG_LEVEL_INF=y`). Never leave the default level on a
board whose logging backend is synchronous.

**Why this one is worth remembering.** It was invisible from outside: the
watchdog still ticked at exactly 1 Hz, the box stayed registered, `show` was
normal, and Ethernet was unaffected (the `LOG_INF` is on the CDC TX path only).
Only `dbg/usb_send_us` — the box's own instrumentation — showed it. A diagnostic
change made for one subsystem silently crippled another, and **the same commit
that added the logging also took the Ethernet measurements**, so those numbers
were taken under an unrelated 12 ms hazard that happened not to touch them.
Cross-check a config change against the transports it is *not* aimed at.

### Localisation, for method

The hunt went: harness returns no samples -> manual toggle shows ~86 ms RTT ->
split into legs (`L1` 74 ms, `L2` **12.19 ms, sd ~30 us**) -> a rock-steady 12 ms
on a leg carrying frames the box emits ~62 us apart says *fixed per-frame cost*,
not poll quantization -> stop dserv, read the tty raw (`host/rawpipe.py`, pure
stdlib, no pyserial) -> the announce burst STILL arrives 12 ms apart with no
host software in the path -> therefore box-side -> `box_net_usb_client_send` is
non-blocking by construction (ring_buf_put + `uart_irq_tx_enable`), so a 12 ms
measurement across those two calls indicts `uart_irq_tx_enable` -> read it.

`host/rawpipe.py` is worth keeping: it removes dserv, Tcl and `usbio.c` from the
USB path exactly as `eth_listen.py` does for Ethernet.

### USB-HS vs Ethernet — same box, same host, same session

480 Mbps negotiated (`/sys/bus/usb/devices/*/speed`), Pi 5 on `performance`,
load ~0.1, D4->D3 loopback:

| leg | **USB-HS** | Ethernet |
| --- | --- | --- |
| loopback median | **866–890** | 948–999 |
| loopback min | **756–758** | 892–910 |
| loopback p99 | **1002–1022** | 1073–1126 |
| `L1` cmd -> do_echo | **650–673** | 813 |
| `L2` do_echo -> di | 198 | 209 |

**USB-HS wins on every metric** — ~90 us faster median, ~140 us lower floor,
tighter tail. This CONFIRMS the projection recorded on 2026-07-26 above, which
deliberately refused to claim it ("Measure it; do not cite this paragraph as a
finding"). It is now a finding. The mechanism is the one predicted: USB's cost
is off the RT loop by construction (ring + TX interrupt), and High Speed cuts the
frame quantization 8x versus USB-FS.

### CLOSED: the Teensy USB harness disagreement is Teensy-specific

`loopback_rtt` 866–890 vs `lb_split` total 857–884 — **within ~30 us, i.e. they
agree**. The unexplained ~426 us disagreement on the Teensy's USB path does NOT
reproduce on this box, over the same ingest path (`extio` subprocess ->
`usbio.c`). Combined with the pico agreeing, the anomaly is neither
"USB ingest in general" nor "Zephyr in general" — it is Teensy-specific.

### Operational notes

- `pgrep -f <pat>` / `pkill -f <pat>` **match their own command line**. This bit
  twice: `pkill -f cmd_rtt` killed the ssh session running it, and
  `pgrep -c -f "local/dserv/dserv"` reported dserv alive when it was stopped
  (counting itself). Use `systemctl is-active`, or a pattern that cannot self-match.
- `dbg/usb_drops` counts whole dropped frames and is the real "too many
  datapoints" signal on USB. It reached 210 during the 12 ms period and stopped
  rising afterwards — a useful confirmation that the fix took.
- The Pi has no `lsof` and `sudo` is not passwordless.

### THE HARNESS BUG: `dpointSetScript <dp> {}` does not remove a script

Every harness here tore down with `dpointSetScript $DEV/state/... {}`. That does
**not** remove the script. `TclServer.cpp` `dpoint_set_script_command` does:

```c
tclserver->dpoint_scripts.insert(varname, script);   /* stores "" */
```

and the delivery path then does:

```c
if (tserv->dpoint_scripts.find(varname, script)) {
    dpoint_tcl_script(interp, script.c_str(), dpoint);   /* fires even when "" */
}
```

So an empty script is **stored and evaluated on every publish of that datapoint,
permanently, until dserv restarts.** There is a proper `dpointRemoveScript`;
nothing used it. Fixed across 10 harnesses (14 sites) 2026-07-26.

**Effect, measured.** With a stale empty script on `state/do/18` left by earlier
`lb_split` runs, `loopback_rtt` — which never touches that datapoint — paid an
extra Tcl dispatch on every `do` echo:

| | `loopback_rtt` | `lb_split` total |
| --- | --- | --- |
| stale empty scripts present | 850–875 | 715–733 |
| after `dpointRemoveScript` | **719–732** | **718–738** |

**This is what the "two-harness disagreement" was.** The harnesses agreed to
10–15 us once cleaned. `lb_split` overwrote the empty script with a real one and
so never paid the tax, while `loopback_rtt` did — a systematic ~130 us offset
that looked like an observer effect and reversed with configuration. **It is
also the leading candidate for the ~426 us Teensy USB disagreement** recorded
above and open for days.

**Consequence for every number in this document:** the harnesses accumulate this
tax across runs on a long-lived dserv, so figures measured late in a session are
inflated relative to early ones by an amount nobody was tracking. **The wiznet
reference set was taken with these same harnesses** — so 676 / 322 us and the
1 ms verdict resting on them are suspect and should be re-taken.

Suggested dserv changes (not yet made — they touch the deployed server):
1. Treat an empty script at registration as a removal, or skip the eval at
   delivery. Today any caller can install a permanent invisible tax by accident.
2. A second `dpointSetScript` on a live datapoint **silently replaces** the
   first — no error. Two subsystems registering on one datapoint means one
   loses silently. (`dservWhen` already keeps a separate registry specifically
   "so it never dislodges a caller's dpointSetScript"; `dpointSetScript` has no
   such protection against itself.)
3. Note the delivery asymmetry: a datapoint WITH an entry costs a hash lookup,
   one WITHOUT falls through to `find_match()`, a wildcard scan. "No script" can
   therefore cost more than "script".

### RW612 Ethernet, battened down and cleanly measured

`mode eth` (strips the USB data pipe entirely, so dserv cannot double-forward),
stale scripts cleared, Pi 5 `performance`, load ~0:

| | RW612 | pico2 + W6300 (reference, SUSPECT — see above) |
| --- | --- | --- |
| loopback median | **719–738** | 676 |
| loopback min | **640–681** | 632 |
| loopback p99 | 872–877 | **717** |

Median within ~7 %, **floor essentially identical**, tail ~20 % worse. This
supersedes the "~1.4-1.5x slower" reading recorded earlier today: almost all of
that apparent gap was measurement contamination — ~370 us of USB
double-forwarding plus ~130 us of stale empty scripts.

### Double-forwarding: a box on BOTH transports pays ~370 us

With the box reachable over USB *and* Ethernet, it stays in `::extio_known` from
the USB session (so `extio/box/cmd/*` is wired to `usbio_forward`) *and* self-
registers over Ethernet. Every command is pushed twice — once into a USB pipe
the box is not draining, because `uplink=eth` means the service loop polls
Ethernet only. Measured cost: loopback 1227-1234 -> 852-870 on
`extio_unforward_box`.

**Operational rule: use `mode eth` on an Ethernet box.** It strips the data pipe
at enumeration (`u_usb_init` registers the console CDC only), so `if02` never
appears and the host cannot forward over USB. Verified: `by-id` shows `if00`
alone. The console CDC remains for CLI.

A host-side fix (unforward USB when `state/transport` reports `eth`) would be
more robust but touches the deployed module shared with production Pico boxes.

---

## 2026-07-26 — pico vs RW612, same host, same session, FIXED harnesses

The comparison this document has needed since the port began. Host
`raspberrypi` (Pi 5, `performance`, load ~0), one dserv, one direct cable
(sequential — the Pi has a single Ethernet port), both boxes measured within
minutes of each other, harnesses with the `dpointRemoveScript` /
`dservRemoveMatch` fixes, leaked state cleared before each.

| | pico2 + W6300 | RW612 (Ethernet) |
| --- | --- | --- |
| loopback median | **616–625** | 715–739 |
| loopback min | **584–592** | 640–681 |
| loopback p99 | **647–668** | 872–877 |
| `L1` cmd -> do_echo | **523** | 667 |
| `L2` do_echo -> di | 85 | **54** |
| ICMP floor | **0.101 ms** | 0.170–0.198 ms |

Both harnesses agree to ~10 us on both boxes.

### The reference set WAS inflated — confirmed, ~50-60 us

`office` measures **616–625** here against the **676** recorded in the wiznet
reference set, with `L1`/`L2` at 523/85 against the recorded 554/100. Same box,
same host class, same harnesses — except the harnesses no longer leak an empty
script and a match per run. **That is the predicted harness-leak tax, measured.**

**Every figure in the 2026-07-26 reference section should be read ~50-60 us
high**, including the **322 us input latency** the "2.8x margin against the 1 ms
requirement" verdict rests on. The verdict survives (it gets better, not worse),
but the numbers want re-taking.

### The gap is entirely in the COMMAND path — and reverses on the EVENT path

- `L1` is **144 us slower** on the RW612. Of that, ~80 us is raw transport (the
  ICMP floor difference: W6300 hardwired TCP/IP offload vs Zephyr's software
  stack on the CPU), leaving ~64 us of box-side processing.
- `L2` is **31 us FASTER on the RW612** (54 vs 85).

**`L2` is the leg the 1 ms requirement is about.** "Subject acts -> dserv knows"
is the input path, and the RW612 is ~35 % better on it. The round-trip figure
that makes the RW612 look worse is dominated by the outbound command leg, which
matters far less for these paradigms — a scheduled pulse or a stimulus onset is
not on the critical path the way a subject response is.

So: **lagging ~16-19 % on the round trip, ahead on input.** Not the "1.4-1.5x
slower" recorded earlier today, which was almost entirely measurement
contamination (USB double-forwarding + leaked scripts).

### The real deficit is the TAIL

p99 **872–877 vs 647–668** — ~32 % worse, and it is the one metric where the
RW612 loses on every reading. That is jitter, not throughput, and it is the
right target for further work. Median latency on this board is already inside
budget; tail behaviour is not bounded by the same budget (see the NVS-GC and
12 ms-logging entries above — both were tail events invisible in the median).

### Caveats

- `L2` is a PROXY for one-way input latency, not the measurement. The real
  figure needs the Pi-GPIO electrical reference (`box_out_rtt.sh` wiring — NOT
  `host_gpio_rtt.sh`'s header diagram, which collides on GPIO22). Both boxes'
  numbers here are software-stamped at both ends.
- Sequential, not simultaneous. The boxes cannot share the single Ethernet port
  on a direct link; a small switch would allow both registered at once, at the
  cost of putting a switch in the path.
- The RW612 ran `mode eth` (data pipe stripped) — the correct configuration for
  an Ethernet box, and required to avoid the ~370 us double-forwarding tax.

---

## 2026-07-26 — PTP WORKS: RW612 locked to the Pi at ~±100 ns

Step 1 of PTP integration, done. RW612 (`CONFIG_PTP=y`) as TIME_RECEIVER against
`ptp4l` on the Pi 5, hardware timestamping at BOTH ends, direct cable (no switch,
so no path asymmetry).

```
Port 1 State   : TIME_RECEIVER
grandmaster    : 2C:CF:67:FF:FE:B3:39:F4 (the Pi)   steps_rm 1
offset_from_tt : -16 / -11 / -29 / -109 / -78 / -37 ns
mean_delay     : 1229-1287 ns          sync status : stable
PHC now        : 1785039984.134287686  (stepped to the Pi's wall clock)
```

**~±100 ns.** For scale, against the same box's other anchoring methods measured
with the electrical reference: `hw` TTL edge **35 us**, `swc` −167 us, `sw`
−181 us. PTP is roughly **350x tighter than the TTL line**, which is exactly the
outcome the PTP section above predicted for hardware timestamps at both ends.

### THE GOTCHA: `hwts_filter full` is REQUIRED on the Pi 5

Without it PTP appears to work and silently does not. The BMCA completes, roles
are correct, `sync status` reads `stable` — and `offset_from_tt` and
`mean_delay` sit at **exactly 0 ns**, because no event message is ever
timestamped:

```
ptp4l: port 1 (eth0): received SYNC without timestamp
ptp4l: port 1 (eth0): received DELAY_REQ without timestamp
```

Cause: the Pi 5's NIC is `macb` (RP1 Cadence GEM) and advertises only `none` and
`all` RX filter modes — **no per-protocol PTP filter**. ptp4l's default request
for `HWTSTAMP_FILTER_PTP_V2_EVENT` is not honoured. `--hwts_filter full` asks
for `HWTSTAMP_FILTER_ALL` instead, which the driver does support. The messages
disappear and the offset starts computing.

Note the failure shape, which is this project's recurring one: **a zero that
looks like perfection.** `offset_from_tt: 0 ns` reads as a perfectly locked
clock. Check `mean_delay` too — 0 there means the delay exchange never
completed.

Reproducible setup: `host/start_ptp.sh`.

### What this does NOT yet give us

**Box events are not yet on dserv's timeline.** PTP disciplines the box's 1588
counter to the Pi's **PHC**. dserv stamps datapoints from `steady_clock` + a
fixed epoch offset (commit `227315e`) — `CLOCK_MONOTONIC`, a **different
oscillator** from the NIC's PHC, and one that is NTP-slewed while the PHC is not.
So the two drift apart and a mapping is still required.

The mapping is, however, **local** — both clocks are on the host, no wire
involved — via `PTP_SYS_OFFSET_PRECISE`, which reads the PHC and the system
clock together with hardware assistance. That converts the hard problem (an
unmeasurable one-way network delay) into an easy one (two clocks on one board).
Structurally better than the obs-anchor mechanism it would replace.

### Next steps, in order

1. **PHC <-> dserv-clock bridge** (host side). The actual engineering.
2. **Certify against the electrical reference.** `host/clock_err.sh` already
   differences one physical edge stamped independently at both ends; PTP becomes
   a fourth `sync/source` value, measured rather than trusted. **Do not quote the
   ~100 ns as end-to-end accuracy until this runs** — it is PTP's own estimate of
   its own offset, which is not the same as a stamped event landing correctly on
   dserv's timeline.
3. **Then the interesting one:** `ENET_TIMER0` on Arduino D5 (IO27), left free on
   purpose, is a 1588 **capture** pin — a TTL edge hardware-timestamped directly
   into the PTP timebase, no software path at all. That unifies the sync line and
   PTP into one mechanism.

### The PHC -> dserv-clock bridge (step 2) — measured, and the drift removed

The chain from a box event to dserv's timeline, each link measured:

| link | mechanism | accuracy |
| --- | --- | --- |
| box 1588 clock <-> Pi PHC | `ptp4l`, hw timestamps both ends | **~±100 ns** |
| PHC rate <-> system clock | `phc2sys` | **0.002 ppm** (was −46.4) |
| PHC <-> `CLOCK_MONOTONIC` | `host/phc_offset.c` | **±703 ns** |
| `CLOCK_MONOTONIC` -> dserv | `dservClockEpochOffset` (new) | exact |

```
dserv_us = phc_us - (phc_minus_mono_us) + dservClockEpochOffset
```

**Total ~±0.8 us**, dominated entirely by the third link. Against the TTL
hardware anchor at 35 us that is ~44x better, and it is ~1250x inside the 1 ms
requirement.

**`phc2sys` is not optional.** The Pi is the PTP grandmaster, so its PHC is
free-running: measured at **−46.4 ppm** against `CLOCK_MONOTONIC`, i.e. ~46 us of
divergence per second, which would need re-measuring every ~21 ms to hold 1 us.
`phc2sys -c eth0 -s CLOCK_REALTIME` disciplines the PHC to the system clock, and
because Linux applies the same frequency adjustment to `CLOCK_MONOTONIC` as to
`CLOCK_REALTIME`, that rate-locks the PHC to *dserv's* timebase. Drift collapses
to 0.002 ppm — re-measure every ~400 s instead. Confirmation that the mechanism
is the one intended: phc2sys reports `freq -46636` ppb, matching the −46.4 ppm
measured independently by `phc_offset`. Script: `host/start_phc2sys.sh`.

**`PTP_SYS_OFFSET_PRECISE` is NOT available here** — `macb` implements no
`getcrosststamp`, so there is no hardware cross-timestamp and the two-method
cross-check degrades to one method. `phc_offset` reports its own error bound
(half the MONO-PHC-MONO read window, min-filtered: 1407 ns window -> ±703 ns) so
the uncertainty is stated rather than assumed. On a NIC that supports it, method
B would tighten this link by ~10x and make the whole chain PTP-limited.

**Why this link is tractable at all:** both clocks are on the host. There is no
wire, so no one-way delay to estimate and no asymmetry to model — precisely the
problem that made the obs-anchor mechanism hard (see "Why the ~200 us has been so
hard to pin down and subtract"). The hard half of clock sync is now handled by
PTP in hardware, and what remains is two local clock reads.

### What is still missing (step 3)

Nothing yet APPLIES this to event timestamps. Two options:

1. **Box-side (recommended):** feed `box_clock` a single offset
   `D = dserv_us - ptp_us` computed on the host, in place of the obs-boundary
   anchor. The existing `cmd/sync` plumbing already does exactly this shape, but
   with a network round trip whose asymmetry is unmeasurable; here D comes from
   two local reads and carries **no transport term at all**. Refresh every few
   minutes, per the drift above.
2. **Host-side:** convert on ingest in dserv. Avoids firmware change but puts a
   per-datapoint conversion in the hot path and loses the box's own view.

Then **certify with `host/clock_err.sh`**, which differences one physical edge
stamped independently at both ends. PTP becomes a fourth `sync/source` value
measured against a real edge rather than trusted. **Until that runs, the ±100 ns
is PTP's estimate of its own offset — not evidence that a stamped event lands
correctly on dserv's timeline.**

### HARDWARE: the MDIO bus on this board is intermittent — and it explains a lot

`E: phy_mc_ksz8081: PHY is still in factory mode!` and no link, on **stock
Zephyr** (`samples/net/ptp`) as well as ours, across power cycles, with
`CONFIG_PTP=n` as well as `=y`. Not our firmware.

The message is **misleading**. `phy_mc_ksz8081_phy_readiness_check` reads the
OMSO register and tests one bit; an MDIO read that returns all-ones sets that bit
and aborts init. So it reports "factory mode" for any failed read.

Proved with `CONFIG_MDIO_SHELL`, reading the same registers repeatedly:

| reg | successive reads |
| --- | --- |
| 2 (PHYID1) | `0x0`, `0xffff`, `0xffff`, `0x5`  (should be `0x0022`) |
| 1 (BMSR) | **`0x7849`** — valid, autoneg-capable, link down — then `0xffff` |
| 0 (BMCR) | `0x1000`, `0x1000` — stable, autoneg enabled |

`mdio scan` finds devices at 0x0 and 0x2, and PHYID2 reads `0x1561` (a genuine
KSZ8081). **The PHY is alive and answers correctly some of the time.** Reads fail
intermittently; when the factory-mode check lands on a bad read, Ethernet never
comes up.

There is **no software lever**: `nxp,enet-mdio` exposes only `pinctrl-0`, and MDC
is derived inside the driver — no clock-rate knob to slow the bus down.

**What this retroactively explains, and corrects:**

- The link that came up and dropped twice earlier, and the cable swap that
  changed nothing. It was never the cable.
- **"Ethernet needs the cable present at BOOT" — retracted a second time, and
  this time for the right reason.** It was neither a PHY-monitor gap nor a
  missing `net_if_up()`; it was whether the MDIO reads at init happened to
  succeed. Boots that worked were boots that got clean reads.
- The `net_if_up()` addition (commit `c97c11b`) stays — bringing the interface up
  explicitly is correct regardless — but **its claimed effect is unproven**, and
  the commit message overstates it. Cable-at-boot worked before that change too.
- Several hours of firmware hypotheses (PTP stack, the TIMER1 pinmux, config
  ordering) were all wrong. **Stock firmware on the same hardware would have
  partitioned this in one flash**, and it is the second time today that lever was
  reached for last instead of first.

**Standing lesson:** when a driver reports a specific device state, check whether
that state is *inferred from a read that could have failed*. "Factory mode",
`ptp/ns` advancing at 0.15x, `offset_from_tt: 0 ns`, `save FAILED (-1)` — four
times today a confident-sounding report was a failed measurement in disguise.

**Practically:** USB-HS on this box is unaffected and measured clean (866–890 us
loopback, zero resync discards over thousands of frames), so development
continues there. The PTP result (±100 ns, `host/start_ptp.sh`) was taken while
the link was healthy and stands, but cannot be re-verified until the MDIO issue
is resolved. Worth checking power delivery first — the board is fed from two USB
cables — before concluding the board is faulty.

### CORRECTION + the actual rule: the RW612 needs a FULL power discharge

**Retracting "the MDIO bus is intermittent (hardware)" from earlier today.** It
is not a marginal bus. Two things were going on, and both are recoverable:

1. **A partially-seated USB-C connector.** With it loose, reading PHYID1
   repeatedly gave `0x0 / 0xffff / 0xffff / 0x5`. With it properly seated, the
   same read gives `0x22` three times out of three, and BMSR `0x784d` twice out
   of two. The "flaky MDIO" was marginal power, not silicon.
2. **The PHY latching a bad state at power-up**, which a quick USB unplug does
   NOT clear.

**THE RECOVERY RECIPE (verified):**

```
1. remove EVERYTHING -- both USB cables AND the Ethernet cable
2. wait ~10 s for the rails to actually drain
3. connect the ETHERNET cable FIRST
4. then power (USB)
```

Ordering matters: the PHY latches its straps when reset deasserts, within
milliseconds of power arriving. A board still partly charged, or powering up
with no link partner present, latches differently. A quick unplug/replug is NOT
enough and reproducibly leaves the box with no Ethernet.

**What this supersedes.** This single quirk explains essentially every Ethernet
mystery in this document from 2026-07-25 onwards:
- the link that came up and dropped twice, and the cable swap that changed
  nothing (the cable was always fine);
- **"Ethernet needs the cable present at BOOT" -- retracted for the third and
  final time.** The real rule is the discharge, of which cable-at-boot is one
  necessary part;
- the `E: PHY is still in factory mode!` abort -- that message reports a failed
  MDIO read, not a device state;
- OMSO reading `0x22` (NAND-tree bit set) and CTRL2 reading `0x0`. **Both read
  the same when the link WORKS**, so they were never the fault. Do not chase
  them; that diagnosis was wrong.

**A driver patch that is still worth keeping** (`patches/ksz8081-retry-mdio.patch`):
upstream runs reset -> readiness-check -> static-cfg exactly ONCE, so a single
bad MDIO read aborts Ethernet permanently for that boot. Retrying costs nothing
on a healthy board and removes one whole failure mode. It is not a fix for the
discharge issue and should not be described as one.

### Registration DOES survive a dserv restart -- but lands INCOMPLETE

Verified by restarting dserv while watching the box console:

```
reg: config link down -> re-registering (x1)
reg: config link restored
reg: registered as extio/box (INCOMPLETE, watchdog will retry)
```

So the `server_up` watchdog fires correctly and the connect-back is
re-established (`%reg` succeeds -- dserv reconnects to the box's port 5010).
What fails is one or more of the `%match` lines, leaving the box **publishing
but deaf**: uplink datapoints keep flowing while `cmd/*` never arrives, which
reads exactly like a working box.

Not the Pico's old fire-and-forget bug -- this port already waits for dserv's
`1 ...` acknowledgement (`box_net_eth_send_command`), and already rotates the
source port over `55000..55007` with `SO_REUSEADDR`. So `INCOMPLETE` is dserv
*declining* a `%match`, not a dropped write. Direct testing shows dserv accepts
`%match` fine from a fresh connection, so the cause is still open.

**Diagnostic that matters:** `state/watchdog` advancing proves only the UPLINK.
Test the downlink explicitly (`cmd/do/<pin>` -> `state/do/<pin>`) before trusting
a box, and treat `INCOMPLETE` in the boot log as "this box cannot be commanded".

### STEP 3 DONE: box_clock anchored from PTP

```
ptp/offset_us      -36999999      (D = dserv_us - ptp_us; the 37 s TAI offset)
sync/source        ptp            (fourth source, beside hw / swc / sw)
sync/ptp_window_us 1-2 us         (box pairs its local clock and PTP clock)
```

Re-anchoring once a second, sampled over 16 s:

```
offset delta per re-anchor : 0, +2, 0, 0, +1, 0, +1, 0 us
total drift                : 4 us / 16 s = ~0.25 ppm
```

**The step is the point.** This document's "Never anchor mid-obs" rule exists
because a re-anchor STEPS the offset by the transport jitter -- hundreds of us
for an obs anchor, which silently corrupts any interval computed across it. A
PTP re-anchor steps by **0-2 us**, because there is no transport in the path: the
box reads its own two clocks, and the host-supplied D is a constant derived from
two LOCAL clock reads. The obs gate is kept anyway (correctness, not magnitude).

Error chain, each link measured:

| link | mechanism | accuracy |
| --- | --- | --- |
| box 1588 <-> Pi PHC | `ptp4l`, hw timestamps both ends | ~±100 ns |
| box local <-> box 1588 | sandwich read, `sync/ptp_window_us` | ~±1 us |
| PHC rate <-> system clock | `phc2sys` | 0.002 ppm |
| PHC <-> `CLOCK_MONOTONIC` | `host/phc_offset.c` | ±703 ns |
| `CLOCK_MONOTONIC` -> dserv | `dservClockEpochOffset` | exact |

**~±2 us end to end**, against the TTL hardware anchor's 35 us -- about 17x
better -- and ~500x inside the 1 ms requirement. Dominated now by the two
software pair-reads (box-local<->1588 and PHC<->MONOTONIC), not by PTP.

**What is still NOT established.** This is internal consistency, not absolute
accuracy: every number above is the mechanism's estimate of itself. Certifying it
needs `host/clock_err.sh`, which differences one physical edge stamped
independently at both ends -- the same way `hw` / `swc` / `sw` were certified.
Until that runs, do not quote ±2 us as end-to-end timestamp accuracy.

Operating notes: `ptp4l` AND `phc2sys` must both be running (`host/start_ptp.sh`,
`host/start_phc2sys.sh`); without phc2sys the PHC free-runs at ~46 ppm and D is
not constant. Re-run `host/ptp_anchor.sh` every ~400 s, or once a session --
the box re-anchors itself from D in between, costing zero packets.

## 2026-07-27 — MCUboot on the RW612, and the PLL the bootloader takes with it

Step 1 of the OTA port (`sysbuild.conf`, `SB_CONFIG_BOOTLOADER_MCUBOOT=y`).
**Done and rig-green: 12/12 with box1 running under MCUboot.** The build side
was free — the board's DTS already had the A/B layout, `zephyr,code-partition`
already pointed at `slot0_partition`, and `NXP_RW6XX_BOOT_HEADER` is already
`default y if !BOOTLOADER_MCUBOOT`, so the app drops its boot header and relinks
to `0x18020000` on its own. MCUboot fits in 45.6% of the 128 KB boot partition;
the signed app is 639 KB = **20.4% of a 3 MB slot**. Swap mode resolves to
`BOOT_SWAP_USING_OFFSET` (no scratch partition in the DTS) — note for step 2/3:
**offset mode wants the update written at the SECOND sector of slot1, not the
first.** Signing is still MCUboot's published dev key; a real key is step 4 work.

### The failure, because it is worth recognising again

First boot under MCUboot: bootloader banner fine, image validated, `Jumping to
the first image slot` fine, app banner fine, `PHY (2) is entering autonegotiation
sequence` — then nothing, forever. No DHCP, no ARP, no crash, no fault message.
`rig_check` cannot see this box at all.

What it actually was, via SWD:

- `curr_tick` frozen at a **repeatable** 8.37 s (10.43 s with BT built in).
- OS timer IRQ 41 **enabled and pending and never taken**; VTOR correct.
- PC in `ENET_Ptp1588CaptureBlocking()`, `PRIMASK=1`.
- `ATCR` `0x291` → `0xA91`: bit 11 `CAPTURE` set and never self-clearing.
  `ATPER` = 1e9 and `EN` = 1 — the timer is *configured*, `ATVR` stays 0.

The mechanism is in `soc/nxp/rw/soc.c`:

```c
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(enet)) && CONFIG_NET_L2_ETHERNET && CONFIG_ETH_DRIVER
        RESET_PeripheralReset(kENET_IPG_RST_SHIFT_RSTn);
        RESET_PeripheralReset(kENET_IPG_S_RST_SHIFT_RSTn);
#else
        CLOCK_DeinitTddrRefClk();      /* powers the TDDR PLL DOWN */
#endif
```

MCUboot has no networking, so **the bootloader powers down the TDDR PLL** — the
source of `tddr_mci_enet_clk`, the 1588 timer clock. Our app takes the other
branch, which only resets the peripheral. **Nothing in the whole Zephyr tree ever
calls `CLOCK_InitTddrRefClk()`**; the ENET path silently assumes the boot ROM
left that PLL up, which is true booting from ROM and false the instant a
bootloader sits in front. This is an upstream defect, not a config mistake.

Two things make it nastier than "Ethernet is broken". The IPG bus clock is
untouched, so ENET registers read normally and MDIO works well enough to *start*
autonegotiation — the box looks like it is trying. And the SDK's capture wait is
an **unbounded spin held under `DisableGlobalIRQ()`**, so a dead clock on a
peripheral we merely *read* takes the entire box down, interrupts and all. Same
family as the `ENET_Ptp1588StartTimer` rate bug of 2026-07-25: check that the
clock is RUNNING, not that the register accepted the write.

Fix: `src/platform/box_soc_rw612.c`, a `PRE_KERNEL_1` `SYS_INIT` that mirrors the
deinit in reverse — only when the PLL is actually found powered down, so the
non-MCUboot image keeps its long-validated path byte for byte. Note
`CLOCK_InitTddrRefClk()` does **not** clear the output gates its own deinit sets,
so `TDDR_MCI_ENET_CLK_CG` has to be cleared by hand. `TDDR_MCI_FLEXSPI_CLK` is
deliberately left gated exactly as MCUboot left it: FlexSPI runs from the TCPU
branch here, and the proof is empirical — MCUboot calls that deinit and keeps
XIP-ing out of the same flash.

### Method note

The cheap discriminator was the A/B, not the theory. Reflashing the pre-MCUboot
image brought box1 straight back (ping, watchdog, DHCP, PTP anchored), which
killed "the PHY is marginal on this board" — a real documented failure mode that
fit the symptom perfectly and was wrong. The BT-off build then killed the BT/NBU
theory that the 10.43 s freeze had made attractive (10 s is exactly Zephyr's
`HCI_CMD_TIMEOUT`): with BT out it still froze, just at 8.37 s. **BT only moved
when the first PTP read happened.** Two plausible, well-motivated theories, both
wrong, both cost one flash each to rule out.

### NVS

Confirmed by round-trip, not by inspection: set `config/desc`, `cmd/save`,
`cmd/reboot`, and the value came back from flash on a fresh boot (`watchdog=1`),
box re-registering with its persisted identity and the same 76 keys as baseline.
`storage_partition` at `0x620000` is outside both slots, so "config survives an
update" is a property of the layout — but the *write* path under MCUboot is now
tested too, which is the part the layout does not give you for free.

## 2026-07-27 (later) — OTA step 2: slot1 writes, and what they cost the RT loop

`src/platform/box_ota_flash.c` opens `slot1_partition` via
`flash_area_open(FIXED_PARTITION_ID(slot1_partition))` and erases / programs /
reads it in area-relative offsets, so the caller cannot reach slot0 and never
touches the `zephyr,mapped-partition` XIP-address trap that `box_flash.c`
documents. `cmd/ota/flashtest <kb>` drives it with a deterministic pattern and
reads three pages back, so a pass means the bytes LANDED, not that the calls
returned 0. Rig-green afterwards: 12/12.

Slot geometry confirmed live: **3,145,728 B, 4 kB sectors**, `verify=1`, `rc=0`
at every size tried. Writing 512 kB of pattern into slot1 while XIP-ing out of
slot0 works — that was the open question, and the answer is yes.

### Measured (box1, MCUboot, slot1)

| size | wall | erase max | prog max (256 B) |
|---|---|---|---|
| 64 kB | 1010 ms | 56.0 ms | 885 us |
| 128 kB | 2065 / 2089 ms | 59.4 / 59.5 ms | 937 / 987 us |
| 256 kB | 4074 / 4117 ms | 57.7 / 59.4 ms | 928 / 992 us |
| 512 kB | 8297 / 8351 / 8409 ms | 60.7 / 61.2 / 62.9 ms | 861-954 us |

Linear at **~16 ms/kB (~62 kB/s)**, so a 640 kB image is **~10.5 s** of flash
work. Erase dominates: 16 sectors x ~56 ms = ~0.9 s of the 64 kB run's 1.01 s.

### The number that actually constrains the design

**Flash work blocks the service loop completely, for its whole duration.**
`state/watchdog` sat frozen at 554 for the entire 8 s of a 512 kB burst and then
jumped straight to 563 — it did not tick once. The box is deaf for the duration:
no watchdog, no downlink, no publishes.

I initially read this the other way. `dbg/loop_max_us` peaked at ~1 ms during a
burst, which looks like "the loop kept running" — but that counter is published
BY the loop and reset after publishing, so a stalled loop simply cannot report
its own stall. **A counter that has to be published to be seen cannot measure the
thing that stops publishing.** The watchdog freeze is the honest instrument.

So the receive path in step 3 must be **incremental — one chunk per frame,
returning to the service loop between chunks**. Then the RT hole is bounded by a
single sector erase (**~63 ms worst seen**) instead of the whole image. Cumulative
deafness over a 640 kB image is still ~10 s, in ~60 ms slices, so an OTA WILL
make the box miss watchdog beats: keep it obs-gated (it is — the handler refuses
with `-EBUSY` while `box_obs_active()`), and do not let a fleet monitor read those
missed beats as a dead box.

### A telemetry trap that cost a wrong diagnosis, again

For several runs `state/ota/dbg/bytes` stayed at the first run's 65536 while the
box console showed every run completing correctly at the right size. The results
were not missing, they were **dropped**.

Mechanism: the 1 Hz gate is `next_wd += 1000`, so after an 8 s stall it fires
**eight times in eight consecutive ~1 ms passes**, each pushing a full status
burst — several hundred frames into a 40-deep queue (`dbg/pubq_dropped` 128 ->
358). Anything published once, right at the end of a long stall, is exactly what
gets dropped. Note the first fix failed too: re-announcing for "5 ticks" is spent
in ~5 ms inside that same storm. Only a **wall-clock deadline**
(`ota_dbg_until = k_uptime_get() + 6000`) survives it.

This is the third appearance of the same shape — OTA.md's RP2350 "nothing that
would show success was visible, even though it succeeded", and this morning's
MCUboot hunt. **When box state and box console disagree, believe the console.**

Related, NOT fixed here because it touches the shared RT loop: the `next_wd +=
1000` catch-up burst is itself worth reconsidering (resync to `now + 1000` after
a long stall) — it converts any stall into a telemetry outage on top of the stall.

### Follow-up: the 1 Hz catch-up burst, fixed at the source

The `next_wd += 1000` behaviour noted above is now fixed rather than just
documented, because step 3 streams chunks through exactly this path.

The gate still advances phase-preservingly in the normal case (no drift). What
changed is that falling a full period behind no longer fires the gate once per
missed period: it emits ONE status burst, adds the missed beats to the count, and
resyncs. `state/watchdog` therefore still reads as seconds-since-boot -- rig_check
and any historical comparison are unaffected -- and the gap is now published as
**`dbg/wd_skipped`** rather than being inferable only from a jump in the watchdog
value. A stall is easier to see than before, not harder.

Measured across an identical 512 kB burst, before -> after:

| | before | after |
|---|---|---|
| `dbg/pubq_dropped` | +230 | **0** |
| `dbg/wd_skipped` | (did not exist) | 7-8 |
| `state/ota/dbg/*` | dropped; read as "never ran" | lands first time |

The wall-clock re-announce on the OTA result is deliberately KEPT even though its
provoking cause is gone: a terminal result published exactly once, at the end of
the operation most likely to have disturbed the link, is the worst-placed frame in
the system. It is now defence rather than the only thing holding the report up.

## 2026-07-27 (later still) — OTA step 3: the image actually lands in slot1

`cmd/ota/begin "<sha-hex> <size>"` → `cmd/ota/chunk` × N → `state/ota/ack` →
verify. **Rig-proven end to end on box1: a real 644,544 B signed image staged in
slot1 and confirmed present.** Rig 12/12 after.

`src/core/box_ota.h` is the RP2350's `pico_ota.h` ported (`pico_*` → `box_*`):
page buffering, erase-as-you-go, sha256, STAGING/VERIFY/DONE. Two deliberate
changes: the **erase granularity is a runtime field** taken from
`flash_get_page_info_by_offs()` rather than a hardcoded 4096 (a module that
disagrees with the device about sector boundaries erases the wrong span and eats
already-written data), and `box_ota_begin()` validates that a sector is a whole
number of pages instead of trusting the caller.

### Carrier: datapoints, not 'D' frames

The framer here already accepts `DSERV_OTA_CHAR`, so 'D' frames looked free — but
they are raw bytes on the box's link, and **on an Ethernet box no host can inject
those**: dserv owns the single connect-back socket on `BOX_ETH_CFG_PORT`, and a
second connection displaces it, killing the downlink for exactly the operation
that needs it. The RP2350's other carrier, the `<`-get pull, needs box-side socket
code this port does not have.

So the image rides ordinary datapoints. It costs payload — the name
`extio/box1/cmd/ota/chunk` eats 24 of the 109-byte budget, leaving 85, minus an
8-byte `[seq u32][crc32 u32]` header = **77 B/chunk** vs 117 for a 'D' frame — and
buys a path that works today over both carriers with no new transport. The chunk
size is computed per box, not hardcoded: a longer box name silently shrinks it and
a fixed 77 would overflow the frame builder and drop every chunk. `box_ota.h` is
front-end agnostic, so a 'D'-frame or pull carrier can be added later untouched.

Host side is `extio_ota_push_dp` in `config/extioconf.tcl`, reusing the USB path's
debounced ack-driven tail-resend verbatim — only the frame emitter differs.
`extio_ota_push` dispatches to it on `state/board` matching `frdm_*`.

### Measured, box1, 644,544 B

| | |
|---|---|
| transfer + verify | **17 s** (~8,371 chunks; flash work alone is ~10.5 s) |
| erase max / prog max | 63.2 ms / 1.25 ms |
| **`dbg/wd_skipped`** | **0** |
| `dbg/pubq_dropped` | 0 |

**`wd_skipped = 0` is the headline.** Step 2 showed a 512 kB burst freezing the
loop for 8 s straight; done incrementally — one chunk per frame, back to the loop
between — the same volume of flash work costs **not one missed 1 Hz beat**. A
63 ms hole per sector is invisible at a 1 s heartbeat. The step-2 measurement
predicted exactly this, and it held.

### Two things that are checked, and one that is not

- **Wrong sha is rejected**: pushing with a zeroed sha gives `state=fail`,
  `result=sha_mismatch`. Verify is real, not decorative.
- **`cmd/ota/verify` re-hashes the image BY READING IT BACK OUT OF slot1**
  (`flash_verify=1`, 644 kB read + hashed in 200 ms). This is NOT redundant with
  the transfer sha, which hashes bytes as they ARRIVE: `ota/state=ok` means "the
  image crossed the link intact and no flash call returned an error", which says
  nothing about what is in the slot. Step 4 must run this before arming a trial —
  MCUboot will happily try to boot a slot we merely believe we wrote.
- **Not checked: that the staged image is BOOTABLE.** Nothing here inspects the
  MCUboot header, and the slot still ends with 512 kB of `flashtest` pattern past
  the image. Step 4's `boot_request_upgrade()` is where that gets real, and note
  swap-using-offset wants the update at slot1's SECOND sector.

Note the whole step was validated without deploying `extioconf.tcl` (that needs
sudo on the rig): the procs were loaded into the live subprocess with
`dservctl extio "source /tmp/otadp.tcl"`. Good pattern for orchestrator work.

## 2026-07-27 — office-stim: a second PTP host, and a better-instrumented one

A second site (office) now has a PTP-capable host. It matters beyond convenience:
every clock number in this document so far came from ONE host (rpi500), and
single-host measurement has confounded conclusions here repeatedly. This is the
independent cross-check that was missing.

**Hardware:** `office-stim`, Intel **I226-V** (`igc`), `enp86s0`, PHC `/dev/ptp0`,
Debian 13 / kernel 6.12. MikroTik **hEX** (RB750Gr3, RouterOS 7.23) between it
and the boxes.

### It is better than the Pi 5 on the term that was limiting us

The `igc` driver implements **`PTP_SYS_OFFSET_PRECISE`** (PCIe PTM hardware
cross-timestamp). The Pi 5 does not, so on the rig the PHC <-> `CLOCK_MONOTONIC`
term can only be measured by sandwich reads and is bounded by the read window.

| | rpi500 (Pi 5) | office-stim (I226-V) |
|---|---|---|
| method B available | no | **yes** |
| `phc_offset --once` error bound | +/-704 ns (method A) | **+/-11 ns (method B)** |
| PHC<->MONO drift, disciplined | -- | **-0.3 ns/s = 0.000 ppm** |
| re-measure interval for 1 us | ~400 s | **~3200 s** |

That is a **64x** tightening of one of the two software pair-reads that dominate
the ~+/-2 us end-to-end budget (2026-07-26 section), and it is measured, not
inferred: B's first->last samples moved 5 ns over 29.8 s, independently agreeing
with the -0.3 ns/s fit.

**phc2sys is what buys the drift figure.** Undisciplined, the same host measured
229.8 ns/s (0.230 ppm) -- a ~750x difference. Do not quote a drift number without
saying whether phc2sys was running.

### Two traps this host re-taught

**`--hwts_filter full` is NOT Pi-specific.** `ethtool -T enp86s0` lists exactly
`none` and `all` under Hardware Receive Filter Modes -- same as the Pi 5's macb.
So ptp4l's default per-protocol PTP filter is not honoured here either, and
without the flag every event message arrives untimestamped, leaving `mean_delay`
and `offset_from_tt` at 0. **Check that list on any new host** before assuming
the default works. `host/start_ptp.sh` now takes an interface argument
(defaults to `eth0`, so the rig is unchanged).

**An NTP daemon is not a conflict in this direction.** `phc2sys -c <iface> -s
CLOCK_REALTIME` disciplines the PHC FROM the system clock, so it is a consumer;
`systemd-timesyncd` steering that clock is fine and the PHC follows. The clash to
avoid is the opposite direction (`-s <iface> -c CLOCK_REALTIME`), where phc2sys
and NTP both steer. I initially advised stopping the NTP daemon here, which was
wrong for the configuration the rig actually uses.

### Method A is noisy on this host -- and that is why drift comes from B

A's read window measures **min 3.2 us but MEDIAN 170 us**, a ~50x spread that is
preemption and C-states, not the PCIe read. Its drift fit is correspondingly
useless (mean |resid| 1584 ns vs B's 16 ns), and in the disciplined run A and B
report 116.7 vs -0.3 ns/s. **That disagreement is not a stop condition**: the
cross-check the tool enforces is on the OFFSET, which agreed to -135 ns inside
A's 3213 ns window. If method A is ever needed on a host without
cross-timestamping, put the box on the `performance` governor first.

### The switch is already correct -- do not repeat the rig's fix

`/interface bridge port print` shows the **`H` flag on all four ports**, i.e.
hardware offload IS active despite `protocol-mode=rstp`. The rig's failure --
RSTP silently disabling offload and costing 45 us of CPU forwarding -- **does not
reproduce on this hardware/RouterOS**. Do not disable RSTP here; there is no
measured gain and it removes loop protection.

Also: the hEX has **no radio**, so the rig's "WiFi bridged with the wired ports
floods the 100 Mb box link" failure cannot happen. And `ether1` (uplink) is
outside the bridge, so office LAN broadcast traffic does not reach the box ports.

Worth knowing for later: the hEX is **not** PTP-capable hardware (transparent /
boundary clock is CRS3xx-class), so its residence time stays inside the error
budget rather than being cancelled. Expect worse than the rig's direct-cable
+/-100 ns when measuring through it -- quantifying that gap is the interesting
measurement, not a fault.

**A speed-mismatch worry I raised and then withdrew:** a 1 G host port feeding a
100 Mb box port does NOT inject a large fixed asymmetry. Each direction crosses
both speeds exactly once (fast-in/slow-out vs slow-in/fast-out), so serialisation
largely cancels, and PTP's four message types are all similar sizes. Pinning both
ports to 100 Mb would cost host bandwidth for no real gain. The real risk through
a store-and-forward switch is queuing VARIABILITY, not the speed step.

## 2026-07-27 (later) — the recurring bug: fields that report memory, not reality

Four distinct hours-costing failures in this document share one shape, and it is
worth naming rather than rediscovering a fifth time. **A field that looks like
live state but is actually configuration, a retained value, or a stale snapshot
is worse than no field**, because it converts "I don't know" into a confident
wrong answer — and the tooling built on top inherits the lie.

| field | looked like | actually was |
|---|---|---|
| `net.ip` in `show` | the address the box holds | the STATIC config field; `0.0.0.0` in DHCP mode no matter what lease exists |
| `state/sync/source` | whether the box is anchored | a RETAINED datapoint; survives the box reboot that destroyed the anchor |
| `persist=FAILED` in `show` | current persistence state | a boot-time snapshot |
| `PHY is still in factory mode!` | a device state bit | a FAILED MDIO READ (all-ones sets the tested bit) |
| `dbg/loop_max_us` | worst service-loop pass | published BY the loop, so it cannot report the stall that stops publishing |

Two of these were fixed today; the rule they imply:

**1. Report reality, not the last thing you were told.** `show` now prints
`net.live=<addr> link=N` from `box_net_eth_get_ip()` beside the configured
`net.ip`, and `dserv.live=<session> uplink=<x> cmds_rx=N`. Both are live samples.
The configured fields are deliberately KEPT — conflating "what you asked for"
with "what you got" is how you lose the ability to tell a static config from a
DHCP lease.

**2. A retained datapoint must be corrected by the thing that invalidated it.**
`state/sync/source` is now republished on every dserv connect with the box's
ACTUAL anchor state (`ptp` if `ptp_offset_valid`, else `none`). Conditional on
purpose: the same path fires on a dserv RESTART, where the box is still up with
its anchor intact, so an unconditional `none` would destroy a good value to fix
a stale one.

This one was not hypothetical. `rig_check` step 5 read the retained value and
**passed while the box refused every at_abs** — observed twice today, on box1
this morning and box3 this evening (step 5 PASS, step 7 `unsynced`, and the PASS
was the one lying). Verified fixed: reflashing box3 flipped dserv `ptp` -> `none`
immediately, matching `sync_fire`, and re-anchoring flipped it back.

**3. `cmds_rx` is now on the box's own console.** main.c's comment for that
counter ends "no field on the box revealed it" — true until now: it was published
to dserv but the box could not show it locally, so a box you were holding on a
bench could not tell you its downlink was dead. `show` reports it.

**Still outstanding, a decision not a bug:** `ble.en=0` in `show` while the radio
is up and scanning. `box_ble_init()` is called unconditionally under
`#if defined(CONFIG_BT)` and never consults `cfg.ble_en`, so the persisted flag
is inert — diverging from the RP2350's stated "ENABLE CONTRACT: persisted
cfg->ble_en, default OFF" (`box_ble_central.h`). Either gate init on the flag or
drop the flag; do not leave a config field that does nothing.

## 2026-07-28 — OTA steps 4 and 5: trial boots, and making a rollback visible

Step 4 gave the box a trial boot (`cmd/ota/arm` → `BOOT_UPGRADE_TEST`,
`cmd/ota/confirm` → `boot_write_img_confirmed()`) and proved both directions on
box3: reboot without confirming reverts to the old image, confirm and it is kept.
Permanent upgrade is deliberately not offered — a one-way door on a box that may
be physically inaccessible; confirmation must be earned by the running image.

**Step 5 is about the failure that leaves no trace.** A revert is silent by
construction: MCUboot swaps the old image back and it boots normally. Nothing is
broken, nothing logs, the fleet page shows a healthy box — running firmware
nobody chose. That is worse than a crash, because a crash gets investigated.

`src/box_boot.c` answers three questions once at boot and publishes them on every
connect: why we reset, which image this is, and whether an armed update failed.

### Three verdicts, all proven on silicon

| `state/boot` | meaning | proven by |
|---|---|---|
| `trial` | running an unconfirmed image; the next reset undoes it | arm → swap |
| `revert` | an armed image ran and was not kept | reboot without confirm |
| `rejected` | an armed image **never ran** — MCUboot would not take it | corrupted payload |

`state/fw_ver` carries the MCUboot header version of the running image
(`0.0.0+0` → `0.0.2+0` → back), which is what makes a swap self-evident.
`state/fw` cannot do this job: it is a build-time string, still `"dev"` in every
image this tree produces.

### Why not compare versions to tell revert from success

Because dev images routinely share a version, and that is exactly how the RP2350
OTA looked stuck for a day when it had in fact succeeded (OTA.md, 2026-07-14:
"base and trial shared a version, so `state/fw` never changed on commit"). The
breadcrumb instead records whether a trial boot was ever **seen**:

- armed, trial seen, now confirmed+old → **revert**
- armed, trial never seen → **rejected**

Version-independent, and the rejected case is the one worth naming. The test for
it staged an image with the *same* version the box was already running, so a
version comparison would have reported a successful update.

### The rejected test is the interesting one

Take the signed image, flip one byte in the middle, push it. Every check we have
passes — the transfer sha matches (it is the sha of the corrupted file), the
read-back verify matches, and `ota/hdr_ok=1` because the header is untouched.
Arm it and MCUboot silently declines to swap. Result: `state/boot=rejected`,
`ota/rejects=1`, `ota/last_arm_ver=0.0.2+0`. **An image can pass every test the
application is capable of and still never run**; only the bootloader's verdict
settles it, and step 5 is how that verdict gets off the box.

### The breadcrumb is in NVS, not RAM

The revert you most want to see is the one after a power cut during a trial —
exactly the case where a RAM breadcrumb (noinit, retention, watchdog scratch: the
RP2350 approach) is gone. Stored under its own NVS id, **not** in the
`box_persist` config blob: that blob is written by an operator's `cmd/save` and
carries whatever the live config happens to be, so sharing a key would mean
arming an update silently persists unsaved config edits, and a `save` clobbers
OTA bookkeeping. Different writer, different lifetime, different record.
Verified: the lifetime counters survived a full `west flash` of slot0.

### Reset cause was a "field that reports memory" waiting to happen

`box_announce` read `hwinfo_get_reset_cause()` at every connect and never cleared
it. That register **accumulates**: one watchdog trip and the box answers
"watchdog" to every boot for the rest of its power cycle. Now read once, latched,
and cleared — so `state/boot` describes *this* boot. (Same family as `net.ip`,
`sync/source`, `persist=FAILED`; see the 2026-07-27 section.) Note the RW612 has
no power-on bit at all — `power_reset_cause_t` stops at `ResetB` — so a clean
cold start sets nothing and reports cause `0x00`.

### A header check that would have caught step 4's bug

`cmd/ota/verify` now also reads the MCUboot header out of slot1 at the offset the
bootloader will use, and `cmd/ota/arm` refuses without it (`ota/hdr_ok`,
`ota/staged_ver`). The sha answers "are these the right bytes"; this answers "are
they in the right place" — a separate question with its own failure, which is
precisely what step 4 hit by writing at slot offset 0 under swap-using-offset.

### rig_check step 5, and where it must NOT go

`rig_check.sh` now checks firmware identity: a box that rolled back, or one left
mid-trial, fails. It was written as step 3 — identity before anything that
measures — and that was **wrong**, caught on the first run: these are RETAINED
datapoints written by the announce burst, so on a box still booting they describe
the previous connection. It cheerfully reported the pre-reflash firmware. Moved
after the liveness checks, because `cmds_rx` advancing is what proves the
connection that wrote them is the current one.

## 2026-07-28 (later) — where the boot time actually goes

"Boot seems long, with a 7-10 s pause in the middle." Measured on box3 by
stamping both consoles from the host through `kernel reboot cold`
(`bootstamp.py` pattern: hold the fd across `stty`, macOS discards settings
applied to a closed `cu` device). Baseline was 13.9-15.9 s from reset to
`reg: registered`, wandering ~2 s between otherwise identical reboots.

**The pause is dark on purpose and that is half the problem.** Everything in
that window is printed by `box_console_printf`, which on an Ethernet box goes to
a USB CDC that `mode eth` never enumerates -- so the most interesting 9 seconds
of the boot produce no output anywhere. `printk` (Zephyr console, the J-Link
VCOM) is the only channel that always works; the timing marks used here went
there, and the one line worth keeping stayed there.

Breakdown, uptime ms (uptime 0 == MCUboot's jump, ~310 ms after reset):

| phase | cost | |
|---|---|---|
| MCUboot verify + chainload | 310 ms | signature over 650 kB |
| Zephyr driver init to `main()` | 2580 ms | before our first instruction runs |
| `k_msleep(2000)` | 2000 ms | deliberate: let a USB host open the console |
| LED boot heartbeat | 750 ms | deliberate: 3 hardware-timed pulses |
| `box_net_eth_wait_ip()` | 0.5-4100 ms | link-up + DHCP |
| **service loop -> registered** | **5170 ms** | **the bug** |

### The 5.17 s was a defect, not a cost

Identical in every boot, on a rig where a register round takes 170 ms.
`box_uplink_init()` fires a registration while bringing the transport up, which
on a cold boot necessarily runs BEFORE DHCP has an address, so it always fails;
`eth_reg_watchdog` then charged the full `REG_RETRY_MS` (5 s) cadence before
trying again, while the box sat ready. Now the retry is 250 ms until the FIRST
successful registration and 5 s after it -- the slow cadence is for a dserv that
is down, and a box that has never once been up is a different situation (and the
one someone is standing there watching). `reg: first registration at N ms` is
now printed, which is the single number that says when the box became useful.

### DHCP was a real find but not the one that showed

`CONFIG_NET_DHCPV4_INITIAL_DELAY_MAX` defaults to 10, and Zephyr implements
RFC 2131 4.4.1 as `entropy % (MAX - 1) + 1` -- **a random 1-9 s before the first
DISCOVER, every boot.** That is the reboot-to-reboot wander. Kconfig's range
floor is 2, which collapses the expression to exactly 1 s. Set on both
networked boards. Fixing it alone barely moved the total, because the 5 s
registration penalty was quantising everything behind it -- worth remembering:
two delays in series, and only the larger one is visible.

**Now 8.7-9.2 s, repeatable to ~0.5 s.** rig_check 11/11 after.

### What is left, deliberately

- **2.58 s of Zephyr driver init before `main()`** -- the largest single item
  now. `CONFIG_BOOT_DELAY=0`, so this is SYS_INIT work; `CONFIG_BT=y` and the
  RW612 radio bring-up is the obvious suspect (the step-1 hunt already saw BT
  move boot timing by ~2 s). Not chased.
- **2.0 s `k_msleep`** buying nothing on an Ethernet box, where the console CDC
  is never enumerated. Making it conditional is easy and was left alone rather
  than risk the USB-console case for 2 s.

## 2026-07-28 (later) — two boxes "with dead PHYs", neither of which had one

Both box2 and box1 lost Ethernet within an hour of each other while boxes were
being moved around for flashing. Both looked like the board we RMA'd. Neither
was. Total cost: most of an evening, and I sent it down two wrong paths myself.

**box2 = marginal power via a partly-seated USB-C.** The meter read a mix of
`0x0000` / `0xffff` / `0x0005` — PORTING.md's documented marginal signature,
almost byte for byte. Reseating BOTH USB-C connectors took it to **200/200 on
both ID registers at both addresses, six passes running, zero errors**, which is
exactly the baseline box3 set. Not silicon.

**box1 = a USB HUB.** Same class of fault, new cause. **These boards go straight
into the machine — never a hub.** An unpowered hub splits 500 mA across its
ports and the RW612 wants real current once the PHY is running; the result does
not look like a power problem, it looks like dead silicon. That is what makes it
expensive: we came close to RMA-ing box2 on the strength of it, and the only
reason we did not is that the meter reports a RATE rather than a verdict.

**box1, second fault: the wrong switch port.** After the port was changed (for a
shorter cable) box1 linked at **100 Mb full duplex and got no DHCP lease at all**
— `net iface` showed `carrier=ON` with only an IPv6 link-local. Moving it back
fixed it. Note the box stays dark FOREVER in that state: nothing re-runs DHCP
once the initial attempt has failed, so it links, never gets an address, and
spins on re-registration. Worth fixing — a box that is linked but addressless
should say so (`state/net/ip` = none) instead of leaving a stale IP standing.

### The diagnostic split, which is the reusable part

MDIO is the management bus, on the board; it does NOT traverse the RJ45. So:

- **garbage IDs → POWER.** The Ethernet cable cannot cause this. Do not touch it.
- **IDs perfect + autoneg timeout → the wire** (cable or switch port).
- **IDs perfect + `BMSR [LINK aneg-done]` → both are fine**; suspect software.

The meter now dumps BMCR/BMSR/CTL1/CTL2 for exactly this reason (see its README).

### My three wrong turns, recorded so they are not repeated

1. **"Swap the cables."** Reasonable from box2's autoneg timeout, and wrong: the
   meter then showed that same cable carrying a good 100 Mb full-duplex link.
   Worse, the swap meant handling the boards — which is how box1 ended up on a
   hub and in the wrong port. **Read the registers BEFORE moving hardware.**
2. **"The PHY is in power-down or isolate."** A clean hypothesis that fit every
   symptom — perfect MDIO, no link, static LEDs — and BMCR `0x3100` killed it
   outright: neither bit set, autoneg enabled, link up.
3. **Raising `CONFIG_PHY_AUTONEG_TIMEOUT_MS` to 20000 "fixed" box2.** It did not.
   Autoneg completed in 2.3 s, well inside the stock 4 s, so that boot would have
   worked either way — and the stock build then linked 3/3 boots. **The change is
   NOT in the tree**; do not re-add it. The fix was the connector. This is the
   same trap as the DHCP delay earlier today: a change made while a second,
   larger fault was still present looks like the fix when the fault clears.

Also: an IPv6 link-local ping meant as a segment test went out `wlan0`, because
that is the Pi's default route while the boxes are on `eth0`. It proved nothing.
Pin the interface (`%eth0`) when testing link-local reachability.

**Ended 19/19 on box1 + box2**, both PTP-anchored, both firing.

## 2026-07-28 (end) — the MCP3204 path is PARKED, and why

box3 sits at rig_check 11/11 with the analog switched off (`mcp/enable 0`,
persisted). Everything above the converter is proven; the converter itself never
answered. Parked rather than abandoned — the work below is all reusable.

**PROVEN, on hardware:** sweeps at exactly 1000/s with `late` and `sweep_max_us`
frozen after boot; group packing; decimate + batch; the 12-byte block header
decoding byte-for-byte (`ver=1 mask=3 nchan=2 count=12 interval_us=1000`);
publishing to dserv as `state/ain/<label>`; **live reconfiguration with no
reboot**; and the `ain/dbg/*` counters. `box_ain_group.h` also passes all 24 host
tests with `AIN_MAX_CH=8`.

**NOT PROVEN:** a single real sample. Every read came back `0` or `4095` --
all-zeros or all-ones, the "bus nobody is driving" signature from the MDIO work
the same morning.

**Two findings worth keeping, both from the console rather than from reasoning:**

- `E: Slave 1 is greater than max 0` -- this SoC's flexcomm SPI has exactly ONE
  hardware chip select. `reg = <1>` is rejected outright and every transfer times
  out at ~70 ms. A mikroBUS CS on IO10 must be a GPIO.
- **`pinmux_flexcomm1_spi` cannot be used with a GPIO chip select.** The macro
  names mislead: `IO_MUX_FC1_SPI_SS0` is not "the SS0 pin", it is a composite
  muxing IO6/7/8/9, and SS1 muxes IO7/8/9/10 -- so it hands IO10 to the
  peripheral and a `cs-gpios` write to that pad goes nowhere.

**Where it stopped:** per-pin muxing of SCK/MISO/MOSI (IO7/8/9) plus IO10 as
GPIO got transfers completing (`sweeps` 0 -> 28832) but the data stayed
rail-to-rail. Remaining candidates are all on the wire -- whether IO7/8/9 under
the SS0 function encoding really are SCK/MISO/MOSI, whether CS toggles on IO10,
whether the Click is seated and powered -- and **none can be settled by reading
source. Put a scope on the mikroBUS SCK/CS/MISO pins.** Three guess-and-flash
cycles produced nothing; that is the anti-pattern the MDIO hunt cured the same
morning and I repeated it anyway.

**Why parking is cheap:** `box_adc.h` is a sweep over Zephyr's ADC API, so the
group layer never learned which converter is fitted. Moving to the MCXN947's
on-chip `lpadc0` is a devicetree + Kconfig change with `box_ain_group.h` and
`box_ain.c` untouched -- and it deletes the chip-select problem entirely.
(Half right. It does delete the chip select; it replaces it with a
channel-to-input mapping problem. See the next section.)

---

## 2026-07-28 (end) — FRDM-MCXN947: first build, and what did NOT break

**Why this board at all.** The RW612 was chosen as the universal hub, and then
two things became true: we never turned the radios on, and we wanted a real
analog front end. The MCXN947 is the trade — no radio, but `lpadc0` + `dac0` +
`opamp0` on the die, and it keeps everything the port actually leans on:
Ethernet with an IEEE-1588 clock, USB HS, MCUboot A/B partitions, a storage
partition. Zephyr supports `frdm_mcxn947` in tree.

**Result: it builds.** Two new files — `boards/frdm_mcxn947_mcxn947_cpu0.conf`
and `.overlay`. FLASH 314 KB, RAM 130 KB of 320 KB, no errors and no unsatisfied
Kconfig. Done in three stages (USB+GPIO, then Ethernet+PTP+NVS, then an analog
probe) so that each failure had one candidate cause.

### The only thing that actually bit: the file NAME

This is the first board in the tree with hardware-model-v2 **qualifiers** —
`frdm_mcxn947/mcxn947/cpu0`, against the bare `teensy41` / `frdm_rw612`. Zephyr
builds its board-overlay search names from BOARD + BOARD_QUALIFIERS via
`zephyr_build_string(... MERGE)`, and MERGE appends only the *board+qualifiers*
variant — **not the bare board name**. Confirmed in
`cmake/modules/extensions.cmake` (`zephyr_build_string`, the `BUILD_STR_MERGE`
branch) and `configuration_files.cmake:72`.

So `boards/frdm_mcxn947.overlay` is never looked at. It sits in `boards/`
being silently ignored while the build fails on the very alias it defines —
another entry in the FIELDS/FILES THAT REPORT MEMORY NOT REALITY family. The
tell is the absent `-- Found devicetree overlay:` line in the CMake output;
check for it before debugging anything else on a new board.

**Rule: the name must carry the qualifiers, `/` replaced by `_`.** Same for the
`.conf`. Note this also means the file does NOT apply to the `_ns` or `_qspi`
variants — a `frdm_mcxn947_mcxn947.overlay` would cover every cpu variant, but
not the qspi one either, since MERGE adds exactly one parent level.

Everything the naive first build complained about was the predictable trio:
no `box-gpio-port` alias, no `cdc_acm_data`, no `cdc_acm_console`. Kconfig and
CMake configured cleanly on the first attempt.

### What came for free — read from `.config`, not assumed

The board devicetree already enables `enet` / `enet_mac` / `enet_mdio` / `phy` /
`enet_ptp_clock` / `usb1` / `lpadc0`, and every NXP driver behind them is
`default y` on its DT node. Verified present after building:

    CONFIG_ETH_NXP_ENET_QOS=y      CONFIG_MDIO_NXP_ENET_QOS=y
    CONFIG_ETH_NXP_ENET_QOS_MAC=y  CONFIG_PTP_CLOCK_NXP_ENET_QOS=y
    CONFIG_PHY_GENERIC_MII=y       CONFIG_UDC_NXP_EHCI=y
    CONFIG_NVS=y                   CONFIG_FLASH_MCUX_FLEXSPI_NOR=y

**MCUboot works out of the box too.** `west build --sysbuild` completes,
relinks the app to `0x10014000` (= `slot0_partition`, flash base `0x10000000` +
`0x14000`), and emits `zephyr.signed.bin` at 322 KB against a 984 KB slot. None
of the RW612's MCUboot pain (see "the PLL the bootloader takes with it") has an
analogue yet — but that was a silicon-init defect found on hardware, so absence
from a build proves nothing about it.

### Three RW612 traps are STRUCTURALLY absent here

Worth stating explicitly so nobody spends an evening looking for them:

1. **The missing-`pinctrl-0` PTP failure cannot happen.**
   `ptp_clock_nxp_enet_qos_init()` never calls `pinctrl_apply_state()`. That was
   the entire failure path on `ptp_clock_nxp_enet.c`, and it is not in this
   driver. No dummy pinctrl state is needed, and no pad gets consumed for one.
2. **The silent wrong-rate clock bug cannot happen.** Two independent reasons.
   `MCUX_ENET_QOS_PTP_CLK` (`MCUX_LPC_CLK_ID(0x27, 0x00)`) **has** a case in
   `clock_control_mcux_syscon.c:795`, unlike the RW612's `MCUX_ENET_PLL` which
   fell out of the switch and returned success without writing `*rate`. And the
   QoS driver checks the return (`ret = clock_control_get_rate(...); if (ret)
   return ret;`) instead of casting it to `(void)`. The 0.14996x counter has no
   route in.
3. **A box pin cannot be "configured but electrically dead."** Zephyr's
   `gpio_mcux.c` writes `PORT_PCR_MUX(PORT_MUX_GPIO)` inside `pin_configure()`
   (line ~205), so claiming a box pin muxes the pad to GPIO as a side effect.
   The RW612 needed an explicit `pinmux_box_gpio` group and would otherwise
   accept `pin <n> mode out`, report itself in `state/pins/out`, and do nothing
   on the wire. No such group exists — or is needed — here.

### Pin map: gpio0, and why

`box-gpio-port = &gpio0`. Two reasons, both checked against the board dts:

* It carries **11 Arduino header pins inside the `BOX_NPINS = 30` window**
  (a frozen core constant in `dserv_config.h`, not to be raised casually):

  | box pin | Arduino | note |
  |---|---|---|
  | 10 | D9  | also the RED LED |
  | 14 | A2  | |
  | 15 | A4  | |
  | 22 | A3  | |
  | 23 | A5  | also SW2, the user button |
  | 24 | D11 | Arduino MOSI |
  | 25 | D13 | Arduino SCK |
  | 26 | D12 | Arduino MISO |
  | 27 | D10 | Arduino CS, also the GREEN LED |
  | 28 | D8  | |
  | 29 | D2  | |

  (D4 = P0_30 and D7 = P0_31 exist but fall outside `BOX_NPINS`.) Compare the
  RW612, which had **one** free box pin before the overlay went hunting for
  pads — no loopback pair, no way to prove DI at all.
* **ENET_QOS uses none of port 0.** Its RMII/MDIO pads are all port 1 (P1_4..7,
  P1_13..15, P1_20/21). Choosing `gpio1` would have put box pins straight on
  top of the PHY.

The numbering is still scrambled relative to the silkscreen, which is exactly
what the "devicetree pin map" proposal above is for. This board makes the case
stronger, not weaker.

### Flash topology: better than either previous board, by construction

The app XIPs **internal** flash (`slot0_partition` on the `fmu` controller)
while `storage_partition` is the whole 8 MB of the **external** W25Q64 on
FlexSPI. So NVS writes a device the CPU is not fetching instructions from.
Neither the Teensy's XIP-write hazard nor the RW612's "NVS refuses to erase a
region it does not recognise" (`nvs_mount -> -45`) applies here as a matter of
layout rather than of configuration. `box_flash.c` already handles this board's
`compatible = "zephyr,mapped-partition"` style, from the RW612 work.

Still verify on silicon. "Cannot happen by construction" has been wrong before.

### Analog: it COMPILES against `lpadc0`, and that is the dangerous part

Probed with a throwaway overlay labelling the on-chip converter as the box's:

```dts
box_adc: &lpadc0 { #address-cells = <1>; #size-cells = <0>; };
```

plus `CONFIG_ADC=y`. It builds and links (322 KB). **It is NOT wired up in the
committed overlay, on purpose**, because it would be wrong on silicon and wrong
silently — the failure shape this project keeps writing memos about.

**The mismatch.** `box_adc.c` is MCP3204-shaped: channel *n* **is** input *n*,
one 3-byte transaction per channel. `adc_mcux_lpadc.c` does not work that way:

* `channel_cfg->input_positive` selects the physical input (low 4 bits = channel
  number, bit 5 = A/B side — `mcux_lpadc_channel_setup`, lines 214-217);
* `channel_cfg->channel_id` merely indexes a CMD-register slot
  (`data->cmd_config[channel_id]`), bounded by `CONFIG_LPADC_CHANNEL_COUNT`,
  which is **15** on this board.

`box_adc.c` never sets `input_positive`. So every "channel" would sample the
same physical pad (input 0, side A). Worse, `probe_channels()` — which detects
width by walking channels upward until `adc_channel_setup()` refuses one —
would happily report `BOX_ADC_MAX_CH` = 8, because the driver accepts any slot
index regardless of what is wired. A confident, self-consistent, entirely
fictional 8-channel sweep.

(`ADC_REF_EXTERNAL0`, which `box_adc.c` passes, *is* accepted — it means "use
the board's `voltage-ref` setting". That part needs no change.)

**What the board actually brings out:** `pinmux_lpadc0` muxes exactly three
pads — `ADC0_A2/PIO4_23`, `ADC0_A1/PIO4_15`, `ADC0_B1/PIO4_19`.

**So the real work is a channel -> input map, and that map is board wiring,
which means devicetree.** It is a design decision, not a mechanical port, and it
overlaps the pin-map proposal above — both are "the flat wire index must resolve
to something the board chooses." Do them with one idea, not two.

### Everything timing-related is UNMEASURED

The MAC here is **ENET_QOS** (Synopsys DWMAC-derived), a different IP from the
RW612's ENET, with a different PTP clock driver and a different USB controller.
Per the measurement audit above, numbers do not transfer by inheritance. Carried
into `frdm_mcxn947_mcxn947_cpu0.conf` as *starting points with comments saying
so*, not as facts:

* `CONFIG_SYS_CLOCK_TICKS_PER_SEC=100000` (10 µs). The RW612 could not go below
  this — at 1 µs its main loop was STARVED (watchdog stopped after two ticks
  while ICMP kept answering). That was a consequence of its 1 MHz OS timer; this
  board's kernel timer is a different peripheral, so the floor must be found
  again, and by watching `state/watchdog` advance for a full minute rather than
  by a bring-up check that a starved box passes.
* The unexplained **~30 µs skew floor** below the tick on the RW612. Re-measure
  before assuming it is shared.
* `CONFIG_NET_MAX_CONTEXTS/MAX_CONN=16`. Carried as a PROTOCOL-level lesson, not
  a silicon one: at the default 6 the box ends up PUBLISHING BUT DEAF.
* `CONFIG_MAIN_STACK_SIZE=16384`. Deliberately generous. The 4096 default is
  what overflowed on the RW612 and, silently as an MPU fault with no console, on
  the Teensy SD path. RAM is 320 KB with ~40 KB in use; this buys one fewer
  first-boot mystery for nothing.
* The `NET_PKT_*_STATS` instrumentation, A/B'd as free on the Teensy. Re-verify
  here — different MAC driver.

### Next, in order

1. Get the board on the bench: boot log on `flexcomm4_lpuart4` (the MCU-Link
   VCOM, already `zephyr,console`), confirm the PHY announces autonegotiation,
   confirm USB enumerates both CDCs in the right order (console first).
2. Confirm `ptp/ns` advances **and check its RATE**, not just that it moves —
   the RW612 lesson, even though the mechanism that caused it is absent here.
3. `cmd/save` -> reboot -> confirm config survives, i.e. NVS on the external
   QSPI behaves as the layout predicts.
4. Only then the LPADC channel map, designed together with the devicetree pin
   map rather than bolted on beside it.

**Steps 1-3 are DONE — see the next section.** Step 4 is the open work.

---

## 2026-07-28 (end, later) — MCXN947 first boot: it all worked, and the shell lied

Board on the bench the same evening. **Every bring-up step passed on the first
image**, which has not happened before on this project — the RW612 took two
evenings to reach the equivalent point. The one real defect found was in a
setting *we* copied across, not in the board.

### Flashing: external J-Link, not the onboard probe

`pyocd` has no built-in `mcxn947` target and would need an
`NXP.MCXN947_DFP` pack download; `linkserver` is not installed. A Segger J-Link
EDU Mini on the board's **10-pin SWD header J23, with jumper J19 shorted**, works
with the in-tree runner and no downloads:

```sh
west flash -r jlink -d build-mcxn947
```

J-Link Commander V9.54 knows `MCXN947_M33_0`. Connecting halted the core cleanly
(CM33 with security extensions), and — worth knowing — **attaching the probe to a
running box does not disturb it**: the box kept answering its console across
several J-Link sessions, including the 120 s register-sampling run below.

**J19 shorted disables the onboard MCU-Link's SWD, but its VCOM survives** and
stays the Zephyr console. Both USB cables want to be in: J17 for VCOM + probe
power, and the board's own HS USB-C for the box's two CDCs.

### Results

| check | result |
|---|---|
| Zephyr boots | `*** Booting Zephyr OS build v4.4.0-9159 ***` |
| MDIO + PHY | `I: PHY (0) ID 7C121` (Microchip LAN874x family) |
| link | **100 Mb, full duplex** |
| both CDCs, console FIRST | `usbmodem1301` console, `usbmodem1303` data |
| DHCP | `192.168.88.36`, `link=1` |
| PTP hardware clock | `PTP hw clock: ready=1  now=5115039040 ns` |
| NVS persistence | `persist store -> config LOADED from flash` |
| dserv uplink | `dserv.live=connected uplink=eth cmds_rx=1` |

`ready=1` on the first image is the whole point of the driver reading in the
previous section: no dummy pinctrl state, no clock-ID patch, no evening lost.

### THE ONE REAL DEFECT: `CONFIG_SHELL` corrupts the boot log

Copied from `frdm_rw612.conf`, where it earns its place. Here it did two bad
things at once:

1. **The shell never accepted input at all.** No echo, no prompt on demand;
   only the boot-time prompt ever appeared. So it was useless as a diagnostic
   before it did any damage. (Cause not chased — it was removed instead.)
2. **Shell output and `LOG_MODE_MINIMAL` output interleave on the same UART
   with no mutual exclusion and shred each other.**

The evidence, same board, same line, shell on vs off:

    with shell:   Y0) Lnk speed 10 Mb,ulldpe
    without:      I: PHY (0) Link speed 100 Mb, full duplex

**Read literally, the corrupted line says the rig negotiated a 10 Mb link.** It
is 100. A wrong fact, arriving through the one channel we use to establish
facts, in a form that looks like a reading rather than like damage — and it
would have quietly poisoned every timing measurement taken afterwards. I nearly
recorded it. What saved it was noticing that characters were missing *scattered
mid-line*, which is not what UART overrun looks like; overrun truncates.

**A boot log that lies is worse than no shell.** `CONFIG_SHELL` and friends are
now commented out in the board conf with this reasoning attached. Build drops
314 KB -> 218 KB as a side benefit. If the shell is ever wanted here, route logs
*through* it (`CONFIG_SHELL_LOG_BACKEND`) instead of letting both own the UART —
and fix the input path first, or it buys nothing.

Note this combination is also live in `frdm_rw612.conf`. It was never observed
to corrupt anything there, but it was never specifically looked for either.
**Worth a deliberate look before trusting an RW612 boot log.**

### PTP: not just running — LOCKED to the rig grandmaster, unconfigured

The rate check the section above insists on, done with the box left running and
the ENET_QOS system-time registers sampled directly over SWD:

    ENET_QOS base 0x40100000
      +0xB08  MAC_SYSTEM_TIME_SECONDS
      +0xB0C  MAC_SYSTEM_TIME_NANOSECONDS   (mask 0x7FFFFFFF)

Two reads in ONE J-Link session separated by its own `sleep 120000`, so the
interval is a single host-timed span rather than two process launches:

    PTP delta 120.001886 s over 120 s  ->  +15.7 ppm

**Do not over-read that number.** Command latency at each read bounds this
method at roughly +/-25 ppm, so +15.7 is inside its own noise. What it
establishes is the ABSENCE of gross mis-scaling — the RW612's 0.14996x was
-850,000 ppm and would have been unmissable.

**The seconds register was the real find.** It reads wall-clock epoch time, not
a counter that started near zero at boot (the banner shows it at ~5 s). Against
the host:

    PTP - host UTC = +36.977 s

**37 s is exactly the TAI-UTC offset**, and TAI is the timescale PTP runs on. So
`CONFIG_PTP` found the rig's existing ptp4l grandmaster and disciplined the
clock **on first boot with no configuration at all**. The ~-23 ms residual is
the measurement method (the register read precedes the host stamp), not sync
error — real sync quality needs the +/-100 ns-class harness used on the RW612.

This also reframes the rate figure: with the servo steering, +15.7 ppm is not
the crystal's error, it is the measurement floor.

### Talking to dserv: mDNS resolved to the WRONG INTERFACE

`raspberrypi.local` -> **192.168.0.26**, the Pi's house-LAN side. The rig-side
address is **192.168.88.46**, on the box's own subnet. A box configured from the
mDNS answer would have pointed at an address it cannot route to.

Second trap in the same step: **`mode=auto` picks USB whenever USB is plugged
in**, and a USB uplink carries frames to the machine holding the cable — this
Mac — not to the Pi. Reaching dserv over the rig network needs `mode eth`
explicitly.

    dserv ip 192.168.88.46
    mode eth
    save

`dserv.live` went to `connected` **without a reboot** (config applies live), and
`cmds_rx` reached 1 shortly after — so registration, dserv's connect-back, and
the downlink all work. The box is a live extio box on the rig.

### Bench gotchas worth not rediscovering

* **A J-Link reset re-enumerates the board's USB**, so a capture held across the
  reset dies with `OSError: [Errno 6] Device not configured`. Reopen the CDC
  after the reset rather than holding a handle through it. The boot banner is
  still catchable — poll for the device node and open the instant it returns.
* **The box console needs DTR asserted.** `cat /dev/cu.*` deliberately does not
  raise it, so it silently reads nothing. Use pyserial with `s.dtr = True`.
  Likewise `stty -f` settings revert when `cat` reopens the port — set the baud
  on the open handle instead. Both cost a cycle here.
* The box CLI command is **`dump`**, not `state`. On a fresh box `dump` prints
  nothing, which is correct and looks like a hang.

### Still unmeasured

Everything timing. No RTT, no loopback, no scope skew, no tick-floor probe on
this silicon. The conf's carried-over values (10 µs tick especially) remain
untested guesses. ENET_QOS is a different MAC and this is a different USB
controller; nothing above changes that.

---

## 2026-07-28 (end, later still) — LPADC up, and a jumper that answered two questions

Block #7 on the board it was always meant for. **The MCP3204's entire problem
class — SPI, chip select, a Click board seated or not — does not exist here.**
The converter is on the die.

### The pin decision: spend nothing

Box ain channels 0/1/2 = **ADC0_A1 / B1 / A2 = P4_15 / P4_19 / P4_23**, which are
J8 pins 20/24/28. **No pinmux change at all** — `pinmux_lpadc0` in the board's
own pinctrl already brings out exactly these three, and they cost **zero box
GPIO** because box pins live on gpio0 and these are port 4.

The Arduino analog strip was the obvious alternative and is the wrong trade
*today*: A2/A3/A4/A5 are P0_14/P0_22/P0_15/P0_23 = box pins 14/22/15/23, one of
them SW2. Four of our eleven digital pins, spent on channels nothing needs yet.
Recorded in the overlay as the extension path, not as a rejection.

### box_adc.c learned that an index is not an input

It assumed **channel n IS input n** — true of an MCP3204, false of the LPADC,
where `channel_id` indexes a CMD-register slot and `zephyr,input-positive` picks
the pad. Left alone, `probe_channels()` would have "succeeded" for all 15 driver
slots and reported a channel count describing nothing physical.

Now: if the node declares `channel@N` children, **they are the map** and probing
is not used. The MCP3204 path is untouched. The box's flat wire index is still
`channel_id`, so the contract does not move — it just resolves to a pad the
BOARD chose, which is the same principle as the devicetree pin-map proposal.

### `adccal`: one jumper, two unanswerable questions

`DAC0_OUT` is **P4_2 (J1-4)** and the board already pinmuxes it, so a single
jumper to an analog input turns both of these into measurements:

* **which pad is channel N** — a wrong devicetree map reports just as
  confidently as a right one;
* **what the ADC's real full scale is** — `CFG[REFSEL]`, where the FSL headers
  only offer Alt1/Alt2/Alt3.

Added in `box_console.c`, deliberately **not** in `box_cli.h`: that grammar is
shared verbatim with the deployed RP2350 boxes, which have no DAC, and "CLI
overstates the box" is already an open complaint above.

### Result 1 — the map is PROVEN, with a negative control

Sweeping the DAC, **ch0 goes 1178 → 4095 monotonically while ch1 wanders
386–595 with no correlation.** The negative control is what makes this
specificity rather than mere responsiveness — without it, a short or a coupling
path would look identical to a correct map.

### Result 2 — REFSEL settled by measurement, and the Eyelink problem dissolves

The board's default `voltage-ref = <1>` is the on-chip VREF regulator, which
**caps at 2.1 V** (`regulator-max-microvolt`) — so a 0–3.3 V eye feed genuinely
does not fit, and it is not a tuning knob. Sweeping the setting:

| `voltage-ref` | behaviour | full scale |
|---|---|---|
| 1 (board default) | saturates at DAC code ~2256 | 1.8 V |
| **0** | **no saturation, `adc_code == dac_code`** | **the DAC's** |
| 2 | identical to 0 | " |
| 3 | identical to 0 | " |

**A 12-bit DAC and a 12-bit ADC reading code-for-code** (1023→1026, 2047→2052,
3071→3076, 4095→4094) is the signature of the two sharing a reference. Chained
with the 1.8 V run — where a known ADC reference implied a 3.3 V DAC to ~1% —
that lands the full scale at ~3.3 V. Confirmed after the change:

    dac 1023 -> 829 mV   (824 expected)
    dac 2047 -> 1655 mV  (1650)
    dac 3071 -> 2480 mV  (2475)
    dac 4095 -> 3299 mV  (3300)

So **no divider, no OPAMP, no external parts** — one devicetree line. Channels
must be `ADC_REF_EXTERNAL0`; EXTERNAL1 makes the driver program the vref
regulator and use that instead.

**The trade is real and is recorded in the overlay:** 0.81 mV/LSB at 3.3 V
against 0.44 at 1.8 V, and VDDA is noisier than a bandgap. A low-range precision
channel should go back to EXTERNAL1.

**And a joystick needs none of this.** It is a potentiometer, so its span is
whatever supply drives it; power the pot from the reference and the measurement
is ratiometric, with reference drift cancelling. Worth knowing before anyone
builds a divider for one.

### Sampling, and one number that looked worse than it was

50 Hz, **1000 sweeps → 1000 blocks over 20 s, zero ongoing loss**,
`sweep_max_us` 114, `late` 0. A first reading showed `dropped=354` against 512
sweeps, which looks like a 69 % loss rate; it is **frozen** — a startup burst
while the box rebooted and the uplink was not yet draining. Sampling counters
have to be read as RATES; a single snapshot of a monotonic counter says nothing
about the steady state.

### FIRST TIMING NUMBERS on this silicon — DO->DI loopback

Wire: box pin **24 (Arduino D11) -> 26 (D12)**, adjacent on the digital header.
`pin 24 mode out`, `pin 26 mode in_pullup`, **`pin 26 debounce 0`** (a 25 ms
debounce would dominate every sample). The jumper doubles as an independent
check of the gpio0 pin map: `di/26` follows `do/24` exactly.

**Both harnesses were run as a pair, per the measurement audit above** —
`wiznet-io/host/loopback_rtt.sh` and `host/lb_split.sh` compute the same span by
different routes, and their disagreement is the only check on the apparatus.
They agreed to within 3 µs (957/975 vs 956/972), so these numbers are usable.
Driven from a Mac with `DSERV_HOST=raspberrypi.local` against the rig dserv;
~0.25 s per dservctl call, so ~90 s per 100-iteration run.

    median RTT ~975-987 us,  floor ~960-975 us,  p99 ~1470 us

Split into legs (analog on):

| leg | min | med | p90 |
|---|---|---|---|
| L1 `cmd -> do_echo` (dserv -> box -> GPIO -> echo) | 714 | **730** | 970 |
| L2 `do_echo -> di` (box sense -> publish) | 9 | **242** | 246 |
| total | 956 | **972** | 1210 |

**L1 is ~75 % of the loop** — the inbound leg dominates, as it did on the
Teensy. L2 at ~240 µs is notably larger than the 3-96 µs recorded for teensy41,
and its distribution is suspiciously tight (227-254 with analog off), which
looks like a fixed processing cost rather than a poll wait. Unexplained; a lead.

Against the audit table above (teensy41 eth 954/964, office W6300 676/654), the
MCXN947 sits with the Teensy at ~975 and both are ~1.4x the W6300 Pico. **Treat
that as indicative, not controlled** — those rows were measured against a
different dserv host and network.

### What the analog subsystem costs the RT path — and a claim I withdrew

Asked whether to disable the ADC for the loopback. Measured both instead. Five
runs, n=100 each:

| | min | med | p90 | p99 |
|---|---|---|---|---|
| analog ON  | 957, 969 | 975, 984 | **1210, 1042** | 1475, 1466 |
| analog OFF | 962, 975, 973 | 974, 987, 987 | **989, 1007, 1007** | 1462, 1471, 1473 |

**Median, floor and p99: no difference at all** — OFF is marginally *higher* at
the median in two of three runs. The 50 Hz sampler costs nothing where it counts.

**p90 is the one place it shows**, and only weakly: OFF clusters tightly
(989/1007/1007) while ON is both higher and scattered (1042/1210). So the honest
statement is *the analog sampler makes the tail higher and less repeatable by
roughly 50-200 µs*, consistent with 50 extra publishes/s contending for the same
service loop.

**A claim was withdrawn on the way to that.** The first ON/OFF pair showed p90
1210 vs 989, and it was reported as a ~220 µs analog cost. The repeat ON run
came back 1042 — the between-run spread was as large as the effect, on a
single run per condition. It took three OFF runs and two ON runs before the
comparison meant anything. **One run per condition is not a measurement**, which
is the same lesson as `dropped=354` earlier the same evening: read the
distribution, not the sample.

**Left unexplained: p99 is ~1466-1475 in ALL FIVE runs**, regardless of
condition. A stable ~1.5 ms stall hitting ~1 % of samples smells like a periodic
event (a 1 Hz publish, PTP, DHCP renewal), not like contention. Worth chasing —
it, not the analog subsystem, is what sets the jitter a rig would see.

### Context for every number above: this is a MODEST core, and it does not matter (yet)

David's point, and it is the right frame to read the loopback in. Verified from
the builds rather than from spec sheets where possible:

| board | core | clock | notes |
|---|---|---|---|
| **frdm_mcxn947** | Cortex-**M33**F | **150 MHz** | `ICACHE=y`, FPU, MPU; internal flash |
| teensy41 | Cortex-**M7** | **600 MHz** | superscalar, L1 caches — but XIPs external QSPI |
| RP2350 / Pico 2 | Cortex-M33 | 150 MHz | **the Pico firmware uses BOTH cores** |
| frdm_rw612 | Cortex-M33 | 260 MHz (spec) | |

**The MCXN947 is dual-core silicon and we use one core** —
`frdm_mcxn947_mcxn947_cpu0.dtsi:18` does `/delete-node/ cpu@1`. So against the
Pico 2 it is the *same core at the same clock*, and the difference is entirely
that the RP2350 build splits work across two cores (core 1 RT, core 0
console/I2C/flash) while this one does not. The second core is available if a
reason ever appears.

Against the Teensy it is ~4x less clock on a weaker microarchitecture —
realistically 5-8x less throughput.

**And yet the loopback matches the Teensy: ~975 µs against ~954 µs.** That is
the useful conclusion: **the round trip is not CPU-bound.** It is dominated by
the network path, which is exactly what the split says (L1 = 75 % of the loop).
A 4x clock deficit buying no measurable penalty is strong evidence the CPU is
not the constraint here. Note also that the Teensy's advantage is partly eaten
by its own memory system — it XIPs from external QSPI, which is why its net
stack had to be relocated to ITCM at all, while this part runs internal flash
behind an I-cache.

Where the modest core WILL matter, and should be watched: the ADC sweep costs
114 µs for 3 channels, so a 1 kHz 4-channel eye feed is a real fraction of the
budget, not a rounding error. And anything compute-heavy has ~1/6 the headroom
the Teensy would give.

**One difference that matters for the two-box scope work:**
`CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC` is **150 MHz here against 1 MHz on the
RW612**. The RW612's hard 1 µs floor on scheduler resolution — the thing that
capped `at_abs` simultaneity and starved its main loop when pushed — **is a
property of that board's OS timer and does NOT apply here**; the tick source has
6.67 ns resolution. The practical limit becomes tick-interrupt overhead instead
(a 1 µs tick is 150 cycles between interrupts, obviously unusable; the 10 µs in
use is 1500). So finer ticks are worth actually testing on this board before
assuming the RW612's ±37 µs skew floor transfers.

### ENET_QOS: what the "QoS" actually is, and why it is NOT our lever

David asked whether, with a proper partner, we could prioritise time-critical
traffic. Checked in the HAL and the driver rather than assumed:

* **The silicon has it, modestly.** `ENET_MTL_QUEUE_COUNT = 2` for the MCXN
  family (MCXA has 1) — two MTL queues and two DMA channels, with fixed or
  weighted-strict-priority arbitration, per-queue weights, and 802.1Q PCP →
  queue mapping (`rxqueuePrio` / `txqueuePrio`, and a `pcp:3` field in the VLAN
  API). **No Qbv time-aware shaper, no EST, no frame preemption** anywhere in
  `mcux-sdk-ng/drivers/mcx_enet/fsl_enet.h`. So this is **DCB-class priority
  queuing, not TSN** — prioritise a class, do not schedule a window.
* **Zephyr uses none of it.** `eth_nxp_enet_qos_mac.c` hardcodes `MTL_QUEUE[0]`
  and `DMA_CH[0]` throughout. Both queues exist; the driver drives one.
  Enabling this is driver work, not a Kconfig line.

**And the measurement says we would be building it for nothing.** Loopback under
three offered loads, same box, same wire:

| condition | frames/s | min | med | p99 |
|---|---|---|---|---|
| analog off (x3 runs) | 0 | 962-975 | 974-987 | 1462-1473 |
| analog 50 Hz, batch 1 (x2) | 50 | 957-969 | 975-984 | 1466-1475 |
| **analog 1 kHz, batch 8** | **125** | **982** | **1001** | **1485** |

1 kHz continuous streaming with maximum batching — the realistic shape, since we
would never send a sample per frame — costs **~15-25 µs of median and leaves p99
alone**. Verified as genuinely 1 kHz: +19,999 sweeps and +2,500 blocks over 20 s,
`throttled` 0, drops frozen. (Batch caps at 8 here: `AIN_BLOCK_MAX` is 24 int16
samples, 3 channels → 24/3. That gives 125 blocks/s, under the 200/s limiter.)

**The arithmetic that should have preceded the experiment:** a 128-byte dserv
frame is ~206 bytes on the wire with headers and IFG, so 125 frames/s is
**~0.2 Mbit/s — about 0.2 % of a 100 Mb link.** *The frame format caps the
problem.* One box streaming as hard as it practically can cannot congest its
own uplink; it would take several hundred boxes. There is no queuing contention
for a priority scheme to arbitrate.

**So QoS is not the lever on the box uplink.** It would only earn its keep when
(a) many boxes share a switch path to dserv, or (b) foreign traffic — video, file
transfer, imaging — shares that path. Both are switch-level problems, and both
are addressable by **marking** (VLAN PCP / IP DSCP via Zephyr traffic classes and
`net_pkt_set_priority`) with the existing single-queue driver, because the mark
rides in the frame and the MikroTik does the sorting. That is far cheaper than
driver surgery and is the thing to do first if the need ever appears.

**Ordering trap if it ever does:** two hardware TX queues behind ONE serialising
service loop buy nothing. Every publish is currently a blocking send on that
loop, so the box emits in submission order regardless of how many queues the MAC
has. "Move sends off the service loop (own TX queue + thread)", already listed as
pending above, is the prerequisite. Software first.

**What this strengthens instead:** p99 is now measured at 1462-1485 µs across
**three** offered loads spanning 0 to 125 frames/s. A tail that ignores a 2.5x
change in traffic is not contention. It looks periodic — a 1 Hz publish, PTP, a
DHCP renewal — and it, not the network, is what sets the jitter a rig would see.
**That is the thing worth chasing.**

### Chasing the p99: localised to the box's own long send, not dserv or the wire

Ruled OUT, each by measurement:

* **dserv's dispatch.** `host/dispatch_test.sh`: min 16, med 21, **p99 136 µs**.
  Two orders below the RTT tail.
* **The measuring host.** The harness stamps `cmd/do` when *dserv* processes it
  and takes arrival inside dserv's callback, so the span is dserv → box → dserv.
  Driving it from a Mac over the house LAN does not enter the measurement.
* **Network contention** — p99 invariant across 0 / 50 / 125 frames/s (above).

The box's own instrumentation, read live at 1 kHz streaming:

| stage | µs |
|---|---|
| `wake_us` RX thread saw readability → loop reached recv | 34 |
| `recv_us` the recv that returned the frame | 126 |
| `disp_us` framer + dispatch + GPIO + `state/do` publish | 113 |
| **box-internal inbound** | **~273** |

Against L1's 730 µs median that leaves **~457 µs off-box** in dserv's send path
plus transit — and PORTING.md recorded "~430 µs unaccounted" for teensy41, so
**this is the same structure, not something MCXN-specific.**

For the tail itself: `loop_us` 182 typical against `loop_max_us` **733**, and
`send_us` 141 against `send_max_us` **560**. That ~550 µs of occasional extra
loop time is the right magnitude for the ~495 µs p99 excess, and the send is
most of it. **Working conclusion: the tail is the box's own occasional long
send.** What makes a send 4x longer, and whether it is periodic, needs a TIME
SERIES — running maxima cannot show periodicity. That is where to pick this up.

### How many analog channels can this board actually do? (asked: 6)

Answer: **yes, and the blocker is neither pins nor conversion time — it is one
hardcoded constant in the shared CLI.**

* **Pins: not a constraint.** ADC0 reaches nine distinct pads on **port 4 alone**
  — P4_2, P4_3, P4_12, P4_13, P4_15, P4_16, P4_17, P4_19, P4_23 — all at **zero
  box-GPIO cost**, since box pins live on gpio0. (Of those, P4_15/19/23 are J8
  and P4_2/P4_3 are Arduino D1/D0; the header positions of P4_12/13/16/17 need
  the schematic.) The Arduino analog strip adds four more at the cost of box
  pins 14/22/15/23.
* **Software ceiling is 8, not 4**: `AIN_MAX_CH` 8, `BOX_ADC_MAX_CH` 8,
  `ain_group_chans[]` is a `uint8_t`, `CONFIG_LPADC_CHANNEL_COUNT` 15,
  `BOX_NAGROUPS` 4.
* **THE BLOCKER: `box_cli.h:292` masks with `~0x0Fu`** — four channels, an
  MCP3204 fossil baked into the grammar SHARED with the deployed RP2350 boxes.
  `ain group 0 channels 0,1,2,3,4,5` is refused with "ch 0-3" on a board whose
  converter has six declared and announces `ain/dbg/chans = 6`.
  **Do not just widen it to `~0xFFu`.** On a Pico with a real MCP3204 that would
  let someone configure channels the part does not have, and the group would
  then silently publish nothing — the exact failure shape this project keeps
  writing memos about. The honest fix is to validate against the FITTED channel
  count (`box_adc_channels()`), which means carrying it in `box_config_t` where
  the platform-agnostic CLI can see it. A design call, not a one-liner.
* **Conversion time is NOT the constraint.** Measured at 1 kHz base:
  `sweep_max_us` was **894 µs at 3 channels** and **573 µs at 4** — it went
  *down* with more channels, so the maximum is scheduling jitter (the ADC thread
  preempted by the service loop / net RX), not per-channel cost. `late` ran 17
  in ~26 000 sweeps (0.06 %), `throttled` 0.
* **The real throughput limit is block rate.** `AIN_BLOCK_MAX` is 24 int16, so
  batch caps at `24/nchan` — 8 scans at 3 channels, **4 at six**. At a 1 kHz base
  that is 250 blocks/s, over the `AIN_MAX_BLOCKS_PER_S` limiter of 200.
* **And the architecture already answers the actual use case.** Eyes at 1 kHz,
  pupil/heart-rate far slower: put them in SEPARATE GROUPS and decimate the slow
  one. Four groups exist. **Note the base rate applies to ALL channels** —
  groups decimate on the *publish* side only — so a 6-channel 1 kHz base pays
  the full 6-channel sweep every millisecond regardless of how slowly a group
  publishes. Given the numbers above, that is affordable.

### Known and unexplained: the DAC floors at ~517 mV

Codes 0 and 511 give an identical reading, reproducibly across runs and across
reference settings. **Not** the `buffered` flag — `dac_mcux_lpdac.c` never reads
that field — and `low-power-mode` is not set in the board dts. The bottom ~700
DAC codes are simply unusable. This is a property of the test SOURCE, not of the
ADC, and it did not affect either result above; it matters only if something
wants to synthesise a near-zero voltage on-board.

---

## 2026-07-29 — OTA end to end on the MCXN947, and one real defect

MCUboot + the full try-before-you-buy lifecycle, exercised on silicon. The board
had been running a non-MCUboot image linked at flash base; moving to the A/B
layout is a ONE-TIME SWD flash of both images (`west flash -d <sysbuild-dir>`
flashes MCUboot and the signed app), exactly parallel to the Pico needing one
BOOTSEL flash before OTA can take over.

Config needed adding to the board conf -- `CONFIG_STREAM_FLASH`,
`CONFIG_IMG_MANAGER`, `CONFIG_MCUBOOT_IMG_MANAGER`. Per the RW612's note,
MCUBOOT_IMG_MANAGER alone is **silently ignored** (it sits under the IMG_MANAGER
menuconfig, which needs STREAM_FLASH), so the build succeeds with every
arm/confirm path compiled out. Checked in `.config`, not assumed.

**First chainload was clean** — no analogue of the RW612's
MCUboot-powers-down-the-TDDR-PLL defect appeared:

    I: Bootloader chainload address offset: 0x14000
    I: Jumping to the first image slot
    image: v0.0.0+0, confirmed

### The lifecycle, both halves

Proven with two images distinguishable on sight: the running v0.0.0+0 and a
second build stamped `CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="0.2.0+7"`.

| step | result |
|---|---|
| stage 240076 B into slot1 (datapoint chunks) | `ok`, progress 100, sha verified |
| box-side cost | `erase_max 1286 us, prog_max 2329 us` |
| arm -> MCUboot swap -> trial | `fw_ver 0.2.0+7, trial 1, boot trial` |
| **reset WITHOUT confirm** | **reverts to 0.0.0+0, `reverts 1`, `boot revert`** |
| confirm | `trial 0`, **`updates 1`** |
| **reset AFTER confirm** | **stays 0.2.0+7, `boot software`** |

The box states the rollback in words, which is what OTA step 5 was for:

    image: ROLLED BACK from v0.2.0+7 -- the trial image was never confirmed
           (1 revert(s) lifetime)

**Config survived every swap** — 6 analog channels, 2 groups, pin map, dserv
target all intact, because `storage_partition` is outside both slots. That is a
property of the layout, and it held.

### THE DEFECT: after a REVERT boot, the dserv session went down and stayed down

The revert boot came up, published its announce (`fw_ver`, `boot=revert`,
`ota/reverts=1` all arrived), and then the session died. `show` on the box
console read `dserv.live=down uplink=eth cmds_rx=12` while the box itself was
perfectly healthy. **It did not self-heal in ~6 minutes**; an SWD reset restored
it immediately.

Every OTA attempted in that window failed with `result=host_io`, `progress=0` --
which is the honest symptom, since `cmd/ota/chunk` had nowhere to land.

Ruled OUT afterwards, on a freshly reset box: **the blast itself does not kill
the session.** A 240 KB push with the watchdog polled throughout ran to
`ok/progress 100` with `age_s 0` the whole way. So the trigger is something about
the revert path specifically, not chunk volume. Unresolved.

### A MEASUREMENT MISTAKE WORTH NOT REPEATING

While the box was off the air I read `state/ota/*` and reported `fail` /
`host_io` as if they were live. **They were RETAINED datapoints.** dserv never
deletes, so a dead box serves its last values forever and every query looks
healthy. Two hypotheses were built and reported on those stale reads before the
box's own console (`dserv.live=down`) and a frozen watchdog gave it away.

**Poll liveness, not the value.** `[now] - [dservTimestamp <dp>]` is one
expression and it would have caught this immediately:

    wd=41 age_ms=348271117    <- 348 SECONDS old, value unchanged

This is the same retained-datapoint property already recorded under "Open bugs"
(reboot divergence) and the `extio_clear` discussion, arriving through a third
door. Any harness that reads box state to make a decision should check freshness
first.

## 2026-07-29 (later) — chasing the revert-path drop: dserv leaks a thread per vanished box

David's observation is what cracked it open: during the failure `cmd/ota/begin`
ARRIVED and the chunks did not, while the box's CDC console stayed responsive.
That asymmetry is diagnostic, because the two directions are DIFFERENT SOCKETS:

* `dserv.live` reports `box_net_eth_connected()` — the box's OUTBOUND client
  socket to dserv:4620, the one every publish goes down.
* commands arrive on `srv_conn`, a connection **dserv** makes back to the box's
  listener on :5010.

So the box was not "off the network". It was **publish-dead and command-alive** —
which also explains the frozen watchdog, and why `state/ota/*` froze at whatever
it last managed to send.

### What is PROVEN

`ss -tn` on the dserv host, while the box was healthy:

    40 ESTABLISHED on 192.168.88.46:4620
    peers: 192.168.88.10 .11 .12 ... .34, .36, .43 x3, .49 .50 .51 .53 .54 .55 .56 .57

That near-contiguous run **is this box's reboot history for the day** — a new
DHCP lease every boot, and dserv still holding the socket from every previous
incarnation. Spot-checked five of them (.12 .20 .30 .53 .54): **every one is a
ghost, no ping reply.** dserv had **139 threads**.

The mechanism is in `Dataserver.cpp:1477`:

```c
new_socket_fd = accept(socket_fd, &client_address, &client_address_len);
...
std::thread thr(tcp_client_process, this, new_socket_fd);
thr.detach();          /* one detached thread per connection, no cap */
```

A box that disappears WITHOUT closing — reboot, reflash, power cut, crash —
leaves that thread blocked in `read()` on a socket whose peer will never answer.
There is no keepalive and no read timeout, so **nothing ever reaps it**. One
leaked thread + one leaked socket per box disappearance, permanently, for the
lifetime of the dserv process.

This is a HOST bug, not a box bug, and it is not OTA-specific. It matters beyond
this session: a rig box that reboots nightly adds a thread a day, and a
development session like this one added forty in an evening. It also explains
why the failure appeared LATE — it is cumulative, so nothing looks wrong until
it does.

### What is NOT yet established

**Whether that leak is what killed this box's publish socket.** It is the
obvious suspect and the timing fits, but the causal link is unproven, and this
same evening produced two confident wrong mechanisms already (a post-reboot
race, then the chunk blast — both measured and refuted). Stating it as the cause
now would be the third.

Two concrete leads for whoever picks this up:

* **`ss` showed `192.168.88.46:51898 -> 192.168.88.56:5010` with Send-Q 1280.**
  That is dserv's connect-back to a box that no longer exists, with data
  BACKED UP in it. If a dead peer's send queue can stall the forward path, that
  is a direct candidate for "begin got through, 3000 chunks did not".
* The box side needs a live reproduction with its own view captured: whether
  `sock` went to -1 and every reconnect failed, or a connect stayed permanently
  in flight (`connecting` never completing), which `box_net_eth_connected()`
  also reports as down.

### Fix directions (host side)

1. **TCP keepalive on accepted client sockets**, so a vanished peer is detected
   in minutes rather than never. Smallest change, biggest return.
2. A read timeout in `tcp_client_process` with the same effect.
3. Reap on write failure — dserv already writes to these sockets when fanning
   out datapoints; a failed write should retire the client rather than being
   ignored.

Worth doing before a rig runs unattended for weeks, independent of OTA.

### FIXED AND VERIFIED ON THE RIG, 2026-07-29

`src/socket_keepalive.h` + the three accept sites; David built and installed it
on raspberrypi.local and tagged a release.

**Keepalive is armed on accepted sockets** — `ss -tno` on a live box connection:

    ESTAB 0 0 192.168.88.46:4620 192.168.88.57:40397 timer:(keepalive,4.296sec,0)

**A vanished peer is now reaped.** Reset the box over SWD (it disappears with no
FIN and comes back on a new DHCP lease — the exact case that used to leak), and
watched the socket table:

| | ESTAB | FIN-WAIT-1 |
|---|---|---|
| before reset | 1 (the live box) | 39 |
| t+20 s | **2** — the ghost plus the box's new lease | 39 |
| t+40 s | 2 | **0** |
| **t+60 s** | **1 — ghost reaped** | 0 |
| t+80 s | 1, stable | 0 |

Reaped between t+40 and t+60, which is 30 s idle + 3 x 10 s probes. **Before the
fix that socket stayed ESTABLISHED for about two hours.**

**Threads 139 -> 97**, and 139 - 97 is almost exactly the 40 ghosts, so the
detached threads came back with the sockets. The 39 FIN-WAIT-1 were residue from
the OLD process (killed on restart, no peer left to ACK the FIN) and the kernel
cleared them on its own. Box healthy throughout.

Steady state is now 1 ESTAB + 1 LISTEN with one box connected, which is what it
should always have been.

**Still open:** the outbound direction — dserv's connect-back to a dead box was
seen with Send-Q 1280 backed up, and the fan-out path ignores write failures
rather than retiring the client. Different socket, different fix, and still the
most likely explanation for "begin got through, 3000 chunks did not".

### The outbound half — and a CORRECTION to what I said it was

I wrote twice that "the fan-out path ignores write failures rather than retiring
the client". **That is wrong.** `SendClient::send_dpoint()` sets `active = 0` on
a short or failed write in BOTH the binary and string paths, and
`SendTable::forward_dpoint()` reaps inactive clients under the same lock that
guards removal. The retirement machinery was already there and already correct.

Reading the code turned up two DIFFERENT defects, and the real one was better
hidden:

**1. `SO_KEEPALIVE` was already set on the outbound socket — bare.** There is even
a comment saying "detect a dead/rebooted peer even when the client is idle". But
a bare `SO_KEEPALIVE` inherits the kernel default of roughly **two hours**, which
is why a rebooted box's connect-back sat with Send-Q 1280 looking permanent. The
intent was right; the timing was never set. Now it calls the same
`dserv_set_keepalive()` as the accept side (30/10/3, ~60 s), after which the
EXISTING path does everything: write fails -> `active = 0` -> reaped.

**2. The socket is deliberately restored to BLOCKING mode after connect, with no
send timeout.** That is the more interesting hazard, because keepalive does not
cover it: a peer that is *wedged rather than dead* keeps answering keepalive
probes while never reading, so the send buffer fills and `write()` blocks that
client's thread **forever** — while `forward_dpoint()` keeps pushing onto its
`dpoint_queue`. Unbounded memory growth, no error raised anywhere, and the
client never retires because a blocked write never returns to report failure.

`SO_SNDTIMEO` of 5 s converts that into precisely the failure the code already
handles: a short write, `active` cleared, client reaped. Five seconds is far
outside any healthy consumer's stall on this rig, and a subscriber that has not
drained in five seconds is no longer useful to a realtime box.

**Lesson worth keeping:** the bug was not a missing mechanism, it was a
mechanism that could never fire. Both defects are of that shape — an option set
without its parameters, and an error path unreachable because the call blocks
before it can fail. Grepping for "does it handle X" finds the handler and stops;
it does not ask whether the handler is reachable.

### KEEPALIVE ONLY RUNS ON AN IDLE CONNECTION — the outbound socket needed more

Deployed the outbound fix and re-ran the vanish test. Both directions now arm
keepalive, confirmed in `ss -tno`. But the result was only half right:

| | before | t+25 s | t+75 s |
|---|---|---|---|
| ESTAB | 2 (in + out) | 4 (2 ghosts + 2 new) | **3** |

Three, where two is correct. Identifying the survivor is what taught the lesson:

    ESTAB 0 256 192.168.88.46:49276 -> 192.168.88.58:5010  timer:(on,4.520sec,13)
    ESTAB 0 0   192.168.88.46:37414 -> 192.168.88.59:5010  timer:(keepalive,11sec,0)
    ESTAB 0 0   192.168.88.46:4620  -> 192.168.88.59:47993 timer:(keepalive,7.640sec,0)

The **inbound** ghost reaped at ~60 s exactly as designed. The **outbound**
connect-back to the dead box did not — and its timer says why: `timer:(on,...,13)`
is the RETRANSMIT timer at attempt 13, not keepalive.

**Keepalive probes only run when a connection is IDLE.** The moment there is
unacknowledged data in flight, TCP switches to retransmission and keepalive never
fires; the connection is then governed by `tcp_retries2`, roughly 15-30 minutes.
And the outbound connect-back is *never* idle by construction — dserv is always
pushing datapoints to it, so it always has data queued (Send-Q 256 here).

`SO_SNDTIMEO` does not cover it either: 256 bytes never fills the send buffer,
so the write still succeeds and never blocks.

**`TCP_USER_TIMEOUT` is the option for exactly this** — it bounds how long
transmitted data may go unacknowledged before the connection is torn down, which
is the "peer went away mid-conversation" case rather than the "peer went away
while we were both quiet" case. Set to 20 s: far beyond any hiccup on a wired rig
LAN, far below the retransmit default. Verified applied on the Pi
(`TCP_USER_TIMEOUT = 20000`); Linux-only, compiles out cleanly on macOS.

**The generalisable bit:** "TCP keepalive detects dead peers" is only true for
connections that are idle. For a connection carrying data the mechanism is a
different timer entirely, and the two are easy to conflate because both present
as "the socket eventually goes away". The `ss -tno` timer column distinguishes
them at a glance — `keepalive` vs `on` — and that single column is what turned a
guess into a diagnosis here.

### VERIFIED ON THE RIG — both directions reap, and the revert drop is gone

With `TCP_USER_TIMEOUT` installed, the vanish test that half-worked before now
completes:

| | sockets |
|---|---|
| before reset | 2 ESTAB to .60 (in + out), keepalive armed |
| t+25 s | 4 — the two .60 ghosts (outbound at `timer:(on,492ms,6)`) + two new .61 |
| **t+50 s** | **2 — both ghosts reaped** |

The outbound cleared between t+25 and t+50, matching the 20 s user timeout;
previously it sat at retransmit attempt 13 with 15-30 minutes to run.

**The post-revert session drop does not reproduce.** Full cycle — push v0.3.0+1,
arm, trial boot, reboot WITHOUT confirming:

    t+030s  fw 0.2.0+7  boot revert  wd  18  age_s 0
    t+060s  fw 0.2.0+7  boot revert  wd  48  age_s 0
    t+150s  fw 0.2.0+7  boot revert  wd 138  age_s 0

Watchdog advancing at exactly 1 Hz for 150 s, `age_s 0` throughout. Before, the
box published its announce after a revert and went dark within a minute, staying
dark until an SWD reset.

Structurally: after roughly a dozen box reboots this session (.58 through .65)
the table holds exactly **2 ESTAB — one inbound, one outbound — and 96 threads**,
where the same pattern previously accumulated 40 sockets and 139 threads.

**Causality, stated honestly:** the failure no longer reproduces and the
cumulative condition that preceded it is gone, but that is not the same as
proving the leak caused it. The original defect appeared only after many
reboots, so a single clean run is weak evidence on its own; what carries more
weight is that the accumulation itself is now structurally impossible. If it
ever returns, the first thing to check is `ss -tno` for ghost sockets — and the
timer column, `keepalive` vs `on`, to tell which mechanism is meant to be
reaping them.

### OTA edge case found on the way: re-pushing the just-swapped image is REJECTED

Pushing the image MCUboot had *just swapped out* (slot1 already held it) staged
and verified fine — `ota ok, progress 100` — but the arm produced
`boot rejected`, `rejects 1`, and the box stayed on the running version.

`rejected` is a distinct state from `revert`, and the distinction is what made
this diagnosable: `box_boot.c` classifies **armed + ran + back on confirmed** as
a revert, and **armed + NEVER ran** as rejected. So MCUboot declined the image
rather than trialling and rolling it back.

Not downgrade prevention — `CONFIG_MCUBOOT_DOWNGRADE_PREVENTION` is not set.
The likely cause is inconsistent trailer/swap state in slot1 from re-writing the
image that was already there. Pushing a genuinely new version (v0.3.0+1) armed
and trialled immediately.

**Operationally:** do not test OTA by pushing the image the box just came from.
Bump the version. And `state/ota/rejects` is the counter that tells you MCUboot
refused, as against `reverts` which says it tried and rolled back.
