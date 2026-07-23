# extio-zephyr — unified extio hub on NXP RW612 (Zephyr)

A **converged extio box** on the NXP RW612 SoC: USB-HS + Ethernet(+PTP) + BLE +
Wi-Fi in one part, running Zephyr. Intended to fold three current threads into
one — the W6300+RP2350 wired box, the Pico 2 W BLE receiver, and the parked
ESP32-C5 Wi-Fi box. Rationale, board comparison, and the D1–D10 benchmark plan
against the shipping W6300 baselines live in [`../wiznet-io/BENCH_NXP.md`](../wiznet-io/BENCH_NXP.md).

**This is a fork, not a move.** The shipping `wiznet-io/` RP2350 tree is 100%
untouched. Once RW612 proves out, the shared core here and there collapses into a
single `extio-core/` (see BENCH_NXP.md §D7).

## Boards

The same source builds for three targets. The Teensy 4.x boards are **test
systems**: the i.MX RT1062 drives Ethernet/1588 through the *same* Zephyr drivers
as the RW612 (`nxp,enet`, `nxp,enet-ptp-clock`) and is USB-HS, so blocks #2–#5
plus the uplink arbiter can be validated on hardware today.

| Board | Uplinks | BLE | Role |
|-------|---------|-----|------|
| `frdm_rw612` | USB-HS + Ethernet(+PTP) | ✅ multi-peripheral | the real target |
| `teensy41`   | USB-HS + Ethernet(+PTP) | ✗ no radio | **wired test system** — native IP stack vs the W6300's hardware offload |
| `teensy40`   | USB-HS only | ✗ no radio | **USB test system** — USB-HS vs the Pico 2's USB-FS |

Board differences live entirely in `boards/<board>.{overlay,conf}`: two devicetree
aliases (`box-gpio-port`, `box-pulse-counter`) point the platform layer at each
SoC's GPIO controller and hardware counter, and per-board `.conf` files gate the
networking and Bluetooth subsystems. No `#ifdef`s in the transport code — the
uplink arbiter simply has one candidate instead of two on a USB-only board.

**Caveat:** the RT1062 is a Cortex-M7 @600 MHz vs the RW612's M33 @260 MHz, so
Teensy numbers are a *functional* validation and a second data point, **not** a
performance proxy for the RW612.

## What's the same, what's new

The **portable core** is forked verbatim from `wiznet-io/`, with the leaked
`pico_*` symbol names renamed to `box_*`. The wire contract is unchanged: the
128-byte dserv datapoint frame, the `config/*`/`cmd/*`/`state/*` datapoint keys,
`BOX_CLASS "extio"`, and the frozen `d5e7000x` BLE GATT UUIDs (BLE.md) all carry
over byte-for-byte, so an RW612 hub interoperates with existing hosts and Nordic
peripherals. Only C identifiers changed.

| Path | What | Status |
|------|------|--------|
| `src/core/dserv_msg.h`      | Portable 128-byte dserv codec + stream framer | forked, unchanged |
| `src/core/dserv_config.h`   | Datapoint dispatch → `box_config_t` (wire keys frozen) | forked, `pico_*`→`box_*` |
| `src/core/dserv_ble.h`      | BLE frame contract | forked |
| `src/core/box_persist.h`    | Versioned + CRC config blob (was `pico_persist.h`) | forked |
| `src/core/box_cli.h`        | Bootstrap/recovery CLI (was `pico_cli.h`) | forked |
| `src/core/box_clock.h`      | Clock/sync helpers (was `pico_clock.h`) | forked |
| `src/core/box_group.h`      | DI chord groups (was `pico_group.h`) | forked |
| `src/core/box_ain_group.h`  | Analog groups (was `pico_ain_group.h`) | forked |
| `src/main.c`                | Zephyr entry — core smoke test + GPIO hardware demo | block #2–3 |
| `src/platform/box_gpio.{h,c}` | GPIO via devicetree hsgpio0; box-timed pulse on a CTIMER counter; DI edge capture + debounce | block #3 |
| `src/platform/box_usbd.{h,c}` | USB device (device_next) context; registers CDC-ACM classes, HS+FS configs | **block #4** |
| `src/platform/box_net_usb.{h,c}` | CDC-ACM 128-byte frame transport (data pipe) + console handle | block #4 |
| `src/platform/box_net_eth.{h,c}` | Ethernet transport: DHCP + TCP socket to dserv, 128-byte frames | **block #5** |
| `src/platform/box_ptp.{h,c}` | ENET IEEE-1588 hardware clock access (sub-µs sync enabler) | block #5 |
| `src/platform/box_uplink.{h,c}` | Uplink arbiter: carrier/strap/mode selection over USB+Eth (TRANSPORT.md) | **block #6** |
| `src/platform/box_ble.{h,c}` | Multi-peripheral BLE central (frozen d5e7000x pipe) — ingress → uplink | **block #6** |
| `boards/frdm_rw612.overlay` | Enables ctimer1 (pulse), two CDC-ACM instances, and the ENET PTP clock | block #3–5 |
| `tools/box_sim.c`           | POSIX sim/selftest (was `wiznet-io/sim/pico_sim.c`) | forked, passing |

## Building blocks (roadmap)

> For what the Pico box does that this port does **not** yet — the remaining
> feature gap, the open bugs, and a specific lead on `ptp_ready=0` — see
> **[PORTING.md](PORTING.md)**.

1. **[done] Fork + clean the core** — `pico_*`→`box_*`, verified by the forked
   `tools/box_sim.c --selftest` (compiles with plain `cc`, selftest passes).
2. **[done] Core under Zephyr** — `src/main.c` smoke test cross-builds clean for
   `frdm_rw612` (`zephyr.elf`, ~38 KB flash), proving the `arm-zephyr-eabi`
   toolchain + verbatim core port. On-silicon run pending the board. (`native_sim`
   is Linux-only, so host testing stays on the plain-`cc` `tools/box_sim.c`.)
3. **[done] GPIO platform layer (`box_gpio`)** — DO/DI via devicetree hsgpio0,
   box-timed pulse on a hardware CTIMER (Zephyr `counter` API, ctimer1), DI edge
   capture + debounce. Cross-builds for frdm_rw612 (`zephyr.elf`, ~44 KB); the
   demo pulses the user LED and reads SW2. On-silicon run pending the board.
4. **[done] USB-HS CDC transport** — device_next stack on the HS EHCI controller
   (preserves the 40× headroom), a CDC-ACM binary frame pipe (data + config, one
   pipe) + a CDC-ACM console, interrupt-driven UART + ring buffers. `main.c` is
   now a converged service loop: inbound frames → dispatch → GPIO; settled DI +
   1 Hz watchdog → host. Builds frdm_rw612 (`zephyr.elf`, ~71 KB). On-silicon E2E
   (host `modules/usbio` + enumeration/throughput) pending the board. The frame
   pipe can move to raw vendor bulk later without touching box logic (BENCH_NXP D1/D2).
5. **[done] Ethernet transport + hardware PTP** — Zephyr native IP stack over the
   ENET MAC + KSZ8081 PHY (RMII), DHCP + TCP socket to dserv (`box_net_eth`), and
   the ENET IEEE-1588 hardware clock exposed via `box_ptp` (the sub-µs sync
   enabler the W6300 couldn't provide). Builds frdm_rw612 (`zephyr.elf`, ~156 KB —
   the native-stack cost vs the W6300's hardware offload). `main.c` brings up
   DHCP + reports IP/link/PTP. On-silicon (lease, link, real PTP time) pending the
   board. Wiring HW packet timestamps into the sync anchor = follow-on (BENCH_NXP D3).
- **[design done] Transport arbitration** — [TRANSPORT.md](TRANSPORT.md): one
   authoritative uplink (USB/Eth/Wi-Fi) chosen by the config `transport_mode`,
   BLE + local I/O as *ingress* feeding the active uplink, USB console always-on.
   A `box_uplink_if` vtable + arbiter; BLE (block #6) plugs into the publish path,
   not the arbiter.
6. **[done] Transport arbiter + BLE ingress** — `box_uplink` (carrier-detect
   Ethernet-else-USB + strap override + always-on console) and `box_ble`, a
   **multi-peripheral** central (up to `BT_MAX_CONN` clients over the frozen
   `d5e7000x` pipe, bonding, whole-frame notify → the active uplink). `main.c` is
   the full converged loop: arbitrate uplink; USB/Eth/BLE/GPIO all funnel to
   dserv. Builds frdm_rw612 (`zephyr.elf`, ~471 KB *with the NXP BT controller
   firmware blob*). On-silicon (BLE scan/connect/fleet-ceiling, failover) pending
   the board. **Requires `west blobs fetch hal_nxp`** (accept NXP's firmware
   license) for the controller image — a blob-less build compiles but can't run BT.
7. Analog: GAU GPADC vs external MCP3204 (BENCH_NXP.md D10, RF-coupling check).

## Build

Toolchain: Zephyr (mainline) + `arm-zephyr-eabi` SDK, installed under
`~/zephyrproject` via `uv`. Activate and build freestanding:

```sh
source ~/zephyrproject/.venv/bin/activate
export ZEPHYR_BASE=~/zephyrproject/zephyr
cd /Users/sheinb/src/dserv/extio-zephyr

west build -b frdm_rw612 . --pristine    # the target
west flash                               # via the on-board MCU-Link

west build -b teensy41 . --pristine      # wired test system (add a magjack)
west build -b teensy40 . --pristine      # USB-only test system
west flash                               # teensy runner; press the board's
                                         # program button when it waits
```

Teensy flashing needs `teensy_loader_cli` (`brew install teensy_loader_cli`);
the build emits `build/zephyr/zephyr.hex`, which is what the loader wants.
BLE on `frdm_rw612` additionally needs `west blobs fetch hal_nxp` (accept NXP's
controller-firmware license).

`native_sim` (host-runnable `main.c`) requires Linux — Zephyr's POSIX arch
refuses macOS. On macOS use `tools/box_sim.c` below for host testing.

## Console status — USB identity MUST match, or dserv eats the console

The USB console is fine. It looked catastrophically broken for a day — shredded
output, mangled ANSI escapes, truncated `show`, terminals dropping — on **both**
macOS and Linux. **The cause was a second process on the port: dserv's own
`extio` subprocess.** Nothing was wrong with the CDC stack, the shell, the
buffers, or the firmware.

The chain, worth understanding because it is a trap for any new box:

1. The box advertised USB product string `"extio box"`.
2. The dserv host module (`config/extioconf.tcl`, `extio_find_data_port`) matches
   the box identity **exactly**: `$product eq "extio USB box"`. Ours failed.
3. On macOS it then falls back to a heuristic — highest `cu.usbmodem` — which is
   the **console** CDC, not the data pipe.
4. `extio_service` opened it, read it, and reopened it on a 2 s hot-swap poll.
   Stolen bytes look exactly like shredding; the reopen looks exactly like a
   disconnect.

Linux reproduced it for the same reason from the other end: the by-id glob is
`*extio*if02*`, and our two CDC instances were declared in the wrong order, so
`if02` was the console.

**Two invariants keep this fixed. Do not break either:**

* **Product string is exactly `"extio USB box"`** (`src/platform/box_usbd.c`).
  The host module deliberately opens nothing it cannot positively identify —
  that strictness is why a stray heuristic once opened a juicer pump.
* **CDC order is console FIRST, data SECOND** in every `boards/*.overlay`.
  Instance order sets USB interface numbering; the host expects the data pipe at
  `if02` (Linux) / the `*3` tty (macOS).

Diagnosing a repeat: `lsof /dev/cu.usbmodem*` (macOS) or `lsof /dev/ttyACM*`
(Linux) while a terminal is open. A second holder is the answer. Killing dserv
and watching the console go clean is the decisive test — do that **first**, well
before suspecting the USB stack.

Blind alleys from that day, recorded so nobody re-walks them: the shell backend's
8-byte TX ring and 30-byte print chunk, `USBD_CDC_ACM_WORKQUEUE`, the two-CDC
split, and hand-rolled-console-vs-Shell. All were changed and reverted; none
mattered. The stock `samples/subsys/shell/shell_module` built for `teensy41` also
"failed", which looked like proof of an upstream bug — it was not: with a single
CDC port, dserv's fallback grabbed **the shell itself**. That test ruled out our
firmware, never the host.

One override that IS real: do **not** set `USBD_CDC_ACM_TX_DELAY_MS=0` — see the
app `Kconfig`. It removes the window a host tty needs to turn ECHO off, so the
banner is reflected into our own RX and the console answers itself.

## Host-side core check (no Zephyr)

The forked core still compiles and self-tests with a plain C compiler, exactly
like the wiznet-io sim — the fastest inner loop:

```sh
cc -O2 -Wall -Isrc/core -o /tmp/box_sim tools/box_sim.c
/tmp/box_sim --selftest
```
