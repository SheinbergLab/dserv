# extio box pin map (W6300-EVB-Pico2, dual image)

One row per RP2350 GPIO. "Fixed" pins are owned by the firmware/board and
refused by `pico_gpio_reserved()`; "claimed" pins are reserved only while the
matching feature is enabled (`ain enable 1` / `oled enable 1`, persisted);
everything else is free for `pin N mode ...` user I/O.

| GPIO | Function            | Owner / when                  | Notes                                        |
|------|---------------------|-------------------------------|----------------------------------------------|
| 0    | UART0 TX            | fixed (console fallback)      | 115200; boot/rescue console, always on       |
| 1    | UART0 RX            | fixed (console fallback)      |                                              |
| 2    | SPI0 SCK -> OLED CLK / MCP CLK | claimed iff `oled`/`mcp enable 1` | shared write bus; SSD1306 + MCP3204   |
| 3    | SPI0 TX -> OLED DATA / MCP DIN  | claimed iff `oled`/`mcp enable 1` |                                       |
| 4    | SPI0 RX <- MCP DOUT | claimed iff `mcp enable 1`    | MCP3204 MISO (was ADS1115 I2C0 SDA)          |
| 5    | MCP3204 CS          | claimed iff `mcp enable 1`    | software CS, idle high (was I2C0 SCL)        |
| 6    | OLED CS             | claimed iff `oled enable 1`   |                                              |
| 7    | OLED DC (SA0)       | claimed iff `oled enable 1`   |                                              |
| 8    | OLED RST            | claimed iff `oled enable 1`   |                                              |
| 9    | user I/O            | free                          |                                              |
| 10   | user I/O            | free                          |                                              |
| 11   | user I/O            | free                          |                                              |
| 12   | user I/O            | free                          |                                              |
| 13   | user I/O            | free                          | (office box: DI, in_pullup, active_low)      |
| 14   | user I/O            | free                          | (office box: DI, in_pullup, active_low)      |
| 15   | W6300 QSPI          | fixed (board)                 | GPIO15-22 wired to the W6300 on the EVB      |
| 16   | W6300 QSPI          | fixed (board)                 |                                              |
| 17   | W6300 QSPI          | fixed (board)                 |                                              |
| 18   | W6300 QSPI          | fixed (board)                 |                                              |
| 19   | W6300 QSPI          | fixed (board)                 |                                              |
| 20   | W6300 QSPI          | fixed (board)                 |                                              |
| 21   | W6300 QSPI          | fixed (board)                 |                                              |
| 22   | W6300 QSPI          | fixed (board)                 |                                              |
| 23   | (board)             | fixed (Pico2 SMPS mode)       | not routed for I/O                           |
| 24   | (board)             | fixed (Pico2 VBUS sense)      | not routed for I/O                           |
| 25   | onboard LED         | semi-free                     | usable as an output (e.g. `obs pin 25`)      |
| 26   | user I/O            | free                          | ADC-capable, unused (analog-in is MCP3204)   |
| 27   | user I/O            | free                          | ADC-capable, unused                          |
| 28   | mode strap          | fixed (dual image)            | open = auto (USB-first), GND = force Eth     |

**Free for user I/O with everything enabled:** 9, 10, 11, 12, 13, 14, 26, 27
(+25 as an output). Disable the OLED and 2, 3, 6, 7, 8 return; disable the MCP
analog-in and 4, 5 return (2, 3 too if the OLED is also off).

## MCP3204 analog joystick wiring (Mikroe Thumbstick Click)

The box's analog input is an MCP3204 12-bit SPI ADC (`mcp enable 1`, persisted),
CH0 = X, CH1 = Y, CH2/CH3 free; published as `state/ain/scan` = int16[4]. SPI0
bank-0 pins (NOT the SDK default SPI0, GP16-19, which collide with the W6300).
Shares SCK/MOSI with the OLED (write-only) when both are enabled; distinct CS.

| Thumbstick Click pin | -> | Board pin |
|----------------------|----|-----------|
| 3V3                  | -> | 3V3       |
| GND                  | -> | GND       |
| SCK                  | -> | GP2       |
| SDO (MISO)           | -> | GP4       |
| SDI (MOSI)           | -> | GP3       |
| CS                   | -> | GP5       |
| INT (stick push-btn) | -> | GP9 (DI, optional) |

Enable with `mcp enable 1` + `save` + `reboot`. Pin defaults live in
`pico/pico_gpio.h` (`MCP3204_PIN_*`, overridable at build time); the scan runs
on core 0, so it never perturbs the RT core.

## OLED breakout wiring (Adafruit 661, SSD1306 128x32 SPI)

| Display pin | -> | Board pin  |
|-------------|----|------------|
| GND         | -> | GND        |
| Vin         | -> | 3V3        |
| CLK         | -> | GP2        |
| DATA        | -> | GP3        |
| SA0/DC      | -> | GP7        |
| RST         | -> | GP8        |
| CS          | -> | GP6        |

Enable with `oled enable 1` + `save` + `reboot`. Pin defaults live in
`pico/pico_gpio.h` (`OLED_PIN_*`, overridable at build time); the display is
serviced from core 0 at 4 Hz (~0.5 ms/frame), so it can never perturb the RT
core or delay an MCP3204 sample (both share SPI0, both on core 0).

Display rows: `name + transport(+ '*' sensing)` / `IP or USB-host state` /
`ds: rg: ob: di:` (dserv client, registration ack, obs, DI levels) /
`boot cause + uptime`.

## Other builds

- **usb (plain Pico 2)**: no W6300 -> GPIO 15-22 are free user I/O; GP28 unused.
- **pico2w / thingplus (WiFi)**: CYW43 wireless pins reserved per the SDK board
  header instead of 15-22; check `pico_gpio_reserved()` for the exact set.
