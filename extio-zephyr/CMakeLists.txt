# extio-zephyr -- Zephyr application (freestanding; builds against $ZEPHYR_BASE).
#
#   west build -b native_sim  <this-dir>     # host smoke test, no board
#   west build -b frdm_rw612  <this-dir>     # the target
#
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(extio_zephyr LANGUAGES C)

# Common to every board: core + GPIO + USB transport + the uplink arbiter.
target_sources(app PRIVATE
	src/main.c
	src/box_announce.c             # the box describing itself to dserv
	src/box_boot.c                 # why we booted + which image (OTA step 5)
	src/box_obs.c                  # obs-period state + idle-deferred work
	src/platform/box_gpio.c        # block #3: GPIO + k_timer box-timed pulse
	src/platform/box_sched.c       # scheduled events (fire at beginobs + N us)
	src/platform/box_console.c     # two-way CLI over the USB console CDC
	src/platform/box_usbd.c        # block #4: USB device (device_next) context
	src/platform/box_net_usb.c     # block #4: CDC-ACM frame transport
	src/platform/box_event.c       # service-loop wakeup (ISR-driven)
	src/platform/box_uplink.c      # block #6: uplink arbiter
	src/platform/box_pub.c         # publish queues + the uplink-writer thread
)

# Persistent settings -- exactly one box_flash backend behind box_flash.h:
#   NVS on internal flash        (RW612; flash writes are safe)
#   FAT file on the SD card      (Teensy 4.1; XIP-safe -- separate device)
# BOX_HAVE_PERSIST tells main.c that box_flash_* is available.
if(CONFIG_NVS)
	target_sources(app PRIVATE src/platform/box_flash.c)
	target_compile_definitions(app PRIVATE BOX_HAVE_PERSIST=1)
elseif(CONFIG_FILE_SYSTEM)
	target_sources(app PRIVATE src/platform/box_flash_sd.c)
	target_compile_definitions(app PRIVATE BOX_HAVE_PERSIST=1)
endif()

# BOX_HAVE_OBS_LEADER: this app's main.c carries the +30 obs-leader onset
# machinery (the at_abs obs-pin fire hook that ISR-stamps the actual instant
# and publishes actual-stamped state/in_obs). Board-independent within
# extio-zephyr, so it is an app capability, not a board one. It gates the CLI
# `obs mode leader` verb (box_cli.h) so a build WITHOUT the machinery -- an
# RP2350 image built from the same shared core -- refuses to announce an
# authority it cannot exercise. The datapoint obs/mode path is ungated (a
# leaderless box that persists mode=leader simply never fires, same as before).
target_compile_definitions(app PRIVATE BOX_HAVE_OBS_LEADER=1)

# MCUboot chainload repair: the RW6xx SoC init powers the TDDR PLL down in any
# image without Ethernet (i.e. MCUboot), and nothing ever powers it back up, so
# our ENET 1588 clock arrives dead and the first PTP read spins forever with
# interrupts disabled. Only needed when something boots before us.
if(CONFIG_SOC_SERIES_RW6XX AND CONFIG_BOOTLOADER_MCUBOOT)
	target_sources(app PRIVATE src/platform/box_soc_rw612.c)
endif()

# OTA step 2: writing the inactive MCUboot slot. Needs a real slot1_partition,
# so it rides on the same MCUboot gate -- a non-MCUboot build has no A/B pair to
# write into and links the app across where slot1 would be.
if(CONFIG_BOOTLOADER_MCUBOOT AND CONFIG_FLASH_MAP)
	target_sources(app PRIVATE src/platform/box_ota_flash.c)
	target_compile_definitions(app PRIVATE BOX_HAVE_OTA_SLOT=1)
endif()

# ---- per-board console default ----
# A fresh config (or one after `factory`) binds the box CLI to whichever console
# that BOARD can actually reach with the cable that powers it.
#
# On the FRDM-MCXN947 the console UART is the MCU-Link VCOM, and J17 supplies
# BOTH power and that VCOM -- so one cable brings the board up AND shows its
# config console, which is what bringing a box up at a new site needs. The USB
# CDC console needs a second cable to a port that does not power the board, and
# in `mode eth` the data pipe is suppressed anyway, so the CDC has no other job.
#
# Deliberately NOT global: on the Teensy the same setting means lpuart6 on pins
# 0/1 and a soldered-on USB-serial adapter, where defaulting to uart would make
# a healthy box look dead. Every other board keeps CDC.
#
# The nRF52840 DK is the MCXN947's case, not the Teensy's, and 2026-08-22 is
# when that got established rather than assumed: its J-Link OB exposes a VCOM on
# uart0 (P0.06/P0.08) over the SAME USB connector (J2) that powers the board,
# while the box's CDC console lives on the separate nRF USB port (J3) and needs
# a second cable. Flashed with the CDC default, the first boot printed exactly
# one line -- Zephyr's own banner -- and nothing from the box, on a board that
# was provably running (PC in arch_cpu_idle). A healthy box looking dead is the
# failure this gate exists to prevent, so the DK joins it.
#
# The XIAO nRF54LM20A does NOT, and the distinction is the point: that module
# has no debugger VCOM at all -- its single USB connector IS the nRF USB device
# port -- so CDC is the only console it has and the default is already right.
#
# The nRF54LM20 DK joins on 2026-08-31, VERIFIED rather than assumed -- which is
# what its overlay asked for when it deferred this decision as a scout. Its
# cpuapp dtsi chooses `uart20` for zephyr,console, uart20 is an IMCU VCOM, and
# the IMCU USB port is the one connector the board's own doc tells you to plug
# in. Flashed with the CDC default it reproduced the nRF52840 DK's symptom
# exactly: VCOM1 printed Zephyr's banner and not one line from the box.
if(CONFIG_BOARD_FRDM_MCXN947 OR CONFIG_BOARD_NRF52840DK OR CONFIG_BOARD_NRF54LM20DK)
	target_compile_definitions(app PRIVATE BOX_DEFAULT_CONSOLE_MODE=CONSOLE_MODE_UART)
endif()

# ---- second core (MCXN947 + sysbuild only) ----
# The cpu1 heartbeat image (cpu1/, built by sysbuild.cmake) rides inside THIS
# image as a generated byte array; box_cpu1.c copies it to SRAMG and releases
# the core. Gated on SYSBUILD so the plain build-mcxn947 compile-check dir --
# which has no cpu1 sibling image to embed -- still builds, just without a
# second core. See box_cpu1.c for why the vendor CONFIG_SECOND_CORE_MCUX
# path (XIP from slot1) is exactly what we must not do on an OTA box.
if(CONFIG_BOARD_FRDM_MCXN947 AND SYSBUILD)
	target_sources(app PRIVATE src/platform/box_cpu1.c)
	target_compile_definitions(app PRIVATE BOX_HAVE_CPU1=1)
	generate_inc_file_for_target(app
		${CMAKE_BINARY_DIR}/../cpu1/zephyr/zephyr.bin
		${ZEPHYR_BINARY_DIR}/include/generated/cpu1_fw.inc)
endif()

# Analog input: compiled wherever Zephyr's ADC subsystem is enabled. The file
# itself degrades to stubs when the board declares no `box_adc` node, so a board
# with CONFIG_ADC on for some other reason still links.
if(CONFIG_ADC)
	target_sources(app PRIVATE
		src/platform/box_adc.c         # one sweep, over Zephyr's ADC API
		src/platform/box_ain.c         # the sampling thread + group packing
	)
	target_compile_definitions(app PRIVATE BOX_HAVE_ADC=1)
endif()

# Hardware-paced ADC streaming (Stage 0: the streamtest instrument).
if(CONFIG_BOX_ADC_STREAM)
	target_sources(app PRIVATE
		src/platform/box_adc_stream.c  # CTIMER -> INPUTMUX -> LPADC -> eDMA ring
	)
endif()

# Ethernet + PTP: frdm_rw612 and teensy41 (teensy40 has no PHY).
if(CONFIG_NETWORKING)
	target_sources(app PRIVATE
		src/platform/box_net_eth.c     # block #5: Ethernet transport (sockets)
		src/platform/box_ptp.c         # block #5: ENET IEEE-1588 hardware clock
		src/platform/box_beacon.c      # LAN discovery broadcast (UDP :5011)
		src/platform/box_fetch.c       # OTA pull worker ('<' binary-get)
	)
endif()


# BLE ingress: frdm_rw612 only (no radio on Teensy).
if(CONFIG_BT)
	target_sources(app PRIVATE
		src/platform/box_ble.c         # block #6: multi-peripheral BLE central
	)
endif()

# The portable core (forked from wiznet-io, pico_* -> box_*) plus the RW612/Zephyr
# platform layer, added building-block by building-block.
target_include_directories(app PRIVATE src src/core src/platform)

# ---- network stack -> ITCM ----------------------------------------------------
# The box XIPs from external QSPI flash (CONFIG_XIP=y on Teensy 4.x), so every
# instruction-cache miss is a FlexSPI fetch. Our service loop is small and runs
# constantly, so it stays hot -- measured at 10 us per idle pass. The network
# stack is large and runs intermittently, so it is cold on every publish, and a
# single 128-byte zsock_send() measured 275-864 us of CPU (state/dbg/send_us,
# 2026-07-24). The VARIABILITY is the tell: DMA waits and protocol timers do not
# swing 3x, cache state does. Ruled out beforehand: Nagle, syscall count, TX
# traffic classes, and TX descriptor depth -- see boards/teensy41.conf.
#
# CONFIG_CODE_DATA_RELOCATION is already on, and the in-tree precedent is the
# FlexSPI flash driver, which relocates itself to ITCM for the same reason
# (drivers/flash/CMakeLists.txt). ITCM is 128 KB with ~9 KB used, so there is
# room; if there is not, the link fails loudly rather than silently degrading.
if(CONFIG_NETWORKING AND CONFIG_CODE_DATA_RELOCATION)
	zephyr_code_relocate(LIBRARY subsys__net__ip           LOCATION ITCM_TEXT)
	zephyr_code_relocate(LIBRARY subsys__net__l2__ethernet LOCATION ITCM_TEXT)
	zephyr_code_relocate(LIBRARY drivers__ethernet         LOCATION ITCM_TEXT)
	zephyr_code_relocate(LIBRARY lib__net_buf              LOCATION ITCM_TEXT)
endif()
