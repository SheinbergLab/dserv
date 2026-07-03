# Prebuilt firmware (W6300-EVB-Pico2 / RP2350, ARM Cortex-M33)

Built from ../pico + ../common against WIZnet-PICO-C (QSPI_QUAD, static IP
192.168.11.2). These flash as-is; no source tree needed.

- wizchip_dserv_config.{uf2,elf}  config listener + USB CLI + persist + GPIO (the "box")
- wizchip_dserv_wdt.{uf2,elf}      watchdog/telemetry TCP client
- wizchip_udp_do.{uf2,elf}         direct-UDP DO latency test

Flash options:
  UF2:  hold BOOTSEL, drop the .uf2 on the RP2350 drive
  SWD:  openocd -f interface/jlink.cfg -f target/rp2350.cfg \
             -c "program wizchip_dserv_config.elf verify reset exit"

To rebuild fresh, see ../README.md (Build & flash). These are a convenience
snapshot — treat the sources in ../ as the source of truth.
