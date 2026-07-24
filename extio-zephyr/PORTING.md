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

- [ ] **frdm_rw612 will very likely hit the SAME failure, with no easy fix.** Its
      `enet_ptp_clock` node carries no pinctrl, the board never mentions PTP, and
      **no `1588` pinmux entries exist anywhere in the RW612 dts or board
      pinctrl** — yet it is the same driver with the same unconditional
      `pinctrl_apply_state`. So there is nothing valid to point `pinctrl-0` at.
      Options, in order of preference: confirm from the RW612 reference manual
      whether 1588 event pads exist and add the pinmux entries; or carry a small
      patch making the default pinctrl state optional in the driver (the 1588
      event pins are genuinely optional hardware) — `wiznet-io/patches/` is the
      precedent for a locally-applied Zephyr patch; or upstream that fix. Settle
      this early — PTP is a headline reason the RW612 was chosen.
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
**BISECTED: it is `CONFIG_SHELL`, and it is NOT any of the obvious levers.**
Same board, same harness, n=300 each:

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
