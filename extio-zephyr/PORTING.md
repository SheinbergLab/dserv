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

## Tier 2 — peripherals

- [ ] **Analog: MCP3204 + analog groups** — `src/core/box_ain_group.h`, zero
      callers; no platform driver. Block #7. On RW612 this also decides on-chip
      GAU GPADC vs the external MCP3204 (BENCH_NXP D10 — watch RF coupling into
      on-chip analog on a tri-radio part).
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
