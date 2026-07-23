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

## PTP — a specific lead, not a mystery

`ptp_ready=0` is almost certainly **not** a silicon or clock-source problem. The
driver's init has exactly one failure path:

```c
ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
if (ret) return ret;          /* -> device_is_ready() == false */
```

Our overlay enables the node with a bare `status = "okay"` and no pinctrl, so the
default state does not exist and init fails. The **RT1060 EVK — same SoC family
as Teensy 4.1 —** supplies one:

```dts
&enet_ptp_clock {
	status = "okay";
	pinctrl-0 = <&pinmux_ptp>;      /* GPIO_AD_B1_02 1588_event2_out */
	pinctrl-names = "default";      /* GPIO_AD_B1_03 1588_event2_in  */
};
```

- [ ] Add a `pinmux_ptp` group to `boards/teensy41.overlay`. **Unverified:**
      whether those two pads are broken out and free on a Teensy 4.1 — check
      before assuming. The 1588 *event* pins are only needed for external
      trigger/capture, but the driver applies pinctrl unconditionally, so some
      valid state must exist.
- [ ] `CONFIG_PTP_CLOCK_SHELL=y` (from `samples/net/ptp`) adds shell commands to
      read/adjust the clock directly — a real diagnostic now that the console
      works.
- [ ] `CONFIG_PTP=y` is the full PTP protocol stack. Not needed for the local
      free-running clock; needed for actual sync against the i.MX95 partner.

Keep the distinction that matters: a local free-running clock needs no peer and
*should* tick; sync/discipline needs a grandmaster.

---

## Tier 1 — box functionality rigs depend on

- [ ] **Manifest / self-description announce.** Nothing is published: `board`,
      `fw`, `build`, `boot`, `desc`, `channel`, `pins/in`, `pins/out`, `obs_pin`,
      `sync_pin`, `transport`, `dserv`, `ip`, `uptime_us`. This is how a host
      learns what a box *is*, and what drives extio-setup and the fleet page.
      Cheapest large win — no dependencies.
- [ ] **DI groups (chords)** — `src/core/box_group.h`, zero callers. Per-group
      settle/quiet, onset-stamped edges, announced pin lists. This is the
      joystick/button API behind `ess-2.0.tm joystick_dir`, so it is a real
      functional gap for experiments.
- [ ] **Clock sync** — `src/core/box_clock.h`, zero callers. All six `sync/*`
      keys missing (`box_us`, `dserv_us`, `offset_us`, `rate_ppb`, `source`,
      `transport_us`). This is the sub-ms obs timeline validated on the W6300,
      and it gates the next two items.
- [ ] **Scheduled events** — `box_gpio_exec` handles only `GPIO_OP_SET` and
      `GPIO_OP_PULSE`. `GPIO_OP_SCHED_PULSE` and `GPIO_OP_SCHED_TIMER` (fire at
      beginobs + N µs) are parsed by the core and then silently dropped. Needs
      clock sync for the beginobs epoch.
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
- [ ] **Persistence.** Written (`box_flash.h`, NVS + SD-FAT backends) but
      validated on **no** board: both Teensy backends hard-fault at boot (XIP
      FlexSPI NOR re-init; usdhc/SD stack). RW612 NVS is the mature path.
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
