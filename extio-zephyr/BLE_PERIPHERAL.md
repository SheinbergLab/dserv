# BLE_PERIPHERAL.md — the peripheral role on Zephyr/Nordic

**Status: PLAN, no code started (2026-08-22).** Written after the nRF52840 scout
port ran on hardware, which settled several things this plan depends on.

This is a **port of a validated role, against a frozen contract** — not a design
from scratch, and the plan should be read that way. Two documents remain
canonical and are not re-litigated here:

* **`wiznet-io/BLE.md`** — the radio contract, the topology, the time model, and
  the shipped Pico 2 W handheld (`hh1`) that already implements the peripheral
  side and is deployed.
* **`wiznet-io/NORDIC.md`** — why the battery tier is Nordic at all, and the
  two-tier decision (RP2350 keeps every wired box; Nordic takes the peripherals).

## 1. What is already fixed, and must not move

`src/core/dserv_ble.h` is the contract, frozen 2026-07-17, and it is already in
this tree:

| | |
|---|---|
| Service | `d5e70001-8f2c-4b6a-9ae5-3c7a10a5b2c1` |
| TX (peripheral → receiver) | `…0002…`, **NOTIFY**, whole 128-byte frames |
| RX (receiver → peripheral) | `…0003…`, **WRITE_WITHOUT_RESPONSE**, cmd/config |
| ATT MTU | `DSERV_BLE_MTU_MIN` = 131 — one frame per PDU |
| Advertised name | `extio-<cfg name>`, in the scan response; the adv PDU carries the **service UUID**, which is what the receiver matches on |
| Echo-sync | `'E'` frames, `'?'`/`'!'`, `r0` at byte 2, `h_recv` at byte 10 |

**The unit end-to-end stays the 128-byte `dserv_msg` frame.** BLE.md's framing is
the design in one sentence: *the radio is just another pipe carrying them;
nothing upstream can tell.* Keep the resync framer on both ends even at MTU 131,
because a negotiation that lands at 23 must degrade to slow, not to broken.

A Zephyr **central** already speaks this contract: `src/platform/box_ble.c` (272
lines, RW612), which scans, connects up to `CONFIG_BT_MAX_CONN` peers,
subscribes, and enqueues frames for the service loop. It compiles unmodified on
the nRF52840 with the open-source controller (233 KB flash / 168 KB RAM,
measured 2026-08-22). **The central end is not the gap.**

## 2. The actual gap

There is no peripheral role in this tree. That is the whole of the missing work,
and it is smaller than it sounds because of where the seam falls.

**The peripheral must present itself to the rest of the firmware as another
uplink.** `box_uplink.c` already arbitrates USB and Ethernet candidates; BLE
peripheral becomes a third. Everything above the transport — `dserv_config.h`
dispatch, the announce/manifest, DI publish, groups, `ain` groups, `box_cli`,
`box_persist` — then works **unchanged**, because it already only knows how to
hand 128-byte frames to an uplink. That is not a convenient accident; it is the
seam the wiznet-io tree was cut along, and NORDIC.md's audit measured it (1,562
LOC portable, zero pico-sdk includes).

So the deliverable is essentially **one new file**:

    src/platform/box_ble_periph.c   — GATT server + advertising + the uplink face

implementing: register the service with the two characteristics, advertise with
the service UUID and `extio-<name>`, notify frames out of the TX characteristic,
feed writes on RX into the existing framer, and answer echo-sync.

Everything else it needs already exists and is hardware-validated: `dserv_msg.h`
codec and framer, `box_gpio` DI capture with ISR-stamped edges and the debounce
settle machine, `box_group`/`box_ain_group`, `box_persist`, `box_clock`.

## 3. The clock, which is where the care goes

BLE.md's rule is **stamp at source, one clock boundary, rewrite once.** The
consequences for the peripheral build are specific and at least one of them is a
real code change, not a configuration:

* **Edges are stamped on the PERIPHERAL's own µs clock**, at GPIO IRQ, by
  `box_gpio`'s `now_us()`. Today's nRF52840 work matters directly here: that
  function returned 0 forever on that part until 2026-08-22, and the fix is
  keyed on `CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER`. **The L-series is fine** —
  nRF54L15 and nRF54LM20A both use GRTC, which selects the symbol, so they get a
  real cycle counter. An nRF52840 peripheral gets the 30.5 µs tick fallback,
  which is inside this tier's needs but should be a conscious choice.
* **A peripheral must send its RAW source clock, not a dserv-mapped time.**
  `event_stamp()` currently routes through `box_clock_stamp()`, which returns 0
  until the box has been synced — and a peripheral is never synced, because
  nothing sends it `ess/in_obs`. A 0 tells dserv to arrival-stamp, which throws
  away the very thing the design is trying to preserve. **The peripheral build
  must publish the source clock directly**, and the receiver rewrites the ts
  field in place at the radio boundary (BLE.md: "translate exactly once").
  *This is the one place the existing firmware is actively wrong for the new
  role, and it should be the first thing written down in code.*
* **The peripheral's echo-sync obligation is small and specific**: reflex-reply
  from the ATT write callback with minimal and, above all, *near-constant*
  latency — echo `r0` back untouched, add `h_recv`. All the estimation lives in
  the receiver (minimum-RTT filtering, adaptive-EMA rate). The peripheral must
  not average, buffer, or defer the reply into a work queue; jitter added here is
  indistinguishable from radio jitter and cannot be filtered out later.

## 4. Phases, with exit criteria

Mirrors how the wired ports were run: each phase ends on a *measurement*, not on
"it seems to work."

**Phase 0 — role scaffold, wired.** Build a peripheral image for a USB-equipped
Nordic board; bring the GATT server up; verify advertising and a connection from
the existing `box_ble.c` central. *Exit: the central logs a connection and an
MTU ≥ 131, and one hand-built frame arrives intact.*

**Phase 1 — the pipe.** Peripheral publishes its own manifest and DI events;
config pushes route back over RX. *Exit: `extio/<name>/state/di/<n>` appears in
dserv from a button on the peripheral, with `extio_cfg_set` reaching it over the
radio — i.e. parity with what `hh1` does today on the Pico.*

**Phase 2 — time.** Echo-sync in-band; receiver maps and rewrites. *Exit: a
button wired to BOTH a wired box and the peripheral shows the two events within
the accuracy BLE.md predicts (~1 ms after convergence). This is the whole point
of the tier and it deserves a physical two-source comparison, not a plot of the
peripheral's own numbers.*

**Phase 3 — power and packaging.** Connection interval and peripheral latency
tuning; battery life on the actual cell; sealed-device provisioning. *Exit: a
measured runtime figure on the real battery, and a documented way to update and
name a device that has no USB.*

## 5. Board order, and why it is not the obvious one

**Prototype on a USB-equipped part; ship on the part without USB.**

`nrf54l15.dtsi` has **no USB node at all** — verified, and consistent with
NORDIC.md. For a sealed battery peripheral that is arguably correct: BLE DFU is
the right update path for something with no connector. But the entire bring-up
method this project relies on runs through the USB/serial console. The
2026-08-22 nRF52840 session is the evidence: `now`, `show`, `pin N mode`,
`ain group`, the loopback measurement — all of it over the console, and the two
defects found that day (a dead clock, a silent debounce path) were *both*
diagnosed there.

Developing a new role without that is masochism. So:

| stage | board | why |
|---|---|---|
| Phases 0–2 | **XIAO nRF52840** or **XIAO nRF54LM20A** | USB console for development; L-series also gives the real cycle counter |
| Phase 3 → ship | **XIAO nRF54L15** | sealed, better power, same SAADC and GRTC idioms — nothing learned is wasted |

Nothing is thrown away moving between them: same SoC family, same analog
compatible, same clock source class.

## 6. Open questions — decisions this plan deliberately does NOT make

* **Which central?** `box_ble.c` was written for the RW612 hub. If the trainers
  standardise on the XIAO nRF54LM20A (BLE **and** USB-HS in one part), the hub
  could be an LM20A instead, and the RW612 becomes one option rather than the
  assumption. Worth settling before Phase 2, since the receiver owns all the
  clock estimation.
* **OTA over radio.** BLE.md excludes multiframe `'D'` over the radio in v1. But
  Zephyr gives MCUmgr/SMP-over-BLE essentially free, and a sealed device with no
  USB *needs* a wireless update path. Reconsider on Zephyr's terms rather than
  inheriting the Pico-era exclusion — and note the XIAO LM20A's MCUboot only
  started linking on 2026-08-22 (`sysbuild/mcuboot_xiao_nrf54lm20a.conf`).
* **Role selection at build time.** Central and peripheral can coexist in one
  Zephyr image, but a battery peripheral should not carry the central. Expect a
  Kconfig choice (`BOX_BLE_ROLE`), following BLE.md's "compiled per-board,
  enabled at runtime" policy for the radio itself.
* **Proprietary radio (ESB/Gazell) is NOT on this path**, and the reason is
  structural rather than technical: neither is in upstream Zephyr — they ship in
  NCS, which this tree does not use. ESB would buy *closed-loop* latency
  (sub-ms vs BLE's 7.5 ms connection-interval floor), and this tier's job is to
  **report** events rather than to close a loop. A press stamped at source and
  delivered 10 ms later is still accurate to sub-ms. Revisit only if a wireless
  device must gate a stimulus or deliver reward — and ask first whether that
  device can be wired. If latency ever does force the issue, **802.15.4 is in
  upstream Zephyr** and the XIAO LM20A board already enables `&ieee802154`,
  which is a far cheaper escape hatch than adopting NCS.
* **Bonding** is done and verified on the Pico side (BLE.md, 2026-07-18) but
  **uncommitted**. Land that before Phase 1 or the security model gets rebuilt
  from memory.

## 7. What the 2026-08-22 hardware session already de-risked

Recorded so the plan starts from evidence rather than hope:

* `box_ble.c` (central) **compiles unmodified** on nRF52840 with the upstream
  open-source controller — the seam holds across the vendor change.
* The whole non-radio stack is hardware-proven on Nordic: boot, NVS persistence
  across reboot *and* reflash, analog (6 ch, 12-bit, 3300 mV fs), DI with
  debounce, obs mirror, and hardware sync anchoring (`sync/source = hw`).
* The box clock was measured, not assumed: a loopback pulse sweep (1/5/10/50 ms)
  showed a **constant** ~+65..+104 µs error rather than a proportional one, so
  the crystal rate is sound and the residual is quantisation plus timer
  round-up, with ~±20 µs jitter.
* Nordic L-series gets a real 64-bit cycle counter via GRTC; only the nRF52840
  needs the tick fallback.
