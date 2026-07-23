# extio-rw612 -- Zephyr application (freestanding; builds against $ZEPHYR_BASE).
#
#   west build -b native_sim  <this-dir>     # host smoke test, no board
#   west build -b frdm_rw612  <this-dir>     # the target
#
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(extio_rw612 LANGUAGES C)

# Common to every board: core + GPIO + USB transport + the uplink arbiter.
target_sources(app PRIVATE
	src/main.c
	src/platform/box_gpio.c        # block #3: GPIO + hardware-counter pulse
	src/platform/box_usbd.c        # block #4: USB device (device_next) context
	src/platform/box_net_usb.c     # block #4: CDC-ACM frame transport
	src/platform/box_uplink.c      # block #6: uplink arbiter
)

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
target_include_directories(app PRIVATE src/core src/platform)
