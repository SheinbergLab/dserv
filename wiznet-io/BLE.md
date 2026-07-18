# BLE.md — BLE handheld (wireless response device) design notes

**Status: receiver radio scaffolding BENCH-VALIDATED 2026-07-17 on a real
Pico 2 W** (`sh build.sh pico2wusb`): `ble enable 1` → staged bring-up in
~925ms ([1/4] cyw43_arch_init ~47ms, [2/4] btstack ~1ms, BT fw upload inside
the first polls ~880ms), active scan inventoried a full 24-device table.
2026-07-16 bench crashes (watchdog death during bring-up ×3) closed as
ENVIRONMENTAL — see the incident note in the implementation notes. **GATT
frame pipe BENCH-VALIDATED END-TO-END 2026-07-17**: Thing Plus handheld →
BLE → Pico 2 W receiver → USB → dserv, auto-discovered as extio/hh1 with
battery/watchdog/manifest live (mtu=255) — **including DI events: a grounded
GP26 (A0) publishes extio/hh1/state/di/26 into dserv over the radio. Phase 1
bench goals COMPLETE.** See "The frame pipe" section for the launch-day
fixes (the `#ifndef PICO_*` flash-save bug, the host-side tty grab, and the
16MB ATRANS aperture bug, all resolved). **Thing Plus handheld PROVISIONED
A/B + full docked TBYB update cycle PROVEN 2026-07-17** — after root-causing
a slot-boot-only `cyw43_arch_init` wedge to the SDK's btstack TLV bank reading
flash through the ATRANS-translated XIP window (the persist bug's sibling, one
library deeper; fixed via wiznet-io/patches/, auto-applied by build.sh) — see
"Provisioning the Thing Plus + the slot-boot radio wedge". Supersedes the 2026-07-09 receiver-topology discussion: the
UART-sidecar-into-the-W6300-box plan (including its mounting design) is DEMOTED
to a contingency — see the appendix, which preserves that work. Companion docs:
OTA.md (update machinery), PINMAP.md (pin budget), TESTING.md (harnesses,
obs_soak / sync_analyze methodology reused here).

## Goal

A battery handheld (buttons / joystick, later levers etc.) whose events land in
dserv as ordinary extio datapoints — same manifest/labels/groups, same ess
joystick/response API, same fleet page — with **ms-accurate timestamps** despite
a radio hop that delivers events 10–50ms late.

## Non-goals (the boundary that shapes everything)

BLE delivery is quantized by the connection interval: at a 15ms interval expect
~10–20ms median delivery with 30–50ms tails under 2.4GHz congestion. Anything
you'd accept over BLE is therefore *by definition* latency-tolerant. This sorts
the rigs for us:

- **Ethernet (W6300) rigs = time-critical collection (neurophys, eye movement):
  stay wired end-to-end.** No BLE support there, ever, by policy — which is also
  why no radio code enters the `dual`/`w6300` images (no radio chip on the EVB,
  and its 2MB QSPI gives ~1MB A/B slots that several hundred KB of radio
  blobs would eat).
- **USB behavioral rigs: the BLE home.** Source stamps give accurate
  *measurement*; closed-loop *reactions* to handheld input pay radio latency
  (accepted trade, same line as the RT-delegation boundary: never gate a sub-ms
  contingency on a radio input).

## Hardware

| Role | Board | Why |
|---|---|---|
| Receiver / rig box | **Pico 2 W** | Same CYW43439 radio as the Thing Plus (via RM2) at ~1/3 cost; battery HW would be dead weight on a box-powered board. |
| Handheld | **SparkFun Thing Plus RP2350** | RM2 radio + LiPo charge + MAX17048 fuel gauge (BOX_FUEL_MAX17048 path already publishes battery %) + USB-C. Already a build.sh target. |
| Bench test transmitter (Phase 1) | second **Pico 2 W** | Same radio/stack as the real handheld, but USB-wired: powered, full console while frames flow over the radio, and instrumentable with a ground-truth GPIO wire. |

Both ends share one cyw43/btstack platform layer (RM2 == the Pico 2 W chip) —
the strongest argument for this lineup and against fracturing to another chip
family. btstack starting point: pico-examples LE streamer client/server.

## Topology: single-board receiver, handheld as a named box

The receiver is **not a new device class**. It is the standard USB box firmware
built for Pico 2 W, plus a BLE-central relay. One board serves the rig's wired
channels AND ferries the handheld — which appears as a **second named extio
box** (`extio/hh1/...`) multiplexed over the same USB CDC data pipe.

This works because the plumbing already allows it:

- `dserv_msg` frames carry the datapoint *name*; box identity lives in the name
  (`extio/<name>/...`), not in the transport. Multiple logical boxes over one
  pipe is already legal.
- extioconf auto-discovery wires boxes **per-name** from `extio/<name>/state/*`
  telemetry, and `extio_forward_box` wires `config/*` + `cmd/*` forwards
  per-name down the (single) data port. A relayed handheld is discovered and
  addressed with **zero host-side changes**.
- The receiver enumerates with the same `extio…if02` USB identity
  (usb_descriptors shared), so `extio_find_data_port` needs no change either.

Relay rules (receiver):

- **Upstream**: frames arriving over BLE are relayed onto the box's transport
  after timestamp rewrite (below). The handheld's own telemetry
  (`state/watchdog`, `state/fw`, battery) flows through, so host stale-detection
  and the fleet page just work.
- **Downstream**: the receiver keeps a table of names announced by bonded
  peripherals; inbound frames addressed to those names route over BLE,
  everything else is handled locally as today.
- **Link state**: on BLE disconnect, receiver publishes `extio/<hh>/state/link 0`
  (and stops relaying); the host's existing watchdog-stale logic handles
  disappearance. On (re)connect the handheld announces its manifest exactly as a
  wired box does on transport-up, and extioconf re-forwards config — same
  contract, no new cases.

Why this beats the 2026-07-09 sidecar commitment: the sidecar solved "radio far
from the subject on an Ethernet rig," a topology we've now decided doesn't occur
(Ethernet rigs are wired-only by policy). Single-board also **shortens the sync
chain by one hop** — handheld→box is the only radio boundary; box→dserv is the
already-validated machinery.

## Build policy: compiled per-board, enabled at runtime

Neither "BLE in every USB image" nor a with/without-BLE build axis. **The board
axis we already have is the BLE axis**: radio-capable boards get BLE compiled in
unconditionally; radio-less boards never do; behavior is a persisted runtime
flag.

- `BOX_BLE` is implied by radio-board targets in build.sh — not a user-facing
  flag. As built 2026-07-16 it's on the new **`pico2wusb`** target (BOARD=
  pico2_w, VARIANT=usb: the usb box + BLE central = the receiver). Extending it
  to the WiFi `pico2w`-family targets is a one-line CMake addition once
  WiFi+BLE coexistence has actually been exercised — don't flip it blind.
- Why not a universal binary with runtime probing: (1) on a plain Pico 2 the
  pins the CYW43 driver would touch mean other things (GP23 SMPS mode, GP24
  VBUS sense, GP25 LED, GP29 VSYS ADC) — probing is not benign, which is why
  PICO_BOARD is compile-time; (2) we already learned this lesson when boot-time
  PHY auto-detect was dropped (PHYSR unreliable at cold boot, 2026-07-05);
  (3) flash: cyw43 WiFi+BT blobs + btstack ≈ several hundred KB, landing in
  *both* A/B slots.
- Runtime: persisted **`ble 0|1`, default OFF** (a lab of boxes advertising and
  scanning by default is RF noise and a pairing-surprise surface; rigs opt in
  when a handheld is bonded). Bonding allowlist lives in the same persist
  record. Precedents: `mode` (dual), `oled_en`, fuel-gauge runtime probe.
- **Fail-soft init**: if `cyw43_arch_init` fails, log and continue as a plain
  USB box — a bad flash or hardware fault degrades to wired I/O instead of
  taking down a rig. (The shelf already keys compatibility on BOARD, so OTA
  can't push a radio image to a radio-less board in the first place.)
- Unlike the WiFi target (SSID/password baked at build), BLE bakes nothing —
  bonding is runtime state. Much nicer for the fleet.
- New build target: **`handheld`** = BOARD=sparkfun_thingplus_rp2350,
  VARIANT=handheld (different personality: BLE-peripheral transport + battery,
  no relay). Bench variant of the same app: BOARD=pico2_w. BOARD stays the
  shelf's honest compatibility key.

## Radio protocol: a dumb frame pipe

The unit end-to-end stays the **128-byte dserv_msg frame**. The radio is just
another pipe carrying them; nothing upstream can tell.

- GATT service, NUS-shaped but under our own 128-bit UUIDs (nothing else should
  ever connect): one notify characteristic (handheld→receiver events/telemetry),
  one write-without-response characteristic (receiver→handheld cmds/config).
- Negotiate ATT_MTU ≥ 247 (DLE) so one notification = one frame. **Keep the
  0x00-resync framer on both ends anyway**: at the default MTU 23 frames
  fragment, and the framer makes that a non-event instead of a failure mode.
- Echo-sync runs **in-band** as ordinary frames (a small cmd/reply pair), so
  sync samples traverse exactly the queues and path the event frames do.
- Multiframe `'D'` (OTA etc.) is **excluded over radio in v1** — see Power/OTA.
- Connection parameters: request a 15ms interval (7.5ms is the spec floor;
  whether cyw43/btstack sustains lower is a Phase-1 measurement, not a promise),
  peripheral latency so the handheld skips intervals when idle, supervision
  timeout a few seconds.

## Time: stamp at source, one clock boundary, rewrite once

- Button/joystick edges are stamped at GPIO IRQ on the **handheld's** µs clock;
  chords/debounce use the existing `pico_group.h` onset-stamped settle machine
  on the handheld itself.
- The radio hop is the **only new clock boundary**. The receiver maintains
  offset+rate for each bonded handheld via **minimum-RTT-filtered echo sync**
  (BLE RTTs are conn-interval-quantized, so NTP-style min filtering, not
  averaging) feeding the same adaptive-EMA rate discipline validated in
  `pico_clock`. Expect ~ms-class mapping; the wander lesson from the hw-sync
  work says keep the rate estimate adaptive.
- The receiver **rewrites the ts field in place** (handheld clock → receiver
  clock) before relaying. Downstream, the validated box→dserv sync applies
  unchanged. Translate exactly once, at the radio boundary.
- Consequence consumers must tolerate: events arrive stamped up to ~50ms in the
  past (same pattern as Tier-B DI-on-obs-timeline publishes, larger skew).
  Extractors and obs-boundary logic should be checked against a late-stamped
  event straddling an obs edge.

Honest expectations, to be replaced by Phase-1 measurements:

| Quantity | Expectation |
|---|---|
| Event timestamp accuracy (on host timeline) | ~1ms after echo-sync convergence |
| Delivery latency, median | 10–20ms @ 15ms conn interval |
| Delivery latency, tail | 30–50ms under 2.4GHz congestion |
| Reconnect after supervision loss | ~0.1–1s (why we never disconnect-idle) |

## Power (handheld)

- Stay **connected with peripheral latency** (skip N intervals when idle); never
  advertise-on-press — 100–300ms reconnect latency would poison first-response
  RTs. RP2350 sleeps between events, wake on GPIO.
- Ballpark: low-single-digit mA average → order-of-a-week on a 500mAh pack;
  measure precisely once running, since MAX17048 % already flows to telemetry →
  battery tile on the fleet page + low-battery warning.
- **Handheld firmware updates = USB-C when docked** (picotool / the A/B updater
  over CDC — it's in your hand, and it charges there anyway). No OTA-over-BLE
  in v1. The receiver, being a standard box image, updates via the existing
  shelf/OTA path like any other box.

## Pairing / security

- LE Secure Connections, Just Works (no display on either end), gated by a
  **CLI pairing window** on the receiver (`ble pair 60`), result persisted to a
  bonded-address allowlist. Outside the window, the receiver connects only to
  allowlisted addresses — multi-rig rooms don't cross-talk.
- Handheld gets a name via the existing desc persist; it announces under that
  name (`extio/<desc>/...`), so two handhelds in one room are distinct boxes.

### Bonding — DONE + VERIFIED 2026-07-18 (Inc1 + Inc2, fw bond1/bond2)

The btstack infra was already configured (LE Secure Connections + micro-ecc,
16-entry whitelist + le_device_db, TLV bond store via btstack_cyw43_init); the
work was the SM policy + pairing flow + allowlist gating.

- **Inc1 — pair + encrypt (both ends):** `sm_set_io_capabilities(NO_INPUT_NO_OUTPUT)`
  + `sm_set_authentication_requirements(BONDING | SECURE_CONNECTION)`; an SM event
  handler auto-confirms JUST_WORKS_REQUEST and logs PAIRING_COMPLETE /
  REENCRYPTION. The central triggers it — `sm_request_pairing(pipe_con)` on
  connect (first time bonds; thereafter btstack re-encrypts from the stored LTK).
  VERIFIED: `bonds=1 enc=yes`; pipe + echo-sync survive encryption (sub-ms:
  +0.59 ms median vs +0.37 ms unencrypted — the ~3 ms RTT-floor bump from the
  MIC is absorbed by the min-RTT filter); **bond persists across reboot** (the
  handheld reboots and *re-encrypts from the persisted bond*, not a fresh pair)
  — which also clears any doubt that the ATRANS-patched TLV bank round-trips
  bond WRITES, not just the reads we fixed for the slot-boot wedge.
- **Inc2 — allowlist + window (receiver-only):** the adv-report handler connects
  ONLY to `addr_is_bonded()` addresses (le_device_db compare; the handheld's
  stable locally-administered address = its identity address, so no RPA/IRK
  resolution) OR while a `ble pair <secs>` window is open. `ble forget` clears
  the db (on core 0 via the request queue — never cross-core) + drops the link.
  Status: `bonds=N (pairing window OPEN)`. VERIFIED all four paths on the one
  pair: bonded auto-reconnect (no window, survives both reboots) · **rejection**
  (`ble forget` → receiver refuses to reconnect, 0 relay = the isolation
  guarantee) · **re-pair** (`ble pair 60` → adopts the advertiser as NEW, bonds,
  relay resumes).
- **Design choice (per this doc):** the window is receiver-only; the handheld
  always accepts pairing (only the receiver initiates, only during its window).
  A handheld-side gate (button-hold at boot) is deferrable — mild privacy
  surface only. Flash note: a new-receiver + old-handheld (or vice-versa)
  mismatched-SM-config window BREAKS the pipe (pairing fails); flash the pair
  together, and if the radio bootsel path is down, bootsel the handheld via its
  USB console directly.
- **Inc3 (telemetry) — DONE + VERIFIED (fw bond3/bond3b):** the receiver
  publishes `state/ble/{bonds,encrypted,pairing}` on change (bonds via a core-0
  volatile mirror since le_device_db is core-0 only; pairing = window seconds
  left) + a `ble bonds` console list. **Dropped-publish bug found + fixed:** the
  on-change publish advanced its cache even when box_net_client_send dropped the
  frame during the boot/reconnect tty race, sticking dserv at the stale value
  (state/ble/encrypted read 0 while the link was encrypted). Same class as the
  OTA dropped-single-send; on-change state can't self-heal like the ~3/s echo
  telemetry. Fix: gate the publish + cache-advance on box_net_client_reading()
  (retry, don't record-as-sent) + reset the cache on the up==2 connect burst
  (re-sync after reconnect/dserv-restart). Verified across two reboot cycles:
  encrypted datapoint now tracks the link. GOTCHA found the hard way:
  `cmd/bootsel` enters BOOTSEL on ANY write regardless of value -- `bootsel 0`
  is NOT a no-op (dserv_cfg__cmd returns CFG_BOOTSEL without checking the value).
- **Still open (minor):** remoteable `ble pair`/`ble forget` via cmd/ble/*
  datapoints (console-only today); the peripheral-latency power tradeoff.

## Firmware layout

- `pico/box_ble_central.h` — receiver-side: scan/connect/bond, name table,
  frame relay, echo-sync master, ts rewrite. Lives on **core 0** (console /
  I2C / flash core) with events handed to core 1 through the existing queue
  machinery; the RT core is untouched.
- `pico/box_net_ble_impl.c` — handheld-side transport: the same box app, with
  BLE-peripheral as its `box_net` backend (the vtable seam was built for this).
  On the bench TX, USB console CDC stays alive alongside — full observability
  while data flows over radio.
- Known gotcha to leave a comment on, not "fix": flash persist writes stall
  core 0 briefly, delaying radio servicing. Harmless — BLE supervision timeouts
  are seconds and the CYW43 buffers — but someone will be tempted to move the
  radio off core 0 over it.
- WiFi+BLE coexistence on the CYW43439 is chip-supported; not exercised in
  Phase 1 (receiver transport is USB). Note it so the pico2w WiFi target and
  BOX_BLE don't get declared mutually exclusive by accident.

## Host side

Nothing required. Same USB identity → `extio_find_data_port` unchanged;
per-name auto-discovery + config/cmd forwarding already generalize; label
algebra and the ess joystick/response API consume the handheld's manifest like
any box's. (macOS single-port heuristic in extioconf is unchanged and
unaffected — still one data port per rig.)

## Implementation notes — receiver scaffold (built 2026-07-16)

What exists: `pico/box_ble_central.h` (core-0 radio: lazy fail-soft bring-up,
scan/inventory, phylink-style request word from core 1), `pico/btstack_config.h`
(LE-only, the pico-examples cyw43 flow-control set), persisted `ble_en`
(persist **v15**, CLI `ble enable 0|1` + `ble` / `ble scan 1|0`, datapoint
`config/ble/enable`), and the `pico2wusb` target in CMake/build.sh. usb, dual,
and sim all still build against the shared-header changes.

Findings a future reader should not re-derive:

- **Poll arch is load-bearing, not preference**: `pico_cyw43_arch_none` pins
  threadsafe_background, whose callbacks run in IRQ context on core 0 — and a
  printf there corrupts the SPSC log rings (box_console.h forbids IRQ prints).
  `pico_cyw43_arch_poll` + `CYW43_LWIP=0` runs btstack callbacks inside
  `cyw43_arch_poll()` on core 0's mainline. Don't "simplify" to `_none`.
- **Core 0 is the watchdog petter — radio bring-up hardening.** The bring-up
  is stage-announced ([1/4]..[4/4]) with the log ring DRAINED before each
  stage (a death names its stage on the console), the watchdog is re-armed to
  8s for the phase (restored at UP/FAIL), each bring-up poll is followed by a
  pet, core 1 is parked for [1/4]-[2/4] (radio init owns the machine solo),
  `PICO_STACK_SIZE=0x1000` (2K default is thin for cyw43+btstack call depth),
  and a boot-loop guard: after a watchdog boot the persisted auto-bring-up is
  skipped once with a console note; re-typing `ble enable 1` retries.
  MEASURED healthy bring-up (2026-07-17): [1/4] ~47ms, [2/4] ~1ms (no TLV
  flash format on first init — it's lazy), BT fw upload ~880ms inside the
  first polls, up in ~925ms total — nowhere near either window.
- **2026-07-16 incident: three watchdog deaths during bring-up, closed as
  ENVIRONMENTAL 2026-07-17.** `ble enable 1` on the first bench board killed
  the box inside [1/4]-[2/4] three times (2s, then 8s window, then with the
  park). Next day, after the J-Link was wired (cables/board reseated), the
  IDENTICAL build + sequence came up clean in ~925ms — and so did every
  software permutation under the debugger: boot-forced init, runtime enable,
  watchdog disabled, watchdog re-armed live via gdb mid-bring-up. Every
  software variable was eliminated by experiment; the signature (hard wedge in
  the radio's power-on/fw-upload phase, cured by reseating) points at a
  marginal supply during the CYW43 bring-up inrush. If it recurs: death
  between [1/4] and [2/4]-done is the tell; try reseating/another cable/port
  first, then the BLE_DEBUG builds below.
- **J-Link debug builds + hard-won RP2350 J-Link lessons.**
  `BLE_DEBUG=1 sh build.sh pico2wusb` → `_bledbg`: watchdog DISABLED +
  bring-up forced at boot (a wedge = stable hang to attach to).
  `BLE_DEBUG=2` → `_bledbg2`: watchdog disabled, bring-up on typed enable
  (the runtime-context reproduction). Both get their own build dir + dist
  name (cmake cache is sticky). Lessons (also for reference_jlink notes):
  (1) **flashing over J-Link leaves the RP2350 XIP cache stale** — the old
  image's hot loop keeps executing from cache over new flash, and J-Link's
  `monitor reset` is core-local, so nothing reconciles; symptoms are surreal
  (gdb disassembles NEW code while the CPU runs OLD bytes — we watched a
  true `bne` fall through). Always BOOTSEL-flash or fully power-cycle after a
  J-Link `load`. (2) `0xdeadbeef` register reads = "CPU is running", not a
  broken wire — `monitor halt` first. (3) A full chip reboot from gdb: write
  ENABLE|TRIGGER (bits 30|31) to WATCHDOG_CTRL @0x400d8000. (4) The
  breakpoint-inject trap works: bp inside bring-up, poke the watchdog back ON
  from gdb (LOAD @0x400d8004 then CTRL|=bit30), detach, re-halt — PAUSE_DBG
  freezes the countdown while halted, so a caught wedge can be inspected
  indefinitely.
- In this SDK the arch does NOT auto-init btstack: the app calls
  `btstack_cyw43_init(cyw43_arch_async_context())` explicitly (memory + run
  loop + HCI transport + TLV).
- **btstack TLV bond storage** (set up at init even before bonding) uses the
  RP2350 default `PICO_FLASH_BANK_STORAGE_OFFSET` = top-of-flash −12K → banks
  at −12K/−8K. Persist is the last 4K; A/B slots are 512K each near the bottom
  (pt.json). Nothing overlaps — and do NOT move the offset to −8K, which puts
  TLV bank 1 exactly on the persist sector.
- btstack_config.h gotchas: `ENABLE_BLE` comes from the SDK's pico_btstack_ble
  interface (defining it again warns in every TU); the SDK unconditionally
  compiles `hci_dump_embedded_stdout.c`, which `#error`s without
  `ENABLE_PRINTF_HEXDUMP`.
- **`flash FAIL` on every radio build (found via the first thingplus save,
  2026-07-17): the `#ifndef` vs defined-to-0 trap.** rt_main's lockout-victim
  registration was guarded by `#ifndef PICO_FLASH_ASSUME_CORE1_SAFE` — but the
  SDK's pico/flash.h defines that macro TO 0 as a PICO_CONFIG default, so the
  guard was always false and core 1 never registered → every `save` on an XIP
  build returned NOT_PERMITTED (-4). Latent since the 2026-07-07 Stage-2
  copy_to_ram conversion (which retired the lockout path on all then-shipping
  targets — the bug was born the day it became unobservable); the radio builds
  were the first XIP images since. Fix: `#if !PICO_FLASH_ASSUME_CORE1_SAFE`.
  Rule for this codebase: **never `#ifndef` a `PICO_*` config macro — the SDK
  pre-defines them to 0; use `#if !`.** (`save` now prints the rc + victim
  state on failure, and `show` reports `fse=`, the boot-time save diag.)
- **Persist on 16MB parts (found via the Thing Plus, 2026-07-17): the QMI
  ATRANS aperture must not cross the 4096-sector translation boundary.** The
  slot-safe persist load mapped a 2-SECTOR window at the persist sector —
  which is the LAST sector, so on a 16MB part BASE=4095 + SIZE=2 > 4096
  invalidated the aperture and every read returned 0xFF ("config: none/
  invalid") while the physical sector held a perfect blob (picotool proved
  it). 4MB parts (BASE=1023) never came close, which is why the pico2w
  receiver was immune. Fix: 1-sector aperture (the blob is <=1K; the last
  sector can never overrun). Compounding lesson: the boot-time flash-save
  "diag" REMOVED the same day — it re-saved g_cfg every boot, so any failed
  load overwrote the good blob with defaults, destroying data AND evidence.
  Never add a boot-time flash WRITE probe; `show`'s `persist=` line now
  reports what the loader saw (magic/ver/len) instead.
- **Image size**: ~473K flashed (WiFi+BT firmware blobs) — fits the current
  512K A/B slots with ~40K to spare. A receiver that grows past that needs a
  bigger-slot pt.json at provisioning time (4MB flash has plenty of room).
- copy_to_ram stays OFF for pico2wusb (image + blobs exceed SRAM), so flash
  saves park core 1 via the lockout — the pre-Stage-2 behavior, fine here.

Bench smoke test (real Pico 2 W on USB) — do this FLAT first (probe-flash-first:
don't debut the radio and the slot-boot path on the same boot):

```
picotool load wiznet-io/dist/wizchip_dserv_config_pico2wusb.uf2   # via BOOTSEL
# console CDC0:
ble enable 1        # staged bring-up prints [1/4]..[4/4] with timings (a few seconds;
                    # watchdog auto-widened to 8s for the phase, restored at [4/4])
ble                 # -> "ble: state=up en=1 addr=xx:xx:.."
ble scan 1          # nearby advertisers print once each; `ble` lists them
save                # persist ble_en (v15; older configs load fine, ble_en=0)
```

Then provision the bench board (worth it early: the dev loop becomes OTA push
instead of BOOTSEL cycling, and it validates XIP-from-slot at this image size +
TLV-under-PT + a full OTA round with the radio up). The pico2_w gets its OWN
partition spec, **pt-pico2w.json** (1024K slots on the 4MB part) — the EVB's
pt.json 512K slots would be 92% full with this image on day one. No pico2_w had
been provisioned before this; treat the first one as its own small validation
(check state/ota/boot + bootdiag, persist survival, then `ble enable 1`).

```
sh build.sh pico2wusb --hash        # slot-bootable base -> dist/..._pico2wusb_hashed.uf2 (built 2026-07-16)
PT_JSON=$PWD/pt-pico2w.json ./provision.sh dist/wizchip_dserv_config_pico2wusb_hashed.uf2
# slot B left empty; the first OTA fills it (build.sh pico2wusb --tbyb --push)
```

Note: dserv-agent's /extio/setup embeds the EVB pt.json; if shelf-provisioned
receivers become a thing, the remote flow needs a per-build pt choice.

## Provisioning the Thing Plus + the slot-boot radio wedge (2026-07-17, DONE)

**Status: hh1 (Thing Plus, 16MB) is provisioned A/B and the entire docked
update cycle is rig-proven end-to-end:** `cmd/bootsel` over the radio →
`picotool load <tbyb_hashed>.uf2 -x` → FLASH_UPDATE trial boot of the inactive
slot → radio up → 8s self-test (RT beat + BLE link) → `rom_explicit_buy` →
committed → survives plain reboots (bootrom keeps preferring the bought slot).
Persist survived the PT write, both slot boots, and the trial. **The receiver
(`pico`) was provisioned the same day** — same recipe, `dservctl set
extio/pico/cmd/bootsel 1` + pt-pico2w.json — and additionally proved the
**NATIVE OTA round end-to-end**: `extio_ota_push pico <tbyb .bin>` → 'D'-frame
stream → verify → arm → FLASH_UPDATE trial boot → 8s self-test → buy →
`state/ota/state=committed` in dserv, with the radio up and the hh1 pipe
streaming throughout. (En route that flushed out the scratch[2]/[3] arm
hazard — see the scratch-register HARD RULE below.) Both boards now run
committed fixed-crumb images from their slots. **`ble pipe` persists as
`pipe_en` (persist v16, same day):** `ble pipe 1|0` is live AND sets the
config field (`save` persists, mirroring `ble enable`); box_ble_service
auto-arms the relay once per radio-up when pipe_en is saved. Also remoteable
end-to-end — `dservctl set extio/<rx>/config/ble/pipe 1`, `cmd/save`,
`cmd/reboot` all ride the existing config/* forward (CFG_PIPE_EN fires the
live request in on_frame). Zero-touch acceptance proven on `pico` fw
0.48.0-2-pipev16 (deployed via native OTA): datapoint-armed → saved →
rebooted → came back with radio up + pipe STREAM + hh1 relaying, console
never opened. Host extio_ota_push size gate loosened to 1MB advisory (radio
images crossed 500K at v16; the box's pico_ota_begin enforces the real
per-PT slot cap).

Recipe (pt-pico2w.json's 1024K slots fit the 16MB part fine — one radio-class
PT spec for both boards; image ~484K = 48% of a slot):

```
sh build.sh thingplus-handheld --hash                   # slot-A base
OTA_FWVER=<ver>-trial sh build.sh thingplus-handheld --tbyb --hash   # trial (distinguishable fw)
dservctl set extio/hh1/cmd/bootsel 1                    # works over the BLE pipe
PT_JSON=$PWD/pt-pico2w.json ./provision.sh dist/wizchip_dserv_config_thingplus-handheld_hashed.uf2
# later, docked update = bootsel again, then:
picotool load dist/wizchip_dserv_config_thingplus-handheld_tbyb_hashed.uf2 -x
#   -x = flash-update boot of the slot picotool just filled (the inactive one).
#   picotool `load -u` does NOT mean update-boot (it means skip-identical-sectors).
```

**The wedge.** First slot boot: box healthy (console, persist, XIP-from-slot
all fine) but `ble enable 1` → `[1/4] cyw43_arch_init...` → hard wedge → death
at exactly the widened 8s watchdog, 2/2 reproducible; the SAME binary flat
came up in <1s. The "idle" watchdog resets seen after provisioning were the
same bug: the handheld build auto-brings-up the radio at boot (radio = the
transport, ignores ble_en), each non-watchdog boot wedged once, then the
skip-once guard held the box stable with the radio off.

**Root cause** (found via watchdog-scratch breadcrumbs, below): with
`CYW43_ENABLE_BLUETOOTH`, `cyw43_arch_init` also runs `btstack_cyw43_init` →
`setup_tlv` → pico-sdk's `btstack_flash_bank.c`, which reads the TLV banks
(top-of-flash physical offsets, window 3 on the 16MB part) via
**`memcpy(XIP_BASE + offset)` — the ATRANS-TRANSLATED window**. Flat, identity
mapping, correct; slot-booted, garbage → both banks look invalid → btstack
formats/erases (`flash_safe_execute(..., UINT32_MAX)` — a 49-day timeout) and
the verify-read is garbage again → never converges → watchdog. The persist
aperture bug's exact sibling, one library deeper.

**Fix = wiznet-io/patches/ (pico-sdk-slotboot-radio.patch +
cyw43-driver-slotboot-radio.patch), auto-applied by build.sh** (grep-guarded,
so a fresh `.wiznet-pico-c` clone self-heals; the vendored clone is not in
git — the patch files are the durable copy):
- `btstack_flash_bank.c`: TLV bank reads via
  `XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE` + physical offset — mapping-
  independent, identical bytes on a flat boot. (Cleanup note: our own
  pico_flash.h persist loader could drop its ATRANS1-aperture dance for this
  same alias someday.)
- (A TX-DMA source rebase in `cyw43_bus_pio_spi.c` also shipped briefly —
  **REMOVED same day** after the rebase-necessity experiment: a TLV-fix-only
  build slot-booted with the J-Link standing by came up clean end-to-end
  (radio up, both fw uploads DMA'd from translated addresses, pipe streaming,
  fw 0.48.0-2-noreb committed on the receiver). Verdict: **RP2350 DMA honors
  ATRANS translation**; the rebase was an artifact of the blind first
  diagnosis. The whole functional patch is now just the three TLV read sites
  + the BBC() diagnostics.)

**Breadcrumb infra (keep — it's the no-J-Link wedge nailer).** Radio targets
build with `BOX_BLE_BREADCRUMBS=1`: the bring-up arms watchdog scratch[1]
with a magic and every stage of the cyw43 init chain + SPI/TLV path writes a
stage id into scratch[0] (BBC() markers patched through cyw43_arch_poll.c,
cyw43_driver.c, btstack_cyw43.c, btstack_flash_bank.c, cyw43_ll.c,
cyw43_bus_pio_spi.c). Scratch survives the watchdog reset; the next boot
prints `bledbg: last-run radio crumb=0x...` (also in `show`). Map: 0x01
pre-arch-init · 0x08-0x0B arch chain · 0x10-0x1F driver init · 0x14-0x16
btstack init · 0x70+bank TLV read / 0x74+bank TLV erase · 0x30/0x33 bus init /
chip-up · 0x3E fw-check · 0x40xxxxxx fw-download offset · 0x5x SPI transfer.
The hunt that found the TLV: crumb parked at 0x01 proved the wedge preceded
the bus code everyone suspected.

**Scratch-register map — HARD RULE (found 2026-07-17, cost the receiver's
first native OTA arm):** the RP2350 bootrom PARKS `rom_reboot`'s p0/p1 in
**watchdog scratch[2]/[3]** from the API call until the reset actually fires
(pico-bootrom-rp2350 `s_varm_api_reboot`), and the SDK owns scratch[4..7]
(reboot magic/pc/sp). Only **scratch[0]/[1] are free.** The crumbs originally
used [2]/[3]: `pico_ota_arm_update`'s FLASH_UPDATE base sat parked in
scratch[2] for the ~1-2s arm window while core 0's live radio kept stamping
SPI crumbs over it → the bootrom read garbage, matched no partition, silently
booted the OLD slot (`boot=update`, fw unchanged) — while picotool's
identical reboot from BOOTSEL (nothing running) worked, which is what
isolated it. Crumbs now live in [0]/[1]; the hazard is documented at
`pico_ota_arm_update` (pico_ota_slot.h). Corollary kept honest: the earlier
"07-14 proof" only worked because the `usb` target has no radio and no
crumbs — nothing wrote scratch post-arm.

**provision.sh fix (2026-07-17):** passing ONE explicit image used to default
slot B to the DUAL box's tbyb image — which loaded a W6300-EVB build into the
Thing Plus's slot B. Defaults now apply only when NO positional image is
given; an explicit slot A leaves B empty. If a box was provisioned with a
wrong-board slot B, the first docked `picotool load ... -x` overwrites it.
Also remember the rollback invariant when iterating on the bench: after a
trial commits, REFRESH SLOT A with a fixed base too — a rollback target that
wedges defeats the whole point of rollback.

## The frame pipe (built 2026-07-17; bench pair test pending)

What exists: `common/dserv_ble.h` (the FROZEN service/characteristic UUIDs —
d5e70001/2/3-8f2c-4b6a-9ae5-3c7a10a5b2c1 — and the MTU≥131 whole-frame rule),
`pico/extio_pipe.gatt` (ATT DB), `pico/box_ble_periph.h` + `net/box_net_ble.h`
(the handheld: radio on core 0, `box_net` transport shim on core 1, frames
crossing in two pico queues — the g_cmd_q house pattern), and the central
relay inside `pico/box_ble_central.h` (`ble pipe 1|0`: scan-match on the
service UUID → connect → discover → subscribe → STREAM). Relay semantics:

- handheld notifications (one 128B dserv frame per PDU) → box_pipe_rxq →
  rt_main sends them up the receiver's own USB transport. The name inside
  each frame keeps the boxes distinct — the handheld auto-discovers on the
  host as its own `extio/<name>` box, zero host changes.
- host frames whose name is NOT the receiver's (CFG_NONE in on_frame) →
  box_pipe_txq → write-without-response to the handheld. That is exactly the
  handheld's config/cmd traffic once extioconf wires its forwards.
- `ess/in_obs` is NOT yet forwarded (sync stage next), so handheld events
  carry timestamp 0 → dserv arrival-stamps them (receiver-relay arrival ≈
  ms-class). Fine for the bench; the echo-sync stage replaces this.

## Echo-sync progress (2026-07-18, IN PROGRESS)

Measurement-first (see "Time"). Ground-truth rig: receiver GP14 wired to
handheld GP26, `host/echo_soak.sh` drives level-set edges (state/do = the
near-truth local reference), `host/echo_analyze.py` pairs rising edges and
reports the Δ = handheld_ts − reference_ts distribution.

- **BASELINE (arrival-stamped, pre-sync): median +23.5 ms**, p10–p90 +8.9 to
  +35.7 ms, radio floor (clean min) ~6–10 ms. Load-dependent (sustained drive
  ~23 ms vs light ~12 ms). Caveat: the reference is itself arrival-stamped over
  the same USB/receiver path, so its publish lag adds tens-of-ms jitter (occa­
  sional negative Δ outliers = do/14 lag, not a handheld that's early) — trust
  the median/percentiles, not min/max. Resolving sub-5 ms improvements will need
  a tighter reference (sync the receiver's own g_clock via a bench TTL / obs
  cycling so state/do is box_clock-stamped at actuation, not on arrival).
- **INCREMENT 0 — raw source stamp + receiver rewrite scaffold: DONE + VERIFIED
  (fw 0.48.0-inc0 committed on both boards).** Handheld now emits raw
  `time_us_64` via the single `event_stamp()` seam (BOX_NET_BLE branch); the
  receiver rewrites hh→receiver→dserv at the relay drain (`pipe_rewrite_ts`,
  composing a new per-handheld `g_hh_clock` with its own `g_clock`;
  `dserv_msg_set_timestamp` does the in-place ts surgery at offset 3+varlen).
  `g_hh_clock` is unsynced → stamp returns 0 → arrival-stamp → transparent
  no-op. Re-measured median +22.6 ms (== baseline). POSITIVE proof, not just
  "didn't crash": 187/200 edges still pair within ±100 ms, which is only
  possible if the raw ~50-Mµs handheld value is being correctly collapsed to 0
  by the rewrite — the new paths are exercised and correct. Matched-pair flash
  rule: handheld-raw and receiver-rewrite ship together (flash receiver first —
  it handles an old handheld's ts=0 fine; a new handheld to an old receiver
  would ship garbage).
- **INCREMENT 1 — echo cmd/reply pair: DONE + VERIFIED (fw 0.48.0-inc1 /
  -inc1b).** A marker-byte 'E' frame (dserv_ble.h) rides the SAME two GATT
  characteristics as data but is handled entirely at the radio boundary (core 0
  both ends, dispatched by frame[0]) — never the framer or the relay queues.
  Receiver sends REQ stamped r0 (box_ble_central initiator, ~3/s while
  streaming); handheld reflex-replies from the ATT-write callback (box_ble_periph),
  stamping h_recv on receipt and jumping the data queue in CAN_SEND_NOW;
  receiver stamps r1 on the notification, rtt = r1−r0, h_recv maps to the
  midpoint r0+rtt/2. r0 is echoed so the receiver stays stateless. MEASURE-ONLY:
  publishes state/echo/{rtt_us,offset_us}; nothing feeds g_hh_clock yet.
  - **THE VERIFY-FIRST PAYOFF: default btstack connection params are unusable
    for sync.** First measurement: RTT 56–236 ms, offset noise ±45 ms (> the
    whole ~1 ms target). Cause: no connection parameters were ever requested —
    `gap_connect` used btstack defaults (coarse interval). Fix (RECEIVER-ONLY,
    the central dictates params): `gap_set_connection_parameters(...)` before
    gap_connect — **15 ms interval pinned + ZERO peripheral latency** (handheld
    listens every event; power-saving latency is a later adaptive concern), 4 s
    supervision. Negotiated interval now logged on connect + in `ble` status.
  - **AFTER THE FIX (rig, 15.00 ms confirmed):** RTT min 25.6 ms, p10–p90 within
    **1.5 ms** (reflex is constant-latency — a full-interval smear would mean
    variable latency); occasional tail to ~116 ms = missed conn events, which
    min-RTT filtering is designed to reject. Offset drift **−25 ppm** (was −730
    ppm of pure noise — a believable crystal offset, cf. the +27–43 ppm hw-sync
    measured). **Min-RTT-filtered offset noise ≈ 200 µs** (RTT ≤ min+0.5 ms) —
    sub-ms, the actual input quality the estimator will see.
- **INCREMENT 2 — min-RTT estimator: DONE + VERIFIED (fw 0.48.0-inc2, receiver-
  only). PHASE GOAL MET: +22.6 ms → +0.37 ms median, sub-ms across the whole
  distribution.** Core-0 sampler pushes raw {r0,h_recv,rtt} to an SPSC queue;
  core-1 `echo_estimator_service` keeps the lowest-RTT sample per 1 s bucket
  (min-RTT filter) and feeds it to `box_clock_sync(&g_hh_clock, r0+rtt/2, h_recv,
  trusted=1)`. g_hh_clock is written AND read (pipe_rewrite_ts) on core 1 only —
  no cross-core struct tearing (the flagged risk; solved by the queue, not a
  lock). Buckets whose best RTT is > floor+8 ms are skipped. New telemetry:
  state/echo/{synced (0/1/2), rate_ppb}.
  - **RIG RESULT (200 edges, echo-sync live):** di/26−do/14 delta median
    **+0.37 ms**, p10–p90 **−0.15 to +0.80 ms**, stdev 0.33 ms, 200/200 paired
    (vs baseline +22.6 ms / ±18.7 ms). ~60× better and inside the ~1 ms target.
    synced=2 within ~1 s of pipe-up; rate settled around −1…−13 ppm (per-anchor
    bounce is fine — offset snapped every 1 s bounds inter-anchor drift to ~25 µs,
    so offset accuracy dominates, not rate).
  - **MEASUREMENT NOTE (the "tighten the reference" step, finally needed):**
    pipe_rewrite_ts composes g_hh_clock (echo) AND g_clock (receiver→dserv). On
    the bench g_clock has no obs anchor, so it returns 0 and collapses BOTH di/26
    and do/14 back to arrival-stamp — hiding the win. Fix for the bench: toggle
    `ess/in_obs` (~1/s) during the run to keep g_clock fresh. Because the g_clock
    hop is common to both di/26 and do/14, its offset noise CANCELS in the delta —
    what's left is pure echo accuracy. Real rigs run ess, so g_clock is anchored
    for free.
  - Residual +0.37 ms = handheld DI-detect + reflex-stamp bias (h_recv is stamped
    a few instr after receipt, ~P/2) + do/di path asymmetry; trimmable but well
    inside spec, left as-is.
- **PHASE COMPLETE. Remaining (separate stages):** bonding (bonded-address
  allowlist replacing first-advertiser connect; the pipe is open until then);
  and the power tradeoff — peripheral latency was pinned to 0 for clean echo, so
  when idle-power optimization returns, keep latency 0 during active sync (or
  probe densely then back off), since latency > 0 re-coarsens the RTT.
- connect policy is first-advertiser-with-the-service-UUID (bench); the
  bonding stage replaces it with a bonded-address allowlist. The pipe is
  open/unencrypted until then.

Thing Plus header-pin notes (2026-07-17 bench): GP26 (A0) verified as a DI
end-to-end; the hole silked "17" did NOT produce edges on GP17 (silk label vs
GPIO mapping unverified — SparkFun publishes no pinout diagram). To map any
hole empirically from the console: configure the safe set as inputs
(`pin N mode in_pullup` for N in 8-12,14-22,26-28), ground the mystery hole,
and the `di:` print (debug 1) names the real GPIO. NEVER configure 23/24/25/29
(RM2 radio lines — touching them can kill the link), skip 6/7 (Qwiic/fuel I2C
traffic = constant chatter) and 0/1 (UART fallback console); 13 is the
peripheral-power regulator ENABLE — input+pullup is safe, driving it low is
not.

Board note (2026-07-17): the REAL handheld builds already —
`sh build.sh thingplus-handheld` → dist/wizchip_dserv_config_thingplus-handheld.uf2.
Zero pin adjustments were needed: the SDK board header owns the RM2 wiring,
and on the Thing Plus the RM2 even sits on the SAME GPIOs as the Pico 2 W
radio (REG_ON 23 / DATA 24 / CLK 29). RP2350A (30 GPIO, PICO_NPINS matches),
16MB flash (TLV/persist layout identical, roomier), fuel gauge on default
I2C1 = Qwiic (GP6/7) compiled in → state/battery rides the pipe. Pins to
AVOID for buttons on this board: 23/24/25/29 (radio), 6/7 (Qwiic/fuel), 0/1
(UART fallback console), 2-5 (default SPI0 / microSD). Header-exposed pins in
the teens (check the silk, e.g. GP16-18) are safe test buttons.

Bench pair procedure (receiver + Thing Plus — or a second Pico 2 W — as the
handheld; swap the uf2 name accordingly):

```
# handheld board (BOOTSEL):  picotool load dist/wizchip_dserv_config_thingplus-handheld.uf2
#   console:  name hh1        <- MUST differ from the receiver's name (both default "pico")
#             save            (radio auto-starts: it IS the transport; watch [1/4]..[4/4],
#                              then "advertising as extio-hh1")
#             pin 2 mode in_pullup   (a test button: jumper GP2 to GND)
# receiver board (BOOTSEL):   picotool load dist/wizchip_dserv_config_pico2wusb.uf2
#   console:  ble enable 1  (+ save)
#             ble pipe 1     -> "pipe device xx:.. -- connecting" -> "pipe STREAMING (mtu=247)"
# handheld console: ble      -> pipe=UP, tx/notified counters moving (heartbeats)
# host (dserv + extioconf):  extio/hh1/state/* datapoints appear via auto-discovery;
#                            jumper GP2 -> extio/hh1/state/di/2 events; config pushes
#                            (e.g. labels) route back over the radio automatically.
```

Host discovery vs the handheld (hit live on the 2026-07-17 bench, FIXED same
day): the handheld's dead data CDC won macOS's old highest-cu.usbmodem
heuristic and extioconf read silence — and note macOS numbers these by USB
PORT TOPOLOGY, so plug order can never fix it. Two-sided fix, both landed:
(1) FIRMWARE identity — BOX_NET_BLE builds enumerate as product **"dserv
handheld"**, PID 0x100C (no "extio" substring, so the Linux by-id glob
`*extio*if02*` can never match a handheld by construction; receiver/box
builds stay "extio USB box"/0x100B); (2) extioconf's Darwin path is now
IDENTITY-FIRST via ioreg (Product Name == "extio USB box" → its *3 tty,
mapped tty.→cu.), falling back to the blind heuristic only when no box
product is present. Handhelds flashed BEFORE the identity build still
masquerade — reflash them before sharing a host with a receiver. `ble` on
either end dumps pipe counters (tx ok/drop/nolink, notified, relay
up/down/drops) — the first thing to look at if frames don't flow.

## Phases

### Phase 1 — bench pair: real receiver + wired test transmitter

Two Pico 2 Ws on the bench. **Receiver** = the goal artifact: pico2w box build
+ BOX_BLE, on USB to a dev host running dserv + extioconf. **Test transmitter**
= second Pico 2 W running the `handheld` app, USB-powered: console for poking,
GPIO jumpers as "buttons," and — the point of wiring it — a **shared GPIO
ground-truth wire** driven into both boards so stamp accuracy is measured
against hardware truth with the obs_soak / sync_analyze methodology, exactly as
the hw-sync-input validation was done.

Proves, in order of risk:
1. TinyUSB + cyw43/btstack + the superloop coexisting in one image (core-0
   placement) — the biggest unknown.
2. Frame pipe over GATT: MTU negotiation, framer under fragmentation, relay,
   name routing, manifest-on-connect, link-down handling.
3. Echo-sync discipline: min-RTT filtering converging; stamp error vs the
   ground-truth wire.
4. End-to-end into dserv: handheld auto-discovered as a named box; edges appear
   via the ess joystick API with translated stamps.

Exit criteria: stamp error ≤ ~1ms median vs ground truth; delivery-latency
distribution characterized (median + tail) and recorded here; multi-hour soak
with zero relay stalls; `ble 0|1` + fail-soft init verified; disconnect/
reconnect (walk out of range, power-cycle TX) recovers without touching the
host.

### Phase 2 — Thing Plus handheld

Same app on the real hardware: battery bring-up (charge/dock story), fuel-gauge
telemetry → fleet page tile, sleep + peripheral-latency tuning against measured
battery life, real buttons/joystick + chord groups, enclosure (plastic only —
never metal around the antenna). Measure the power table for real.

### Phase 3 — rig deployment

Swap one behavioral rig's Pico 2 → Pico 2 W (same firmware family, one flash),
bond the handheld, wire the task to the existing joystick/response latch API,
soak in the actual room RF. Battery/link telemetry watched on the fleet page.

Parallel zero-firmware option (unchanged from 2026-07-09): a commercial BT
gamepad through the legacy joystick path still validates *paradigm* questions
for free — no stamps, no manifest, but useful anytime before Phase 3.

## Risks / open items

- **btstack license**: granted via Raspberry Pi for their silicon/modules —
  Pico 2 W clearly covered; **confirm the grant covers RM2 on third-party
  boards** (Thing Plus) before deep integration. Believed yes; verify.
- cyw43+TinyUSB+superloop coexistence is well-trodden in pico-sdk but not in
  *our* loop — hence it's Phase 1 risk #1.
- Real conn-interval floor and notification throughput on this stack: measure,
  don't assume 7.5ms.
- Obs-boundary handling of late-stamped events (extractor check).
- Room RF (many APs + BLE): tails are the concern, not medians — source stamps
  protect measurement, so only closed-loop uses feel it.

## Alternatives considered

- **ESP32-C5** (dual-band WiFi 6 + BLE): wrong tool for *this* role — second
  toolchain (ESP-IDF), single-core, zero code shared with either end. Its case
  remains the **robust-WiFi box track**, decoupled; note the Phase-1 receiver
  doubles as the "pico2w on a good AP" WiFi testbed that plan called for.
- **ESP-NOW** (~1–2ms, dead simple) forces ESP32 at both ends — only if we ever
  go all-in on C5 boxes.
- **nRF24L01 on SPI both ends**: the escape hatch if BLE tail latency ever
  disappoints (sub-2ms) — bespoke pairing/coexistence, keep on the shelf.
- **nRF52840/Zephyr**: better BLE silicon, third ecosystem — rejected for
  codebase unity. nRF54L-class parts worth a look only for a future handheld
  rev chasing battery life.

## Appendix — UART sidecar contingency (superseded 2026-07-16, preserved)

The 2026-07-09 committed topology, kept intact in case a rig ever genuinely
needs a radio at an Ethernet-only box (none is expected — Ethernet rigs are
wired-only by policy; even then, first resort is the USB receiver plugged into
that rig's host, since BLE range across a rig room is normally fine).

- **Shape**: Pico 2 W runs btstack CENTRAL + a slice of the portable core,
  forwarding 128-byte frames over UART into the W6300 box; box side adds a
  small UART-ingest path (UART bytes → the same dserv_framer → on_frame,
  analogous to box_net_server_poll but fed from UART). btstack never runs on
  the box's RP2350. Handheld events publish under a distinct name (same
  named-box principle as the main design).
- **Wiring**: 4-wire connectorized tail — TX, RX, **5V→VSYS**, GND. Power at
  VSYS so each board makes its own 3.3V (the CYW43 draws real current; never
  hang it off the box Pico's 3.3V reg). UART direct at 3.3V, no shifter;
  2–4Mbaud on a free hw UART with CTS or credit frames (128B ≤ ~1.3ms even at
  1Mbaud). PINMAP free pins 9–14/26/27 cover it. JST on the box end —
  unpluggable, can't wiggle loose mid-experiment.
- **Mounting**: Pico 2 W in an Adafruit snap-on Pico case (#6252), strain-relief
  inside, USB cutout free so the radio board reflashes without opening.
  Plastic only (RF-transparent); antenna edge faces away from the Ethernet
  jack/metal. Carrier PCB only if ever stamping out many rigs.
- **Sync**: bridge→box over UART echo (trivially sub-ms), or one GPIO into the
  box's rig-validated TTL sync input.
