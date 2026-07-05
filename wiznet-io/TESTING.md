# wiznet-io — testing status & checklist

Where the box firmware stands, and what still needs validation on real hardware.
Everything below **builds clean** and passes the host tests (`host/*`, `sim/pico_sim --selftest`)
unless noted. Items marked **[HW?]** are compile-verified but **not yet exercised on silicon**.

## Build targets (one image per board)

```
sh build.sh w6300                                  -> dist/wizchip_dserv_config_w6300.uf2      (W6300 wired)
WIFI_SSID=.. WIFI_PASSWORD=.. sh build.sh pico2w      -> ..._pico2w.uf2       (Raspberry Pi Pico 2 W)
WIFI_SSID=.. WIFI_PASSWORD=.. sh build.sh picoplus2w  -> ..._picoplus2w.uf2   (Pimoroni Pico Plus 2 W)
WIFI_SSID=.. WIFI_PASSWORD=.. sh build.sh thingplus   -> ..._thingplus.uf2    (SparkFun Thing Plus, +fuel gauge)
sh build.sh usb                                    -> ..._usb.uf2         (plain Pico 2, USB-CDC to host modules/usbio)
```
WiFi creds are now **runtime** (`wifi ssid` / `wifi pass` over USB); the `-DWIFI_*` bake is only a fallback,
so the WiFi UF2s are generic. Config is forward-compatible across firmware versions (reflash keeps settings).

The **usb** target is a plain Pico 2 (no NIC): it speaks the same 128-byte frames over a TinyUSB CDC data
interface to the host dserv module `modules/usbio` (no IP, no self-registration — the module owns forwarding).
It owns TinyUSB, so `pico_stdio_usb` is off and the CLI/printf go to **UART** on that build. Dev/test transport
only (USB-CDC is a middle latency tier). Develop/validate the module against `sim/pico_sim --pty` before flashing.
Status: firmware + host pieces compile-designed but **not yet built on the ARM toolchain or run on silicon** — see below.

## Validated on hardware (prior + this session)

- **W6300 wired**: DHCP/static, self-registration, DO (level + box-timed pulse), debounced+timestamped DI,
  state publishing, autonomy across box/dserv restart, USB-CDC CLI, flash persist, unique board-id MAC.
- **pico2w / Pimoroni Plus 2 W (WiFi)**: cyw43 join (2.4 GHz only — CYW43439), DHCP, self-registration,
  `state/watchdog` flowing to an rpi4 dserv, runtime WiFi creds. *(First real lwIP-backend run.)*
- **extio/ namespace + `ess/in_obs` clock sync**: box publishes under `extio/<name>/…`; DI edges land on
  the dserv timeline via the snapped offset (`state/sync/*` audit trail).

## Needs hardware validation  [HW?]

1. **Non-blocking DO pulse** (`pico_gpio.h`) — `cmd/do/<n>/pulse_us` now raises + drops via a timer alarm
   instead of `busy_wait`. Verify: fire a long pulse, confirm DI/loop stay responsive; scope the width.
2. **Scheduled output** (`cmd/do/<n>/at <us>`, `cmd/timer/<n>/at <us>`) + `ess::box_schedule_pulse <dev> <pin> <ms>` /
   `box_schedule_timer`. Fires at `beginobs + delta` on the box clock. Verify: sync a box, `box_schedule_pulse dev 5 200`
   mid-obs, scope the pulse vs intended `now+200ms`, watch `state/timer/5` land.
3. **WiFi reconnect + dserv-restart survival** (`box_net_lwip.h`) — Verify: restart dserv (box re-registers,
   watchdog resumes); drop/return the AP (console prints `wifi: link down… link back up`, re-registers).
4. **ADS1115 analog-in** (`pico_ain.h`, runtime `ain enable 1`) — 2 ch, live `ain rate`/`ain gain`. I2C on
   GP4/5 (w6300) or GP6/7 (Thing Plus). Verify: `ain: ADS1115 found` at boot; `state/ain/0|1` tracks a pot.
5. **MAX17048 battery** (Thing Plus, `state/battery`) — Verify: `fuel: MAX17048 found` at boot; `state/battery` %.
6. **WiFi power-save** (`wifi pm 1`) — Verify: lower current draw; RT timestamps unaffected (edge-captured).
7. **obs-mirror LED** (`obs pin N`) — Verify: pin follows `ess/in_obs`; scope vs the rig's obs line for Path-A latency.
8. **active_low DI** (`pin N active_low 1`) — Verify: press reads 1 (logical), matching local `ACTIVE_LOW`.
9. **lwIP inbound config-push** — the dserv→box listener on WiFi (`:5010`). Verify: `dservSet extio/<n>/config/pin/5/mode out`
   takes effect; a box button via `button_bind` in `post-pins.tcl`.
10. **USB-CDC backend** (`box_net_usb.h` + `modules/usbio`, plain Pico 2).
    - [x] (a) `sim/pico_sim --pty` + `usbioOpen /dev/pts/N` → watchdog climbs (M1) and `dservSet ess/in_obs` echoes
      `state/sync/*` with the in_obs timestamp preserved (M2) — validated on a live dserv, no Pico. `modules/usbio`
      binary framing + `usbioSendFrame` done; `sh build.sh usb` compiles clean (216KB UF2).
    - [x] (b) on silicon — W6300-EVB running `usb.uf2` (W6300 ignored), data on CDC1 (`/dev/cu.usbmodem*3`):
      M1 watchdog/uptime/link climb, M2 sync round-trip 4/4 edges match, offset self-consistent. Fixed 3 bring-up
      bugs: `have_dserv_target()` IP gate (USB has no IP → publish block was skipped), DTR→`tud_ready`,
      and `modules/usbio` interruptible reader (blocking read + close wedged the interp — probe a silent port safely now).
    - [x] (c) on silicon (macbook board, GP1->GP2 jumper): DI button events over USB; config-push over USB re-applies
      pin modes (CFG_PIN_MODE -> pico_gpio_apply_config); scheduled pulse Tier C = timer/1 on target (0 us logical),
      looped di/2 falling at +30 ms pulse. (d) dual-CDC USB console via box_net_usb_console_init() (stdio driver on
      CDC0): `show`/CLI work over USB like the other firmwares; console = lower port, data = higher port.

## ESS integration (host side, `lib/ess-2.0.tm`)

- `button_bind <chan> {} box {<dev> <pin>}` in `local/post-pins.tcl` binds a logical button to a box DI line
  (overrides any protocol's `button_init`). Configure the box pin `in_pullup` + `active_low 1` + `debounce`.
- `box_schedule_pulse` / `box_schedule_timer` — see (2) above.

## Notes / gotchas

- CYW43439 is **2.4 GHz only** — use a 2.4 GHz (ideally dedicated IoT) SSID, and **`save`** creds to flash.
- `show` `net.ip` is the *configured static* field; use `ip` for the live/leased address.
- Config pins to avoid: W6300 = GP15–22; pico2w/Thing Plus = the CYW43 pins (23/24/25/29) — the firmware refuses them.
- `build.sh picoplus2w`/etc. no longer clobber each other (target-suffixed dist names).
