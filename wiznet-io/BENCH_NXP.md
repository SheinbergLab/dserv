# BENCH_NXP.md — NXP crossover/wireless MCUs as a universal extio hub

**Status: evaluation plan, 2026-07-22, no hardware ordered yet.** Product of a
spec audit of two NXP FRDM boards against the shipping W6300+RP2350 extio box,
prompted by (a) the TSN/PTP timing question and (b) a future need for
high-bandwidth host↔box transfers. This document exists so a purchasing and
bring-up decision starts from measured numbers, not datasheets. Companion docs:
BLE.md (frozen radio contract — still canonical for the pipe), NORDIC.md
(battery-peripheral tier), OTA.md, PINMAP.md, TESTING.md.

**Strategic decision this plan tests.** Split the fleet by *role*, not by chip:

- **Peripheral / battery / handheld side → Nordic (nRF54L15).** Unchanged from
  NORDIC.md. Coin-cell peripherals, months-to-years runtime. RW612 is a Wi-Fi 6
  SoC and cannot play here — power budget is wrong by orders of magnitude.
- **Hub / receiver / wired-box side → candidate: NXP RW612.** A *universal
  transport hub* that could collapse THREE current threads into one part:
  the W6300+RP2350 wired box, the pico2w BLE receiver, and the parked ESP32-C5
  Wi-Fi box. **The "hub side" of the Nordic plan goes on hold** pending this
  evaluation — if RW612 proves out, Nordic never needs to grow a hub.
- **Interop is already solved.** The frozen `d5e7000x` GATT contract (BLE.md)
  means a Nordic peripheral talks to an RW612 hub over the same UUIDs with no
  new protocol work. That is the linchpin that makes the role-split clean.

RT1186 is evaluated alongside but for a *different* job — see §2.

## 1. Goal

Decide, from numbers comparable to our W6300 baselines, whether a single NXP SoC
should become the extio hub across all desired transports (USB-HS, Ethernet,
BLE, and later Wi-Fi), and whether a higher-bandwidth streaming-DAQ box is worth
a second part. The portable core (~1.5k LOC, common/net + protocol) is expected
to port verbatim; only the platform layer changes (RTOS/Zephyr, not bare-metal).

## 2. The two candidates are not competing for the same job

| | **RW612** (converged hub) | **RT1186 / i.MX RT1180** (bandwidth/timing) |
|---|---|---|
| Intended role | **Replace** the current box + fold in BLE/Wi-Fi | **Second** box class for streaming DAQ |
| Cores | Cortex-M33 @ 260 MHz, 1.2 MB SRAM | Cortex-M7 @ 800 MHz + M33 @ 300, 1.5 MB |
| USB | HS 2.0 OTG + integrated PHY (480 Mbps) | HS 2.0 OTG + PHY (480 Mbps) |
| Ethernet | 10/100 RMII + **IEEE 1588 PTP** | **GbE ×5 (4 TSN) + integrated TSN switch** + PTP |
| Radio | **BLE 5.4 + Wi-Fi 6 + 802.15.4/Thread** | none |
| On-chip analog | **GAU: GPADC + DAC + ACOMP** (specs TBD) | ADC (specs TBD) |
| Expansion | mikroBUS + Arduino + Pmod | Arduino UNO R3 |
| Software | Zephyr (supported) | Zephyr (supported) |
| Board | FRDM-RW612 | FRDM-IMXRT1186 |

**Read:** RW612 is the drop-in evolution of what we ship (same jobs, one chip,
+wireless, +PTP, 40× USB headroom over the RP2350's Full-Speed 12 Mbps ceiling).
RT1186 earns a place only when the sustained data rate exceeds Fast Ethernet
(~10 MB/s) or a TSN daisy-chain topology (switch-per-box, no external managed
switch) is wanted. Buy RW612 first.

## 3. Baselines to beat (our measured W6300+RP2350 numbers)

| Metric | Current | Source |
|---|---|---|
| Clock-sync jitter (Tier A) | **98 µs** | wiznet sync verified |
| Transport RTT median | **300–340 µs** | v14 hw obs-sync rig |
| Scheduled-pulse actuation (Tier C) | **34 µs** (0.000 ms offset) | wiznet sync verified |
| USB-CDC practical ceiling | **~1 MB/s** (USB-FS 12 Mbps) | usb benchmark handoff |
| Drift, uncorrected | **+27 → +43 ppm** (EMA handles) | v14 rig |
| Handheld echo-sync median | **+0.37 ms** | echo-sync complete |

## 4. Devices under test

FRDM-RW612, FRDM-IMXRT1186, and W6300+RP2350 (control/baseline). Identical suite
on all three; results into one comparison table (§7 format).

## 5. The decisive question

Not peak throughput — the datasheets answer that. The question a spec sheet
*cannot* answer: **can the box push bulk data AND keep the sub-ms reactive
control path and the 34 µs scheduled-actuation intact at the same time?** That
contention (D2, D4) is the whole "one converged box" bet.

## 6. Benchmark dimensions

Harness reuse is deliberate — each dimension names the existing tool so results
land next to our baselines with no new analysis code.

### D1 — Bulk throughput (new dimension; no W6300 equivalent)
- Host↔box sustained transfer, payloads 64 B → 64 KB, one-way (box→host, the DAQ
  direction) and round-trip. Per transport: USB-HS, Ethernet (GbE/FastE).
- **Metrics:** sustained MB/s, MCU core utilization at rate, dropped/retried
  frames over a 10-min soak.
- **Pass:** USB-HS ≥ 30 MB/s, zero drops; Ethernet ≥ 8 MB/s (RW612) / ≥ 80 MB/s (RT1186).

### D2 — Control-path RTT *under* streaming load (decisive)
- Run the `echo_soak` RTT loop WHILE D1 saturates the link in the background.
- **Metrics:** RTT p50/p99/max at 0 / 50 / 100 % background stream, via `echo_analyze`.
- **Pass:** p99 control RTT stays **sub-ms** at 100 % stream load. If streaming
  wrecks control latency, the one-box story fails here.

### D3 — Clock sync: hardware PTP vs software estimator
- Enable IEEE-1588 hardware timestamping; compare against echo/min-RTT baseline
  on the same wire.
- **Metrics:** median offset + jitter (`echo_analyze`); PTP servo offset from MAC timestamps.
- **Pass:** PTP jitter ≤ 10 µs (target ≤ 1 µs) vs 98 µs software baseline; holds under D1 load.

### D4 — Scheduled-actuation precision (Tier C, under load)
- Scheduled GPIO pulse off the synced local clock; scope-measured (Digilent),
  idle vs 100 % stream load.
- **Metrics:** actuation offset + jitter.
- **Pass:** ≤ 34 µs and **unchanged under bulk load** — proves local scheduling
  stays immune to network traffic on the new silicon.

### D5 — Obs-timeline anchoring & drift
- Reuse `obs_soak` + `sync_analyze.py`: beginobs anchor jitter and ppm drift over
  a multi-hour session.
- **Pass:** no worse than W6300; ideally PTP shrinks it.

### D6 — Robustness / recovery
- Sleep/wake, cable-pull, USB re-enumeration, dserv-restart re-registration —
  the existing failure catalog.
- **Pass:** matches W6300 box (dead-reader recovery, reg self-heal, bounded sends
  vs vanished CDC).

### D7 — Portability cost (deliverable, not a runtime metric)
- LOC of platform layer rewritten to bring the portable core up on Zephyr; list
  core files ported **verbatim** vs needing shims.
- **Output:** go/no-go migration-effort number, comparable to the Nordic estimate.

### RW612-only extras

### D8 — BLE-as-box (collapses the pico2w receiver)
- Run the handheld against RW612's native BLE. Re-verify echo-sync (+0.37 ms
  median) and bonding (Just Works + LE Secure Connections) on the new radio.
- **Watch:** single-connection gap noted for the receiver role — confirm how many
  concurrent peripherals the RW612 BLE stack sustains.

### D9 — Wi-Fi 6 transport (tests the ESP32-C5 thesis)
- Sanity-check the parked "robust Wi-Fi box" idea on this radio: RTT stability on
  a good AP, to see whether RW612 also retires that separate plan.

### D10 — On-chip analog vs MCP3204 (answers "do we still need the 3204?")
- The RW612 has an on-chip **GAU GPADC** (+ DAC + ACOMP), and the FRDM board has
  mikroBUS, so the existing MCP3204 Click path is trivial to keep. Characterize
  both for an eye-signal-grade input:
  - GPADC noise floor / ENOB, **with the radios idle vs actively transmitting**
    (RF coupling into on-chip analog is a real hazard on a tri-radio SoC).
  - Input range / reference vs the MCP3204's 0–3.3 V (the Eyelink 0–3.3 V gotcha
    applies to whichever ADC we pick).
  - Reuse of the existing analog pipeline: `em::process_analog`, the shared
    block decoder (`lib/extio-1.0.tm`), and calibration persistence
    (`lib/settingsdb-1.0.tm`, `db/calibration.db`) assume the MCP3204 group
    format — on-chip GPADC means re-validating that path.
- **Decision rule:** on-chip GPADC is the default for low-channel-count,
  non-critical analog (fewer parts, no SPI hop, lower latency). **Keep the
  MCP3204-over-mikroBUS path** where (a) channel count / simultaneous sampling
  exceeds the GPADC, or (b) D10 shows radio RF coupling degrades eye-signal ENOB
  — an isolated external ADC is likely quieter on a Wi-Fi/BLE part. Decide
  per-signal, not globally.

## 7. Deliverable

One comparison table — columns **W6300 / RW612 / RT1186** × rows **D1–D7**, plus
RW612's **D8–D10** — so the buy decision is numbers, not datasheets. Plus the D7
portability write-up and the D10 analog decision.

## 8. Open questions to close during bring-up

- GAU GPADC actual resolution / channel count / sample rate (datasheet — not in
  the Zephyr board page).
- RW612 BLE concurrent-connection ceiling (D8) — matters if the hub must receive
  several peripherals at once.
- RT1186 GbE sustained throughput achievable from the M7 + DMA in practice (D1).
- Zephyr driver maturity for GPADC, PTP servo, and USB-HS bulk on both boards —
  how much "supported" means "works at rate."

## 9. Sequencing

1. Order **FRDM-RW612 first** (the part that maps onto what we ship).
2. Bring the portable core up on Zephyr; record D7 as you go.
3. Run D1–D6 vs the W6300 golden reference (the deployed box stays untouched as
   the comparison, exactly as the pico2w receiver does for the Nordic hub).
4. Run D8–D10 (BLE, Wi-Fi, analog).
5. Only if D1 shows Fast Ethernet is the ceiling for a real workload, order
   FRDM-IMXRT1186 and repeat D1–D7 for the streaming-DAQ verdict.
