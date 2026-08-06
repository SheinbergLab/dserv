# Cage-side control panel — 4D Systems gen4-RP2350

**Status: design, no code started (2026-08-05).** Parts on hand: `GEN4-RP2350-SK`
(bring-up/programming vehicle) and `GEN4-RP2350-32CT-CLB` (the deployment part).

## Why this exists

In-cage training boxes dock overnight for power + Ethernet and run untethered
during the day. Collaborators at institutions with restrictive wireless policies
need to interact with a running box while it is off the dock. The alternatives
were surveyed first; see the bottom of this file for why they lost.

The panel is the **twenty-times-a-day surface**: what is it running, is it
working, give a reward, pause, resume. It is deliberately NOT the configure-a-
system surface — that stays in `www/` over a cable or a SoftAP when someone sits
down with the box.

## The decision: this is a box, not a display

The panel joins the fleet as `extio/panel1`. It is an RP2350 running the same
firmware core as every other extio box, whose peripherals happen to be a screen
and a touch controller instead of GPIO pins and an ADC.

That is what makes it cheap. `common/` is ~1,500 LOC with no pico-sdk includes
and eight host test programs; `dserv_msg.h`, `dserv_config.h`, `pico_persist.h`,
`pico_cli.h`, `pico_clock.h` and `pico_group.h` all come across untouched, and
with them the manifest/announce burst, labels, OTA, watchdog, persist, and the
dual-core split. `modules/usbio` on the host already frames `'>'` dserv frames
off a serial device straight into `tclserver_set_point`, so the uplink needs no
host code at all.

## Hardware

The vendored pico-sdk 2.2.0 already ships the board header —
`.wiznet-pico-c/libraries/pico-sdk/src/boards/include/boards/gen4_rp2350_32ct.h`
— and it settles the CLB question outright:

```c
#define GEN4_RP2350_32CT	// CLB variants are exactly the same in operation
```

So `PICO_BOARD=gen4_rp2350_32ct` is correct for the `-32CT-CLB` part. Pin map,
free:

| what | pins |
|---|---|
| LCD, 16-bit 8080 parallel | RS 18, WR 19, RD 20, DATA0..15 = **21–36** |
| LCD reset / backlight | 37 / 17 |
| Cap touch (FT-class, 5 point) | `i2c1`, SCL 39, SDA 46, INT 38, RST 47 |
| SD (SDIO) | 10–15 |
| Panel | 240x320 portrait native, 3.2" |
| Silicon | RP2350**B** (`PICO_RP2350A 0`), 16 MB flash |

240x320x16bpp = **153.6 KB**, which fits in the RP2350's 520 KB SRAM with room to
spare, so a full framebuffer is viable and dirty-rect updates are an optimisation
rather than a requirement. A full blit over a 16-bit parallel bus via PIO+DMA is
single-digit milliseconds — comfortably inside the existing 250 ms core-0 service
tick.

## Identity — and the naming trap

`pico/usb_descriptors.c:27-33` picks identity off the *transport* define, not a
dedicated one:

```c
#ifdef BOX_NET_BLE
#define EXTIO_USB_PID     0x100C            /* dev PID for the BLE handheld */
#define EXTIO_USB_PRODUCT "dserv handheld"  /* NO "extio": host box-globs must not match */
#else
#define EXTIO_USB_PID     0x100B            /* dev PID for the extio USB box */
#define EXTIO_USB_PRODUCT "extio USB box"
#endif
```

Add a third arm: `0x100D` / **`"dserv panel"`**.

**The product string must not contain the substring `extio`.** The comment above
is load-bearing and the reason is asymmetric between platforms. macOS matching in
`config/extioconf.tcl:60` is an exact compare (`$product eq "extio USB box"`), so
an "extio panel" would be invisible there — but Linux matching at
`extioconf.tcl:99` is `glob /dev/serial/by-id/*extio*if02*`, which **would** grab
it, and `extio_find_data_port` has no arbitration when two devices match (it
takes the last one). The result is a panel that works on a Mac and silently
steals the real box's data port on a Pi. `"dserv handheld"` dodged this by
construction; `"dserv panel"` does the same.

Data CDC stays the `*3` tty (console is `*1`), per `box_tusb_config.h`
(`CFG_TUD_CDC 2`).

## Build target

`BOX_TARGET` is a closed set that `pico/CMakeLists.txt` branches on to pick the
transport backend — an unknown value silently falls through to the W6300 wired
build. So the panel rides `usb` and carries a feature flag:

```sh
  gen4panel)                                          # 4D Systems gen4-RP2350 3.2" CT panel: cage-side control surface
    BOARD=gen4_rp2350_32ct; VARIANT=panel
    FLAGS="-DBOX_TARGET=usb -DPICO_BOARD=$BOARD -DBOX_PANEL=1" ;;
```

`build.sh:105` already copies anything dropped in `pico/` into the build tree, so
only new `.c` files need a `target_sources()` line.

## Datapoint contract

### Up — panel to dserv

Touch buttons are published as **synthetic DI events**. `di_event_t` is just
`{pin, level, t_us}`, and feeding synthesised events into the existing core-1
publish loop inherits `di_logical` active-low mapping, chord groups, `group_quiet`
suppression, `event_stamp()` dserv-time conversion, manifest labels, and the
reconnect level-seeding in `publish_di_levels()` — all for free.

```
extio/panel1/state/di/<n>          int 0/1    button press / release
extio/panel1/state/group/<label>   int mask   chord, if ever wanted
extio/panel1/state/label/<n>       string     button caption (manifest)
extio/panel1/state/{fw,board,boot,transport,...}      free from publish_ident()
```

Note the leaf is `state/di/<n>`, not `di/<n>` — `dserv_state_name()` builds
`%s/%s/state/%s`.

Debounce is *not* reused: `pico_gpio.h`'s debounce is bound to real pins and IRQ
timestamps, and an FT-class controller debounces in silicon anyway. `pico_group.h`
**is** reused — it is explicitly hardware-free and host-tested, and takes caller
timestamps.

### Down — dserv to panel

Split by lifetime, which maps cleanly onto the two namespaces `dserv_dispatch`
already routes:

```
config/panel/label/<n>     persisted   button caption      -> pico_config_t, persist v22
config/panel/backlight     persisted   0-255
config/panel/blank_after   persisted   idle seconds before backlight off (battery)
cmd/panel/slot/<n>         volatile    live text for display slot n
cmd/panel/beep             volatile    acknowledge / attention
```

Live values are `cmd/`, not `config/`, because they must not hit flash. Extending
`dserv_cfg__cmd` is the documented four-step recipe (enum member, `strcmp` arm,
`dserv_cfg_result_str` name, `on_frame` arm), and `ble/pair`/`ble/forget` are the
precedent for keys parsed unconditionally in `common/` that are simply inert on
builds which don't handle them.

Roughly eight slots covers it: system/protocol/variant, state, obs count, percent
correct, battery, and a status line.

## Host side

A finder proc mirroring `extio_find_handheld_port` (`config/extioconf.tcl:728`)
exactly — the Linux glob keys on `panel`, macOS on the exact product string:

```tcl
# A docked panel's USB DATA port, by its "dserv panel" identity -- the same
# shape as extio_find_handheld_port. Data CDC = the *3 tty (console = *1).
proc extio_find_panel_port {} {
    if { $::tcl_platform(os) eq "Darwin" } {
        if { ![llength [glob -nocomplain /dev/cu.usbmodem*]] } { return "" }
        if { ![catch { exec ioreg -r -c IOUSBHostDevice -l -w0 } out] } {
            set product ""; set best ""
            foreach line [split $out \n] {
                if { [regexp {"USB Product Name" = "([^"]+)"} $line -> p] } {
                    set product $p
                } elseif { [regexp {"IODialinDevice" = "(/dev/tty\.usbmodem[^"]+)"} $line -> tty] } {
                    if { $product eq "dserv panel" && [string match {*3} $tty] } {
                        set best [string map {tty. cu.} $tty]
                    }
                }
            }
            if { $best ne "" } { return $best }
        }
        return ""                       ;# NO FALLBACK -- see extio_find_data_port
    }
    foreach link [lsort [glob -nocomplain /dev/serial/by-id/*panel*if02*]] {
        if { ![catch { file readlink $link } tgt] } {
            return [file normalize [file join [file dirname $link] $tgt]]
        }
    }
    return ""
}
```

Outbound mirroring is `dpointSetScript` on the datapoints of interest into a proc
that builds `cmd/panel/slot/<n>` frames — the same shape as `usbio_forward`
(`extioconf.tcl:109`). Button actions bind the other way:
`dpointSetScript extio/panel1/state/di/0 {...}` -> `ess::start`, and so on.

## What is actually new work

Everything above this line is assembly. The genuinely new pieces:

1. ~~**The TFT driver.**~~ **DONE — see Stage 1 below.** `pico/pico_panel.h`,
   bit-banged, and the measurements say PIO+DMA is not needed for this workload.
2. ~~**FT-class capacitive touch on `i2c1`.**~~ **DONE — see Stage 2 below.**
   `pico/pico_touch.h`, FocalTech FT5x46 at 0x38.
3. **Touch -> `di_event_t` synthesis on core 1**, or a cross-core queue in the
   shape of `g_ain_q`. `g_cfg` and the publish loop are core-1-owned.
4. **Extending `box_status_t`** past its current four DI pins. Keep the existing
   discipline: byte fields only, single writer on core 1, benign-stale reads on
   core 0 — never torn.
5. Persist bump to **v22** for the panel config fields.

### What is reused verbatim

The *placement* seam from the OLED is exactly right and should be copied
wholesale: a persisted enable flag -> boot-time pin claim before core 1's
`apply_config` (`pico_gpio_oled_claim`, so user pin modes cannot collide) -> a
self-rate-limited service on **core 0** reading a core-1 snapshot. Cosmetic
peripherals live on the housekeeping core and never touch the RT core. That
rationale (`pico_oled.h:1-19`) applies to a panel with more force, not less.

## Constraint to respect: PICO_NPINS 30

`dserv_config.h:41` caps configurable pins at 30 and `group_pins` is a `uint32_t`.
The gen4 is an RP2350B and every panel peripheral pin is above the ceiling —
LCD_RESET 37, touch 38/39/46/47, LCD data through 36.

**Do not widen the types.** Panel pins are firmware-fixed and must never appear in
config, labels, groups, or the manifest. Synthetic touch-button indices live in
the low range and are the only pin-like things the panel exposes. (`picoplus2w` is
also RP2350B and already lives with this ceiling.)

## Alternatives rejected

- **Second HDMI or DSI output.** stim2 has no way to choose an output —
  `glfwGetPrimaryMonitor()` in three places, and "primary" on Wayland is just
  whichever `wl_output` the compositor advertised first. Adding a connector makes
  which panel gets the stimulus registry-order roulette, and `ScreenWidthCm` and
  `FrameDuration` derive from that same handle, so a wrong pick silently corrupts
  visual angle and frame duration. On the Pi it also means replacing `cage`, which
  is a deliberately single-surface kiosk with no config file. And it runs against
  `docs/request_timing.md:272`, where the stated direction is to remove the
  compositor from the present path on trainers. A UART/USB panel is never a
  `wl_output`, never a `/dev/input` device, and never a GPU client.
- **Second touchscreen on the host.** `inputKnownDevice` matches by glob on the
  by-id or libevdev name; two touch panels risks the operator's taps and the
  animal's arriving on the same `mtouch/event`. Same failure class as the juicer
  wedge.
- **BLE console.** The BLE link is publish-only — `box_net_ble.h:27-29` has no
  `%reg`/`%match` and `send_command` is a no-op, and `extio-zephyr/TRANSPORT.md`
  states outright that BLE is a source, not an uplink. A console needs
  bidirectional request/reply plus a client app (no Web Bluetooth on iOS Safari).
- **Host-driven smart display (ViSi-Genie).** No firmware work, but you inherit
  someone else's widget model and write a host-side protocol adapter. Moot: the
  gen4-RP2350 is an RP2350 we program ourselves.

## Bring-up — confirmed on the bench 2026-08-05

**The `-32CT-CLB` is a self-contained USB device.** It has its own USB-C, powers
and enumerates straight off a host port, and runs a factory welcome demo. The
SK's 4D-UPA, ribbon and breakout are **not needed for the deployment part** —
they are for bench work on the breakout or if the 30-pin signals are ever wanted.

Factory identity as it presents today:

```
USB Vendor Name = "Raspberry Pi"   idVendor  = 0x2E8A
USB Product Name = "Pico"          idProduct = 0x0009
IODialinDevice  = /dev/tty.usbmodem1401        (ONE CDC)
```

Three things follow:

- **The factory image cannot collide with the fleet globs.** `"Pico"` fails the
  macOS exact compare, and a by-id name of `usb-Raspberry_Pi_Pico_*` has no
  `extio` substring for the Linux glob. A stock module plugged into a running rig
  is inert. PID 0x0009 also clears 0x100B/0x100C/0x100D.
- **Same VID we already borrow.** `usb_descriptors.c:45` uses 0x2E8A for every
  box, so our image only changes PID and product string.
- **The `*3` data-CDC convention falls out for free.** Factory is single-CDC
  (`...1401`, ends in 1). With `CFG_TUD_CDC 2` the module presents console
  `...x1` + data `...x3` like every other box, and `extio_find_panel_port` needs
  no special case.

**Flashing needs no button and no adapter.** The factory firmware exposes the
SDK's reset-via-vendor-interface, so picotool (v2.3.0, homebrew) reboots it into
BOOTSEL over USB:

```
$ picotool info
No accessible RP-series devices in BOOTSEL mode were found.
but:
RP2350 device at bus 0, address 6 appears to have a USB serial connection, so
    consider -f (or -F) to force reboot in order to run the command.
```

So the first flash is `sh build.sh gen4panel` -> `picotool load -f
dist/wizchip_dserv_config_gen4panel.uf2`. After that the existing A/B OTA path
takes over docked-USB with no picotool, exactly as `OTA.md` describes.

### Stage 0 — the box runs on gen4 hardware (VALIDATED 2026-08-05)

Built clean first try (`fw 0.50.3-dirty`, 305,152-byte uf2, no new warnings) and
runs. `show` over the console CDC:

```
[extio pico 0.50.3-dirty | transport=usb | boot=update | up 14s | 'help' for commands]
  transport=usb boot=update up=15s fw=0.50.3-dirty
  build=gen4panel board=gen4_rp2350_32ct channel=dev
  persist=FAILED magic=ffffffff ver=65535 len=65535 (want 57494f31 v<=21)
```

What this proves, and it is the whole "it's just a box" thesis:

- **RP2350B + 16 MB flash + copy_to_ram boots our image** with no board-specific
  changes. `BOX_TARGET=usb` rides straight onto the gen4.
- **Dual-CDC enumerates**: `usbmodem1401` (console) + `usbmodem1403` (data). The
  `*1`/`*3` convention `extio_find_panel_port` relies on falls out for free.
- **Identity is correct**: vendor `dserv`, product `dserv panel`, PID **0x100D**.
- **`board=gen4_rp2350_32ct` / `build=gen4panel`** are baked and reported, so the
  OTA/shelf compatibility key is right from day one.
- **The CLI `bootsel` recovery path works on this board** — verified by using it
  to get back into BOOTSEL for the second flash. This matters: our image sets
  `pico_enable_stdio_usb(0)`, so picotool's own `-f` reset-via-vendor-interface
  (which the 4D factory image *did* expose) is gone. `bootsel` over the console
  CDC, or `cmd/bootsel` as a datapoint, is now the way back.
- **`oled.en=0` by default**, so nothing drives SPI0 into the LCD pin range at
  boot. No conflict to design around yet.
- **`persist=FAILED magic=ffffffff` is correct, not a fault** — virgin flash at
  16 MB−4K is erased, the magic misses, and the CRC guard falls back to defaults.
  A `name panel1` + `save` writes the first valid blob.

One observation worth keeping: `up` read **3444s** immediately after the first
flash (a warm bootrom reboot) and **14s** after the BOOTSEL round-trip. So `up`
tracks time-since-cold-reset, not time-since-image-start — a warm reboot carries
the timer across. Another instance of the recurring "fields that report memory,
not reality" shape; don't read `up` as image uptime when diagnosing OTA boots.

Not yet done: no display driver, no touch driver, `BOX_PANEL` currently only
selects the USB identity.

## Stage 1 — display driver working (VALIDATED 2026-08-05)

`pico/pico_panel.h`. Colour bars confirmed on the glass; register read-back
confirms the write path independently of anyone looking at it.

### The controller is an ILI9341 — read off the chip, not guessed

4D do not publish the driver IC (Graphics4D abstracts it, and the resource-centre
page for the 32CT links only the RP2350 datasheet), so `panel id` asks the part
directly over the RD line:

```
  04: 0000 0000 0000 0000 0000
  D3: 0000 0000 0093 0041 0041     <- RDID4 = 0x93 0x41 -> ILI9341
  09: 0000 0000 0061 0000 0000
```

The same probe settled a second question for free: **8-bit values come back on
D0..D7**, not the high lane. So command parameters go out in the low byte and
only RAMWR pixel data uses all 16 lines. Guessing that wrong is a blank screen
with no diagnostic.

### Two things that bit, both worth remembering

**The data bus straddles the SIO bank boundary.** D0..D15 is GP21..GP36, so
D0..D10 are in the low bank and D11..D15 in the high one. Every access uses the
*64* GPIO API (`gpio_put_masked64` / `gpio_get_all64` / `gpio_set_dir_*64`),
which RP2350B has. The 32-bit calls compile fine and silently drive only the low
11 bits. This is also why 4D drive it from PIO — with `GPIOBASE=16` the 16 pins
are one contiguous field.

**`RDDCOLMOD` returning 0x05 when we wrote 0x55 is not a fault.** The register
splits: [6:4] is the RGB (DPI) interface format, [2:0] the MCU interface. This
module has no DPI video interface wired, so the RGB nibble is legitimately 0 and
only the MCU nibble echoes our 16 bpp. Compare bits [2:0] only. `RDDMADCTL` is
the honest read-write check and returns exactly the 0x48 we sent.

### Measured — and it retires the PIO+DMA plan

`panel bench`, on the actual board:

| operation | full screen (240x320, 76,800 px) |
|---|---|
| fill (constant colour — bus parked, strobe only) | **6.5 ms** |
| blit (arbitrary pixels — bus write + strobe each) | **17.1 ms** |

The design above assumed PIO+DMA was mandatory. **It is not.** The worst case
that exists for this device — repainting the entire screen with arbitrary pixels
— is 17 ms on core 0, against a 250 ms service tick: under 7% of the
housekeeping core, and that is the case a status panel never actually hits. A
realistic repaint is a few text fields of a few hundred pixels each, comfortably
under a millisecond.

So bit-banging ships. PIO+DMA stays available as a known optimisation if a later
need appears (animation, an image, a much larger panel), but building it now
would be optimising a path with 30x of headroom. **The one discipline that
remains load-bearing** is placement: this blocks whichever core runs it, so it
must stay on core 0 exactly like `pico_oled.h`, and must never migrate to the RT
core.

### CLI

```
panel id       raw ID-register dump (the probe; also lights the backlight)
panel init     run the ILI9341 init sequence
panel test     six colour bars: red green blue white yellow black
panel status   read back RDDPM / RDDMADCTL / RDDCOLMOD
panel fill 0xRGB565
panel bench    the numbers above
```

`panel <sub>` parses in `common/pico_cli.h` on every build and returns
`CLI_PANEL`; all hardware work happens on the pico side, so `common/` stays
hardware-free and host-testable. On a non-panel build it prints "not a panel
build" — the same inert-elsewhere shape as `cmd/ble/pair`.

## Stage 2 — capacitive touch working (VALIDATED 2026-08-05)

`pico/pico_touch.h`. One device on i2c1, and it identifies as FocalTech:

```
  i2c1 scan: 0x38
  addr 0x38  CHIPID 0x54  VENDID 0x79  FIRMID 0x01
```

`CHIPID 0x54` is the **FT5x46** family, consistent with the header's
`LCD_TOUCH_POINTS 5`. Register map is the standard FocalTech layout — TD_STATUS
at 0x02, then one 6-byte block per point from 0x03, with the event flag in
XH[7:6], the 12-bit X split across XH[3:0]/XL, the touch id in YH[7:4] and Y
across YH[3:0]/YL.

**Raw coordinates land directly in panel space** — no scaling needed. A traced
drag reports smoothly:

```
  id=0 ev=2 x=89 y=31 -> x=76 y=29 -> x=73 y=28 -> ... -> x=41 y=15
```

### The trap: the data page reads 0xFF until the first touch, ever

Straight after a cold boot, with no finger ever having landed:

```
  regs 00-0F: 00 FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
```

Register 0x00 reads correctly and every ID register reads correctly, so the bus
is provably fine — the *data page* is simply invalid until the controller's
first touch populates it. After that, idle reads a proper `TD_STATUS = 0x00`.

This matters because `TD_STATUS = 0xFF` means "15 points down". **Reject an
implausible count; do not clamp it.** Clamping 15 to `LCD_TOUCH_POINTS` would
manufacture five phantom touches on every poll forever, and they would look
entirely real downstream — the same "fields that report memory not reality"
shape as the extio persist gotchas. `touch_read` returns -2 instead.

### `panel watch` must not block — the watchdog is 2 s

The obvious implementation, a poll loop inside the CLI handler, **reboots the
box**. `BOX_WDT_MS` is 2000 ms and `watchdog_service()` is petted from the core-0
superloop, so any multi-second inline loop starves it; the console ring would
not drain either, so the output would be lost along with the box. The watch is
therefore a *deadline* armed by the CLI and polled by
`touch_watch_service_core0()` in the superloop, right next to the OLED service.
Same core-0 discipline as everything else cosmetic here.

INT (GP38, active-low) does assert during contact, but poll/edge timing means it
is occasionally high while a point is reported. Polling at 20 Hz is the correct
choice anyway — see the Open note about not treating panel taps as behavioural
timestamps.

### Y is mirrored; X is not — measured, not reasoned

Raw coordinates arrive in panel units but with Y running opposite the display.
`panel corner` paints four labelled corner squares and reports raw touches, which
settles it without any algebra about MADCTL's `MX` bit versus the header's
`LCD_TOUCH_MIRROR_Y`:

| touched (as displayed) | raw report | X | Y |
|---|---|---|---|
| RED, top-left | x=41 y=289 | correct | wrong |
| GREEN, top-right | x=202 y=295 | correct | wrong |
| BLUE, bottom-left | x=42 y=27 | correct | wrong |

So `y = LCD_HEIGHT - 1 - y_raw`, X passes through. Applied in `touch_read`
under `#ifdef LCD_TOUCH_MIRROR_Y` — driven off the board header rather than
hardcoded, so the other gen4 sizes pick up their own convention. Re-verified
after the fix: RED reports (36, 35) and BLUE (45, 278), both correct.

`touch_raw()` deliberately stays unmirrored so bring-up can still see exactly
what the controller said.

### CLI

```
panel touch        probe: i2c scan + ID registers + raw page dump
panel watch [s]    print raw page + decoded points on change
panel draw  [s]    black screen, yellow dot at each reported point
panel corner [s]   four labelled corners; the axis-mapping test
```

## Open

- Whether the panel should timestamp its own touch events through `pico_clock.h`.
  It costs nothing and is consistent with DI, but the honest latency floor is the
  I2C poll interval, not the ~µs of an IRQ-latched edge. Do not let anyone read
  panel taps as behavioural timestamps.
- Backlight-off idle policy and its interaction with `powermon/*` on a battery
  box.
