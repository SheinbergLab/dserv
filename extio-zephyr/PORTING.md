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

- [ ] **Manifest / self-description announce.** Nothing is published: `board`,
      `fw`, `build`, `boot`, `desc`, `channel`, `pins/in`, `pins/out`, `obs_pin`,
      `sync_pin`, `transport`, `dserv`, `ip`, `uptime_us`. This is how a host
      learns what a box *is*, and what drives extio-setup and the fleet page.
      Cheapest large win — no dependencies.
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
      **Not yet exercised:** a real multi-pin chord (two switches closing ms
      apart → one atomic bitmask). Needs a second output jumpered to a second
      input — the single-pin case cannot produce a multi-bit mask.
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
      **Not yet checked:** whether `teensy_loader_cli`/HalfKay full-chip-erases,
      which would wipe saved config on every REFLASH (surviving reboots — what
      was tested — is the functional requirement).
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
