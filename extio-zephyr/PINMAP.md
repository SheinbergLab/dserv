# extio box pin map — FRDM-MCXN947

**Box pin *n* IS Arduino D*n*, and ain channel *k* IS Arduino A*k*.** The
number you send in `cmd/do/<n>` and `config/pin/<n>` is the number printed on
the board, and the analog strip is the channel list in order. No lookup table
needed for the common case — this file is for *routing*, i.e. deciding which
header pin to run each external signal to. The UNO header is the box's ONE
field connector: a screw-terminal shield (or a custom one) reaches every box
pin, digital and analog, with the shield's silkscreen agreeing with the box
numbering.

The map itself lives in `boards/frdm_mcxn947_mcxn947_cpu0.overlay`
(`zephyr,user/box-gpios`); `src/platform/box_gpio.c` resolves it. Everything
below was read out of the built devicetree and the **VDF** package pinctrl
header (`MCXN947VDF-pinctrl.h`) — note the alternate-function list is
PACKAGE-SPECIFIC, and reading the VPB header instead silently returns a
different and wrong answer for several pads.

---

## Digital I/O — the Arduino header

`Free` pins take `pin N mode out|in|in_pullup|ain|off`, `cmd/do/N`, DI groups,
the obs mirror and the sync input. Everything here is 3.3 V; **the MCXN947 is not
5 V tolerant.**

| box pin | Arduino | SoC pad | status | ADC0 | PWM | other alternates |
|---|---|---|---|---|---|---|
| 0 | D0 | P4_3 | free | **B4** | — | LPUART2 RX, DAC1_OUT, CMP |
| 1 | D1 | P4_2 | free — **but see DAC note** | **A4** | — | LPUART2 TX, **DAC0_OUT** |
| 2 | D2 | P0_29 | free | **B21** | — | FC0/FC1 |
| 3 | D3 | P1_23 | free | ADC**1**_A23 | — | FC4, SCT0_OUT5 |
| 4 | D4 | P0_30 | free | **B22** | — | FC0/FC1 |
| **5** | **D5** | **P1_21** | **RESERVED — ENET0_MDIO** | — | — | the PHY's. Refused by `box_gpio_reserved()` |
| 6 | D6 | P1_2 | free — **blue LED** | A18 | CT1_MAT0 | **ENET0_MDC is on this pad's alt list — do not mux it** |
| 7 | D7 | P0_31 | free | **B23** | — | (GPIO/ADC only) |
| 8 | D8 | P0_28 | free | **B20** | — | FC0/FC1 |
| 9 | D9 | P0_10 | free — **RED LED, `ACTIVE_LOW`** | **B10** | **CT0_MAT0** | FC0_P6, FLEXIO |
| 10 | D10 | P0_27 | free — **GREEN LED, `ACTIVE_LOW`** | **B19** | **CT0_MAT3** | FC1_P3 (SPI CS) |
| 11 | D11 | P0_24 | free | **B16** | CT0_MAT0 | FC1_P0 (SPI MOSI) |
| 12 | D12 | P0_26 | free | **B18** | CT0_MAT2 | FC1_P2 (SPI MISO) |
| 13 | D13 | P0_25 | free | **B17** | CT0_MAT1 | FC1_P1 (SPI SCK) |
| 14 | A2 | P0_14 | free — or **ain ch2** | **B14** | — | switchable at runtime |
| 15 | A3 | P0_22 | free — or **ain ch3** | **A14** | — | switchable at runtime |
| 16 | A4 | P0_15 | free — or **ain ch4**, **via SJ8** | **B15** | — | switchable at runtime |
| 17 | A5 | P0_23 | free — or **ain ch5** — **also SW2** | **A15** | — | FC1_P3, CMP2 |

**16 usable digital pins** (17 mapped, minus D5 to the PHY) — of which **four
are dual-purpose**: A2/A3/A4/A5 are digital until you say `pin 14 mode ain`,
and digital again when you say `pin 14 mode out`. No rebuild either way.

Under the old one-port scheme there were 11, and D0/D1/D3/D6 were unreachable
at all.

**Reserved pins are refused by the CLI**, not silently ignored: `pin 5 mode out`
answers `ERR pin 5 is reserved on this board`. Before that gate existed the
mode was stored and echoed back by `show` while the pad stayed wired to
whatever owns it — a config that reported a mode the hardware did not have.
Only **D5** is reserved today: an earlier revision reserved 14–16 as well, when
the analog strip was allocated at build time; runtime `ain` gating (below)
replaced blanket reservation for pads the box legitimately shares.

Three things that are easy to trip over:

* **Every digital pin above is also an ADC input.** Analog is not confined to
  the A strip — any of these can become an `ain` channel by adding a
  `channel@N` to the overlay. The cost is that the pin stops being digital.
  (D3 is the exception that matters: it is **ADC1**, and only `lpadc0` is
  enabled, so it is *not* usable as analog today.)
* **Box pin 1 (D1) is DAC0_OUT** — and box pin 0 (D0) is DAC1_OUT; see the DAC
  section. D1 is the pad `adccal` drives for the DAC→ADC calibration sweep,
  and since the analog move the whole loop lives on the header: **jumper
  D1 → A0**. If you route something to D1, `adccal` stops being available (and
  vice versa — leaving a jumper on D1 means box pin 1 must stay unconfigured).
* **D6's alternate list includes ENET0_MDC**, which is the PHY's other pin. It
  is *not* muxed there by this board — MDC is on P1_20 — but do not let a future
  pinctrl edit put it back.

## The two LEDs

Both are wired **active-low on the board**, and both carry `GPIO_ACTIVE_LOW` in
the pin map, so the box's logical level is the intuitive one: **1 = lit**.
`obs pin 10` lights the green LED for the duration of each observation period,
which is verified on the bench and is the intended "the box is running" tell.

Because the flag is in the map rather than in config, `do 10 1` also lights the
LED — on this board that pad *is* the LED.

**Brightness is possible but not implemented.** Both LED pads reach CTIMER0
match outputs (**green = CT0_MAT3, red = CT0_MAT0**), `ctimer0` is already
enabled in the board devicetree, and Zephyr has the driver
(`pwm_mcux_ctimer.c`, `nxp,ctimer-pwm`). What it costs: a PWM-driven pad is
**not** a GPIO, so the obs mirror could no longer `gpio_pin_set` it — the pin
would need to be a PWM channel whose *duty* the obs mirror sets (0 / N instead
of low / high), plus a config item for the level. That is a real feature, not a
flag. Until then the LED is full-brightness on/off.

## Analog inputs (`ain` channels)

**ain channel *k* IS Arduino A*k*** — the analog twin of the digital rule, and
the whole strip lands on `lpadc0` (no second converter: D3 is the only ADC1
pin on the header and it stays digital). Configured in the overlay as
`channel@N` children; the box announces the fitted count in
`state/ain/dbg/chans` and the CLI validates against it.

| ain channel | Arduino | ADC0 input | SoC pad | costs a digital pin? |
|---|---|---|---|---|
| 0 | **A0** | 0A | ADC0_A0 — dedicated pad | no — always analog |
| 1 | **A1** | 0B | ADC0_B0 — dedicated pad | no — always analog |
| 2 | **A2** | B14 | P0_14 | yes — box pin 14 |
| 3 | **A3** | A14 | P0_22 | yes — box pin 15 |
| 4 | **A4** | B15 | P0_15 — **via SJ8** | yes — box pin 16 |
| 5 | **A5** | A15 | P0_23 — **also SW2** | yes — box pin 17 |

Channels 0/1 sit on **dedicated analog-only package pads** — not port pins, no
GPIO function at all, which is why Zephyr's `arduino_header` gpio-map starts at
A2 and why these two cost nothing and need no pinctrl. Channels 2-5 share box
pins 14-17 and are analog only while that pin is in `ain` mode — **`pin 14
mode ain` / `pin 14 mode out`, live, no rebuild and no reboot.**

The split is deliberate: **the BOARD declares what is possible**
(`zephyr,user/box-ain-pins`, indexed by ain channel, `0xff` = dedicated pad),
**the CONFIG decides what is used.** Channel indices never move, which matters
because the block header's channel mask is 8 bits of frozen wire format — an
index that shifted between reboots would be worse than a rebuild.

A group naming a channel whose pin is currently digital does not carry a stale
value for it: the group mask is intersected with what was actually sampled, so
the block **honestly reports fewer channels**. Verified live — with pin 15
digital the `aux` block goes `mask 60, nchan 4` → `mask 44, nchan 3` and back.

Two positions carry board-level caveats:

* **A4 (P0_15) reaches the header through solder jumper SJ8** (default 1-2 =
  ADC; 2-3 reroutes the position to P2_0/TRIG_IN5). A board with SJ8 flipped
  reads a disconnected input, **silently**.
* **A5 (P0_23) is also SW2, the user button.** The net is pulled up to 3.3 V
  and pressing the button **shorts it to ground** — including whatever the rig
  wired to the A5 terminal. Unloaded it idles at ~3.3 V. The compromised
  position: allocate it last, never to a source that minds a dead short.

### Migration from the J8 scheme

Images before 2026-07-31 had ch0-2 on J8-20/24/28 (`P4_15`/`P4_19`/`P4_23`)
and ch3-5 on A2/A3/A4. Persisted group configs keep their channel INDICES
across the reflash, but those indices now resolve one A position over (**old
ch3/4/5 = new ch2/3/4**) and nothing samples the J8 pads any more. Updating a
rig: re-jumper (wires on J8-20/24/28 land on A0/A1/A2), re-check `ain group`
definitions, re-run eye calibration.

Adding a channel on a PORT pad needs BOTH a `channel@N` and the pad in
`pinmux_box_ain` — a channel declared without the pinctrl entry leaves the pad
muxed as GPIO and the converter reads a disconnected input. Silent wrong
answer, same shape as the RW612's un-muxed digital pads. (Dedicated pads like
A0/A1 have no PORT function and need no pinctrl.) Full scale is **3300 mV**
(`voltage-ref = <0>`, channels on `ADC_REF_EXTERNAL0`), measured, not assumed —
see PORTING.md.

Up to **8** channels are supported end to end (`AIN_MAX_CH`, `BOX_ADC_MAX_CH`,
and the CLI now takes its ceiling from the fitted count). To add ch6/7, declare
further `channel@N` with the right `zephyr,input-positive` — the retired J8
pads (`P4_15`/`P4_19`/`P4_23` = CH1A/CH1B/CH2A on J8-20/24/28, the
pre-migration home) cost no digital pin but leave the shield; any Arduino D pin
from the table above stays on the shield at the cost of that digital pin.

### THROUGHPUT: six channels at 1 kHz in ONE group LOSES HALF THE DATA

`AIN_BLOCK_MAX` is 24 int16, so batch caps at `24 / nchan` — 4 scans at six
channels. At a 1 kHz base that produces 250 blocks/s against the
`AIN_MAX_BLOCKS_PER_S` limiter of 200, and **a throttled block is DISCARDED, not
deferred** (`st_throttled++; continue;`). Measured on the board: +2500 blocks and
**+2500 throttled** over 20 s. Exactly half the samples gone, while every counter
that a casual check looks at — `sweeps`, `dropped`, `late` — stays healthy.

**The fix is what the groups are for**, and it is verified on the board:

| group | channels | rate | blocks/s |
|---|---|---|---|
| `eye` | 0,1 | 1 kHz, batch 12 | 83.3 |
| `aux` | 2,3,4,5 | decimate 10 → 100 Hz, batch 4 | 25 |
| | | **total** | **108.3, `throttled` = 0** |

    ain group 0 channels 0,1     / label eye / batch 12
    ain group 1 channels 2,3,4,5 / label aux / mode continuous / decimate 10 / batch 4

**The base rate applies to ALL channels** — every one of the six is still
converted at 1 kHz; groups decimate on the *publish* side only. So the slow
signals cost sampling time regardless, they just do not cost frames.

## DAC outputs

| DAC | bits | driver | output pad | header | status |
|---|---|---|---|---|---|
| `dac0` | 12 | `nxp,lpdac` — proven (`adccal`) | P4_2 | **D1** | enabled |
| `dac1` | 12 | `nxp,lpdac` — same driver | P4_3 | **D0** | in the SoC tree, disabled |
| `dac2` | 14 | `nxp,hpdac` — driver exists, unproven here | dedicated pad | **none documented** | disabled |

Both 12-bit LPDACs land on the UNO header: D0/D1 are the analog-OUT corner,
opposite the A strip. `dac0` costs **box pin 1** whenever something drives it
(today: `adccal`). Enabling `dac1` is a board-overlay change — a pinctrl group
with `DAC1_OUT_PIO4_3` plus `status = "okay"` — and costs **box pin 0** the
same way. Neither pad has any other claim (LPUART2 on those pads is unused;
the console is FC4 on the MCU-Link).

Full scale is ~3.3 V — `adccal` measured DAC and ADC code-for-code, i.e. they
share a reference (see PORTING.md) — so ~0.81 mV/LSB, and the KNOWN FLOOR
applies: codes below ~700 clamp at ~517 mV, reproducible, unexplained. Fine
for a test stimulus or a coarse control voltage; characterize before trusting
it as a precision source. Zephyr's driver is immediate `dac_write_value` only —
the output changes at command rate, there is no buffered waveform path.

`dac2`, the 14-bit high-precision DAC, has a Zephyr driver (`nxp,hpdac`) but
UM12018 never mentions it: its dedicated output pad has **no documented route
to any header on this board**. Check the schematic before planning anything
around it. Being a dedicated pad it would cost no box pin and no pinmux —
routing is the only question.

## Owned by the firmware — not box pins

| function | pads |
|---|---|
| ENET_QOS RMII | P1_4, P1_5, P1_6, P1_7, P1_13, P1_14, P1_15 |
| ENET_QOS MDIO/MDC | P1_21 (= D5, reserved), P1_20 |
| console UART (MCU-Link VCOM) | P1_8, P1_9 (flexcomm4) |
| USB HS (box CDCs) | usb1 + usbphy1, no header pads |
| QSPI flash (NVS storage) | flexspi |
| LPADC inputs | ADC0_A0, ADC0_B0 (dedicated, = A0/A1) + P0_14, P0_22, P0_15, P0_23 (A2–A5, only while in `ain` mode) |
| DAC0_OUT | P4_2 (= D1) — and DAC1_OUT is P4_3 (= D0) if ever enabled |

## Other boards in this tree

teensy40, teensy41 and frdm_rw612 still use the **legacy** single-port scheme —
box pin *n* → `<box-gpio-port>.n` — because they were not on the bench when the
map landed and silently renumbering an unwatched board is how you never find
out. `box_gpio.c` supports both; a board gets the map by declaring
`zephyr,user/box-gpios`. The RP2350 boxes have their own `wiznet-io/PINMAP.md`.
