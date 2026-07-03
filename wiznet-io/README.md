# wiznet-io — W6300 + RP2350 Ethernet I/O extension for dserv

Firmware and host tooling for a modular Ethernet I/O box: a **WIZnet W6300**
(hardwired dual-stack TCP/IP, 8 sockets, QSPI) on an **RP2350 (Pico 2)**,
letting any NIC-equipped host (macOS dev, x86_64 rigs without GPIO) drive
digital I/O over Ethernet and receive locally-timestamped input events.

Board: **W6300-EVB-Pico2**. Debugger: **SEGGER J-Link EDU** (SWD + RTT).

## Layout

| Path | What |
|------|------|
| `pico/wizchip_udp_do.c`   | Firmware: UDP command → set DO, replies with on-box `t_rx`/`t_tx` timestamps |
| `pico/wizchip_dserv_wdt.c`| Firmware: TCP client pushing a watchdog/heartbeat datapoint into dserv |
| `pico/wizchip_dserv_config.c`| Firmware: TCP server receiving `pico/config/*` datapoints from dserv |
| `pico/CMakeLists.txt`     | Drop-in example CMake for WIZnet-PICO-C (builds all three firmwares) |
| `common/dserv_msg.h`      | **Portable dserv 128-byte codec**: builder (TX) + parser + stream framer (RX) |
| `common/dserv_config.h`   | Hardware-independent `pico/config/*` dispatch → `pico_config_t` |
| `common/pico_persist.h`   | Portable versioned + CRC-checked serialize/validate of `pico_config_t` |
| `common/pico_cli.h`       | Portable text CLI (bootstrap/recovery command set) |
| `pico/pico_flash.h`       | RP2350 flash backend for persistence (last-sector, `flash_range_program`) |
| `pico/pico_gpio.h`        | RP2350 GPIO layer: apply pin modes + execute set/pulse commands |
| `net/box_net.h`           | Transport seam — selects backend at build time |
| `net/box_net_w6300.h`     | Wired W6300 backend (ioLibrary) — **verified** |
| `net/box_net_lwip.h`      | Pico 2 W WiFi backend (CYW43 + lwIP) — real code, **needs board to validate** |
| `net/lwipopts.h`, `net/wifi_config.h` | lwIP + WiFi config for the Pico 2 W build |
| `sim/pico_sim.c`          | **Standalone box simulator (macOS + Linux)** — real codec over POSIX sockets; file-backed persist + `--cli` |
| `host/udp_rtt.c`          | **Portable RTT probe (macOS + Linux)** — the primary dev loop |
| `host/udp_do_send.c`      | RPi GPIO-trigger sender for the scope/Digilent ground-truth test |
| `host/dserv_msg_test.c`   | Round-trip test: builds frames, parses them like dserv's `'>'` handler |
| `host/dserv_rx_test.c`    | RX test: build → stream in awkward chunks → framer → parse → config dispatch |
| `host/persist_cli_test.c` | Test: persist serialize/validate/crc + CLI command parsing |
| `host/dserv_bench.c`      | Benchmark dserv's TCP pub/sub RTT + throughput (localhost) |

## dserv 128-byte binary datapoint message

The reusable format (`common/dserv_msg.h`) matches `src/Datapoint.c dpoint_to_binary`
+ the `'>'` handler in `src/Dataserver.cpp` — the same path the Teensy and
eye-tracker use to pump datapoints in over TCP. Fixed 128-byte frame, all
little-endian, always sent in full:

```
0    '>'                    1  byte   DPOINT_BINARY_MSG_CHAR
1    varlen                 u16       length of name (no NUL)
3    name                   varlen
..   timestamp              u64       0 => dserv stamps now() on arrival
..   datatype               u32       ds_datatype_t (INT=5, FLOAT=2, INT64=16, ...)
..   datalen                u32
..   data                   datalen
..   zero pad to byte 127
```

Budget: `varlen + datalen <= 109`. Connect a TCP socket to dserv (`localhost:4620`)
and write all 128 bytes. Verified byte-exact by `host/dserv_msg_test.c`.

## Configuring the box from dserv (config as datapoints)

Config rides the same 128-byte framing, in reverse: the host `dservSet`s
`<name>/config/*` datapoints and dserv **pushes** them to the box, which listens,
reassembles the stream, parses, and applies (`common/dserv_config.h`). No Pico-side
subscription and no new protocol.

**Configurable device name (prefix).** Every datapoint the box owns lives under a
persisted device name (default `pico`) so multiple boxes coexist on one dserv —
`io1/...`, `rig2/...`, etc. Set it over the USB CLI (`name io1`) or the datapoint
`<name>/config/name`. The box only acts on its own namespace (a mismatched prefix
is ignored). Register each box's matches accordingly, e.g. `io1/config/*` +
`io1/gpio/*`.

**GPIO commands (`<name>/gpio/*`, transient — not persisted):**
- `dservSet <name>/gpio/<n> 1|0` — drive pin level
- `dservSet <name>/gpio/<n>/pulse_us 500` — box-timed high pulse (width immune to
  host/dserv jitter; `busy_wait_us` on device)

This is the **first-pass DO path**: a datapoint command, reusing the whole config
channel. The benchmark showed dserv adds ~9 us, so it's plenty for bring-up; the
direct-UDP lane stays reserved for measured, loss-driven reasons. Pin modes
(`<name>/config/pin/<n>/mode out|in|in_pullup|off`, word *or* int) are applied to
real pins by `pico/pico_gpio.h` at boot and on change (GPIO15-22 refused: W6300).

Tell dserv to relay in **binary** (the `1`) to the box's listen port:

```
%reg <box_ip> <box_port> 1                              # register binary send-client
```
or via qpcs in one call (registers binary mirror + match):
```
dsMirrorAddMatch <server> <box_ip> <box_port> pico/config/[star]
```

Then, e.g.: `dservSet pico/config/pin/5/mode 1`, `dservSet pico/config/dserv/ip
"192.168.11.1"`, `dservSet pico/config/save 1`. Keys are in `dserv_config.h`.

**Value typing (verified against live dserv):** a bare `dservSet name val` stores
the value as a **STRING** (ASCII, e.g. `4620` arrives as type=1, 4 bytes `"4620"`,
no trailing NUL), whereas a typed C/Python setter can send a real int. So the box
accepts **both** — `dserv_msg_as_long()` parses a STRING or reads a numeric type —
and never relies on NUL termination (dserv's `SendClient` does **not** zero-pad the
128-byte frame; parse strictly by `datalen`). Also: dserv's binary send silently
drops any datapoint whose `varname+data > 109`, so keep config within budget.

**Verified live** (dserv on localhost:4620, driven by `dservctl`):
```sh
cc -O2 -Wall -Icommon -o /tmp/pico_sim sim/pico_sim.c
/tmp/pico_sim --listen 5010 &                     # box config channel
printf '%%reg 127.0.0.1 5010 1\n'         | nc -w1 127.0.0.1 4620   # binary mirror
printf '%%match 127.0.0.1 5010 pico/config/* 1\n' | nc -w1 127.0.0.1 4620
dservctl -c "dservSet pico/config/dserv/ip 192.168.11.1"   # -> box applies 192.168.11.1
dservctl -c "dservSet pico/config/dserv/port 4620"         # -> box applies 4620
# reverse: box -> dserv telemetry
/tmp/pico_sim --watchdog 127.0.0.1 4620 &
dservctl -c "dservGet pico/watchdog"                       # climbs 0,1,2,...
```

## Persistence + bootstrap/recovery

Config survives reboot: `pico/config/save` (or the CLI `save`) serializes
`pico_config_t` into a versioned, CRC-checked ~180-byte blob and writes it —
to the **last flash sector** on the RP2350 (`pico/pico_flash.h`) or to a **file**
in the simulator. On boot the box loads it *before* bringing up the network, so a
saved `net/ip` takes effect. A bad/absent/foreign blob is rejected (magic+version
+CRC) and falls back to defaults.

**USB-CDC CLI** (`common/pico_cli.h`) is the out-of-band channel — set the box's
network identity before it's reachable, and factory-reset a mis-configured box.
Same command set on device (USB-CDC) and sim (`--cli`):

```
help | show | net ip A.B.C.D | dserv ip A.B.C.D | dserv port N
pin N mode out|in|in_pullup|off | pin N pulse US | save | factory | reboot
```

Verified on the Mac (survives a simulated power cycle):
```sh
printf 'net ip 192.168.11.50\ndserv ip 192.168.11.1\npin 5 mode out\nsave\n' | ./pico_sim --cli
printf 'show\n' | ./pico_sim --cli          # -> config still there
```

## Build targets: W6300 (wired) vs Pico 2 W (WiFi)

The box firmware is transport-agnostic — everything above `net/box_net.h`
(codec, dispatch, config, persist, CLI, GPIO) is identical across both, and is
the same code the POSIX simulator runs. Pick the backend at build time:

```sh
# wired W6300 (default) — VERIFIED
ninja wizchip_dserv_config

# Pico 2 W / WiFi — real code, not yet hardware-validated
cmake .. -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBOX_TARGET=pico2w -DPICO_BOARD=pico2_w \
    -DWIFI_SSID="myssid" -DWIFI_PASSWORD="mypass"
ninja wizchip_dserv_config
```

`-DBOX_TARGET=pico2w` swaps `box_net_w6300.h` (ioLibrary) for `box_net_lwip.h`
(CYW43 + lwIP, poll mode) and links `pico_cyw43_arch_lwip_poll` instead of the
WIZnet libs. The W6300-only test firmwares (`wizchip_udp_do`, `wizchip_dserv_wdt`)
are skipped for the WiFi target.

The Pico 2 W deps are NOT a separate download — `pico_cyw43_arch` ships in the
pico-sdk, and its `cyw43-driver` (WiFi firmware blob) + `lwip` submodules come
with a recursive clone. If a fresh clone is missing them:
`cd libraries/pico-sdk && git submodule update --init --recursive`.

Notes / status:
- **W6300 path: verified** (compiles + links clean, exercised end-to-end against
  live dserv via the simulator).
- **Pico 2 W path: compiles + links clean for `pico2_w`** on macOS (full CYW43 +
  lwIP stack, 774 KB UF2 with embedded WiFi firmware; our backend warning-free).
  Only the **runtime** is unvalidated — WiFi join + the raw-API listener callbacks
  need the actual board. dserv, the protocol, and all logic layers are unchanged.
- Supply real creds at build time (`-DWIFI_SSID`/`-DWIFI_PASSWORD`); don't commit them.
- WiFi adds latency/jitter (fine for functional/end-to-end testing, not the
  latency experiments). DI events stay accurate anyway — they're timestamped at
  capture on the box, so jitter delays arrival but not the recorded timing.
- A few GPIOs are consumed by the CYW43 on Pico 2 W — check its pinout before
  assigning DO/DI pins (in addition to the W6300's 15-22 on the wired board).

## Transport benchmark & UDP decision

dserv datapoints are TCP-only. Before deciding whether the fast control path
needs UDP (a new dserv Tcl-C send-client module) or the bespoke direct-UDP lane,
measure dserv's pub/sub overhead. `host/dserv_bench.c` sets a datapoint via the
`'>'` path and times its arrival back through a binary subscription (one process,
one clock). Results on localhost (Mac, 5000 samples), with raw-socket floors:

```
raw UDP loopback RTT   min 10  med 13  p99 69  max 150 us
raw TCP loopback RTT   min  8  med 13  p99 20  max  98 us   (NODELAY)
dserv pub/sub RTT      min 16  med 22  p99 80  max 271 us   (415k datapoints/s)
```

Findings:
- **dserv adds only ~9 us (median) over a raw socket** — notify + match + send-
  client thread wakeup. Negligible; throughput (415k dp/s) dwarfs any rig rate.
- **UDP gives no latency win here** — on this stack raw UDP's tail (p99 69) is
  *worse* than TCP's (p99 20). So a dserv UDP module would save at most ~9 us and
  would not beat TCP at the socket level. Not worth building for latency.
- Real box RTT = this dserv floor **+ W6300 QSPI + PHY + wire**, which will
  dominate (measured separately with `udp_rtt` + scope on hardware).

**Decision:** config/telemetry/events ride dserv datapoints over TCP — viable and
fast. Keep the direct-UDP DO lane (`wizchip_udp_do.c`) only for its real-network
advantages (supersession + no TCP RTO spike under packet loss), and A/B it against
the TCP-datapoint path on real hardware before committing. Build a dserv UDP
module only if the real link shows TCP retransmit tails under load. (If built: a
SOCK_DGRAM send-client where each datagram = one 128-byte frame — the box skips
the stream framer since recvfrom yields a whole frame.)

## Simulator — run the box on your Mac (no hardware)

`sim/pico_sim.c` compiles the **same** `dserv_msg.h` + `dserv_config.h` the
firmware uses, with POSIX sockets instead of the W6300. Develop and test the
whole protocol against a real dserv before the board exists.

```sh
cc -O2 -Wall -Icommon -o pico_sim sim/pico_sim.c

./pico_sim --selftest                 # offline: build -> frame -> apply
./pico_sim --listen 5010              # config channel; point dserv here (%reg ... 1)
./pico_sim --send-config <ip> 5010    # inject a config set without dserv (bench)
./pico_sim --watchdog 127.0.0.1       # push heartbeats to dserv:4620
```

Verified: `--send-config` → `--listen` over a real TCP socket reassembles the
stream and applies config end-to-end.

## Wiring facts (W6300-EVB-Pico2)

The W6300 is driven over **QSPI via the RP2350 PIO** and consumes **GPIO15–22**:
INT=15, CS=16, SCK=17, IO0–IO3=18–21, RST=22. **Keep application I/O pins out
of 15–22.** The test DO pin is **GPIO6** (`DO_PIN` in the firmware).

## Latency test strategy

Two complementary measurements — see the design discussion; short version:

1. **RTT probe (primary, no hardware).** `udp_rtt` sends a command and times the
   reply, and the firmware self-reports its own turnaround. Decomposition:
   - `rtt`       = host stopwatch, full round trip
   - `device`    = `t_tx − t_rx`, box turnaround, **free of host jitter**
   - `net+host`  = `rtt − device`, both host stacks + two wire trips

   Take **min** as the floor; **p99/max** is the jitter story. Run it from
   macOS, Linux, and the RPi to compare hosts.

2. **Scope ground-truth (Digilent), run once to calibrate.** `udp_do_send`
   toggles an RPi trigger GPIO high immediately before `sendto()`; scope
   trigger-rising → `DO_PIN`-rising is the true **actuation** latency. Use it to
   validate the firmware's self-reported timing, then trust the software number.

## 8-socket map (design target)

The W6300's 8 hardware sockets let us split traffic by class — chosen by
**what reliability model fits**, not by convenience. TCP is *not* extra MCU work
(the chip runs the state machine in hardware); the tradeoff is latency-under-loss
vs. guaranteed delivery.

| Socket | Proto | Traffic | Why |
|--------|-------|---------|-----|
| 0 | UDP | **DO commands** (fast path) | Latency matters; reliability via *supersession* (newer cmd replaces stale). Seq # to *detect* loss, not prevent. |
| 1 | UDP | DO commands, spare / multi-point | headroom for the fast path |
| 2 | TCP | **DI event reports** | Events are locally timestamped → arrival latency irrelevant; **completeness + order** matter → TCP. Reuse dserv 128-byte framing. |
| 3 | TCP | **Config / capability discovery** | must-arrive, exactly-once |
| 4 | TCP | **Firmware update** | must-arrive, in-order |
| 5 | TCP | **Bulk log / telemetry pull** | flow-controlled stream |
| 6–7 | — | reserved | future channels |

Fast-path UDP; keep-every-event + control-plane on TCP. First bring-up uses
**socket 0 (UDP) only**; the map exists so the TCP channel is a config change,
not a rewrite. If anything latency-sensitive ever goes on TCP: set
**`TCP_NODELAY`** and remember a single lost packet triggers an RTO spike
(~200 ms floor) — the reason the command path stays on UDP.

## Build & flash (command-line, J-Link)

Firmware slots into WIZnet's example tree so we reuse their proven QSPI-PIO port.

```sh
# 1. clone WIZnet's Pico examples (bundles pico-sdk + ioLibrary_Driver)
git clone --recurse-submodules https://github.com/WIZnet-ioNIC/WIZnet-PICO-C.git
cd WIZnet-PICO-C

# 2. drop in this example -- BOTH the sources AND the shared headers, which the
#    firmware #includes and expects next to the .c files.
cp -r /Users/sheinb/src/dserv/wiznet-io/pico examples/wiznet_io
cp /Users/sheinb/src/dserv/wiznet-io/common/*.h examples/wiznet_io/
cp /Users/sheinb/src/dserv/wiznet-io/net/*.h    examples/wiznet_io/   # transport backends
echo 'add_subdirectory(wiznet_io)' >> examples/CMakeLists.txt

# 3. board + QSPI mode are ALREADY the defaults in the root CMakeLists.txt
#    (BOARD_NAME W6300_EVB_PICO2, QSPI_QUAD_MODE, PICO_PLATFORM rp2350).
#    Only touch it to change SCLK:  add_definitions(-D_WIZCHIP_SPI_SCLK_SPEED=40)

# 4. build (RP2350 / ARM Cortex-M33)  -- VERIFIED clean on macOS with
#    arm-none-eabi-gcc 14.3 + cmake 4.2 + ninja.
export PICO_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/14.3.rel1/arm-none-eabi
mkdir build && cd build
cmake .. -G Ninja \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \   # cmake 4.x vs. old submodule minimums
    -DPICO_BOARD=pico2
ninja wizchip_dserv_config    # the "box" -> examples/wiznet_io/wizchip_dserv_config.{elf,uf2}
# (or `ninja` to build all three: wizchip_dserv_config, wizchip_dserv_wdt, wizchip_udp_do)
```
Notes: the root CMakeLists.txt already defaults BOARD_NAME=W6300_EVB_PICO2 and
sets PICO_PLATFORM=rp2350 + QSPI_QUAD_MODE, so no root edits are needed. Point
PICO_TOOLCHAIN_PATH at 14.x — a stale arm-none-eabi-gcc 9.x in PATH will not do
RP2350. Our source compiles with zero warnings; the warnings you'll see are all
from WIZnet's bundled loopback.c/dhcp.c.

Flash + RTT over J-Link (update to the latest SEGGER pack for RP2350 support):

```sh
# OpenOCD path (from examples/wiznet_io/ in the build dir):
openocd -f interface/jlink.cfg -f target/rp2350.cfg \
        -c "program wizchip_dserv_config.elf verify reset exit"
# keep USB plugged in too: printf console + bootstrap CLI over USB-CDC
```

(UF2 fallback: hold BOOTSEL, copy `wizchip_dserv_config.uf2` to the RP2350 drive.)

## Run the RTT probe (from the Mac, today)

```sh
cc -O2 -o udp_rtt /Users/sheinb/src/dserv/wiznet-io/host/udp_rtt.c
./udp_rtt 192.168.11.2 5000 5000
```

Direct cable Mac↔EVB is fine (auto-MDIX). Pico is `192.168.11.2`; give the Mac
an address on `192.168.11.0/24`.

## Status / next

- [x] Folder, UDP command→timestamped-reply firmware, portable RTT probe, socket map
- [x] Firmware compiles + links clean for W6300-EVB-Pico2 / RP2350 (no board needed)
- [x] Portable dserv 128-byte message builder + watchdog TCP client (both compile;
      builder verified byte-exact vs dserv's parser)
- [x] RX side: parser + stream framer + `pico/config/*` dispatch; config-listener
      firmware compiles; verified over real sockets via the simulator
- [x] Simulator (`sim/pico_sim.c`) runs the real codec on macOS (selftest + live socket)
- [x] Verified live end-to-end against running dserv (localhost:4620) via dservctl:
      config datapoints -> box applies; watchdog -> dserv sees it climb. Surfaced +
      fixed string-value typing (dservSet stores STRING; box accepts str or numeric).
- [x] Flash persistence (RP2350 last-sector) + file-backed sim; versioned+CRC blob;
      config survives reboot (verified on sim, incl. network→persist→reload)
- [x] USB-CDC bootstrap/recovery CLI (device + sim `--cli`); factory reset
- [x] GPIO command path (`<name>/gpio/*` set + box-timed pulse) + device GPIO layer
      (`pico_gpio.h`); pin modes applied to real pins at boot/on-change
- [x] Configurable/persisted device name (prefix) — multiple boxes per dserv;
      verified live (box booted as `io1`, drove `io1/gpio/*`, ignored `pico/*`)
- [x] Transport seam (`net/box_net.h`): both backends **compile + link clean** —
      W6300 (verified e2e) and Pico 2 W lwIP (`-DBOX_TARGET=pico2w`, 774KB UF2)
- [ ] Validate Pico 2 W at RUNTIME on hardware (WiFi join + listener callbacks)
- [x] **Bring-up on real hardware DONE** — W6300 QSPI, DO/DI, flash persist, USB CLI,
      configurable name, all validated on the bench
- [x] **Fully autonomous box** — self-registers with dserv from saved flash config on
      every (re)connect; survives box reset AND dserv restart with zero manual steps
- [x] dserv robustness fixes (open_send_sock timeout, tcpip_register return, keepalive)
- [x] Per-pin DI debounce (`config/pin/<n>/debounce_ms`, Linux-like wait-for-stable,
      press-timestamped) — verified collapsing edge glitches on hardware
- [ ] Flash to board (J-Link/OpenOCD or UF2) — pending hardware (tomorrow)
- [ ] On-board: scope a `<name>/gpio/<n>` toggle + `pulse_us` width; confirm flash
      save/load survives a real power cycle
- [ ] First RTT numbers from macOS (functional bring-up)
- [ ] Confirm watchdog datapoint lands in dserv (localhost:4620) once on the bench
- [ ] Digilent scope calibration of `device` vs. real `DO_PIN` edge
- [ ] Full DI-event channel over the 128-byte framing (edges + local timestamps)
- [ ] Step 2: W6300 INT-driven RX (free the core) if desired
