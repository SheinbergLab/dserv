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

# Ethernet + PTP: frdm_rw612 and teensy41 (teensy40 has no PHY).
if(CONFIG_NETWORKING)
	target_sources(app PRIVATE
		src/platform/box_net_eth.c     # block #5: Ethernet transport (sockets)
		src/platform/box_ptp.c         # block #5: ENET IEEE-1588 hardware clock
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
