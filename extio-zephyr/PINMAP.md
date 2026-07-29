# extio box pin map — FRDM-MCXN947

**Box pin *n* IS Arduino D*n*.** The number you send in `cmd/do/<n>` and
`config/pin/<n>` is the number printed on the board. No lookup table needed for
the common case — this file is for *routing*, i.e. deciding which header pin to
run each external signal to.

The map itself lives in `boards/frdm_mcxn947_mcxn947_cpu0.overlay`
(`zephyr,user/box-gpios`); `src/platform/box_gpio.c` resolves it. Everything
below was read out of the built devicetree and the **VDF** package pinctrl
header (`MCXN947VDF-pinctrl.h`) — note the alternate-function list is
PACKAGE-SPECIFIC, and reading the VPB header instead silently returns a
different and wrong answer for several pads.

---

## Digital I/O — the Arduino header

`Free` pins take `pin N mode out|in|in_pullup|off`, `cmd/do/N`, DI groups, the
obs mirror and the sync input. Everything here is 3.3 V; **the MCXN947 is not
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
| 14 | A2 | P0_14 | free | **B14** | — | FC0/FC1, FLEXIO |
| 15 | A3 | P0_22 | free | **A14** | — | FC0/FC1, CMP1 |
| 16 | A4 | P0_15 | free | **B15** | — | FC0, FLEXIO |
| 17 | A5 | P0_23 | free — **also SW2** | **A15** | — | FC1_P3, CMP2 |

**17 usable digital pins.** Under the old one-port scheme there were 11, and
D0/D1/D3/D6 were unreachable at all.

Three things that are easy to trip over:

* **Every digital pin above is also an ADC input.** Analog is not confined to
  the J8 pads — any of these can become an `ain` channel by adding a
  `channel@N` to the overlay. The cost is that the pin stops being digital.
  (D3 is the exception that matters: it is **ADC1**, and only `lpadc0` is
  enabled, so it is *not* usable as analog today.)
* **Box pin 1 (D1) is DAC0_OUT.** That is the pad `adccal` drives for the
  DAC→ADC calibration sweep. If you route something to D1, `adccal` stops being
  available (and vice versa — leaving a jumper on D1 means box pin 1 must stay
  unconfigured).
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

Configured in the overlay as `channel@N` children of `lpadc0`; the box announces
the fitted count in `state/ain/dbg/chans` and the CLI validates against it.

| ain channel | ADC0 input | SoC pad | header |
|---|---|---|---|
| 0 | A1 | P4_15 | J8-20 |
| 1 | B1 | P4_19 | J8-24 |
| 2 | A2 | P4_23 | J8-28 |

**These cost no digital pins** — they are on port 4, and the box maps no digital
pins there except D0/D1. Full scale is **3300 mV** (`voltage-ref = <0>`,
channels on `ADC_REF_EXTERNAL0`), measured, not assumed — see PORTING.md.

Up to **8** channels are supported end to end (`AIN_MAX_CH`, `BOX_ADC_MAX_CH`,
and the CLI now takes its ceiling from the fitted count). To add more, declare
further `channel@N` with the right `zephyr,input-positive` — either the
remaining port-4 pads (P4_12/P4_13/P4_16/P4_17, header positions unverified —
check the schematic) or any Arduino pin from the table above, accepting the loss
of that digital pin.

Throughput note: `AIN_BLOCK_MAX` is 24 int16, so batch caps at `24 / nchan` —
8 scans at 3 channels, 4 at six. At a 1 kHz base that is 125 and 250 blocks/s
respectively, against an `AIN_MAX_BLOCKS_PER_S` limiter of 200. Mixed rates are
what the **groups** are for: eyes fast in one group, pupil / heart rate
decimated in another. The base rate applies to ALL channels; groups decimate on
the publish side only.

## Owned by the firmware — not box pins

| function | pads |
|---|---|
| ENET_QOS RMII | P1_4, P1_5, P1_6, P1_7, P1_13, P1_14, P1_15 |
| ENET_QOS MDIO/MDC | P1_21 (= D5, reserved), P1_20 |
| console UART (MCU-Link VCOM) | P1_8, P1_9 (flexcomm4) |
| USB HS (box CDCs) | usb1 + usbphy1, no header pads |
| QSPI flash (NVS storage) | flexspi |
| LPADC inputs | P4_15, P4_19, P4_23 (J8) |
| DAC0_OUT | P4_2 (= D1) |

## Other boards in this tree

teensy40, teensy41 and frdm_rw612 still use the **legacy** single-port scheme —
box pin *n* → `<box-gpio-port>.n` — because they were not on the bench when the
map landed and silently renumbering an unwatched board is how you never find
out. `box_gpio.c` supports both; a board gets the map by declaring
`zephyr,user/box-gpios`. The RP2350 boxes have their own `wiznet-io/PINMAP.md`.
