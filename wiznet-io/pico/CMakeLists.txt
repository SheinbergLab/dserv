# Drop-in example CMake for WIZnet-PICO-C.
# Copy this dir + common/*.h + net/*.h into WIZnet-PICO-C/examples/<name>/ and
# `add_subdirectory(<name>)` in examples/CMakeLists.txt.
#
# Build-target switch (the box firmware only):
#   default             -> W6300 wired backend (ioLibrary)
#   -DBOX_TARGET=pico2w -> Pico 2 W WiFi backend (CYW43 + lwIP).  Also pass
#                          -DPICO_BOARD=pico2_w and -DWIFI_SSID=.. -DWIFI_PASSWORD=..
#   (pico2w is not yet hardware-validated; W6300 path is.)

if(NOT DEFINED BOX_TARGET)
    set(BOX_TARGET w6300)
endif()
message(STATUS "wiznet-io BOX_TARGET = ${BOX_TARGET}")

# ============================ the box firmware ============================
# Transport-agnostic; only the backend + link libs differ by BOX_TARGET.
set(CFG_NAME wizchip_dserv_config)
add_executable(${CFG_NAME} wizchip_dserv_config.c)
target_include_directories(${CFG_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED BOX_FW_VERSION)              # baked by build.sh (git describe)
    target_compile_definitions(${CFG_NAME} PRIVATE BOX_FW_VERSION=\"${BOX_FW_VERSION}\")
endif()
if(DEFINED BOX_BUILD_TARGET)           # baked by build.sh ($TARGET) -- the OTA/shelf match key
    target_compile_definitions(${CFG_NAME} PRIVATE BOX_BUILD_TARGET=\"${BOX_BUILD_TARGET}\")
endif()
if(DEFINED BOX_BOARD_ID)               # baked by build.sh ($PICO_BOARD) -- the OTA compat filter
    target_compile_definitions(${CFG_NAME} PRIVATE BOX_BOARD_ID=\"${BOX_BOARD_ID}\")
endif()

if(BOX_TARGET STREQUAL "pico2w")
    target_compile_definitions(${CFG_NAME} PRIVATE BOX_NET_LWIP)
    if(DEFINED WIFI_SSID)
        target_compile_definitions(${CFG_NAME} PRIVATE
            WIFI_SSID=\"${WIFI_SSID}\" WIFI_PASSWORD=\"${WIFI_PASSWORD}\")
    endif()
    if(BOX_FUEL_MAX17048)               # MAX17048 fuel gauge (SparkFun Thing Plus)
        target_compile_definitions(${CFG_NAME} PRIVATE BOX_FUEL_MAX17048)
    endif()
    target_link_libraries(${CFG_NAME} PRIVATE
        pico_stdlib
        pico_multicore                  # core 1 = RT loop (see wizchip_dserv_config.c)
        pico_flash                      # flash_safe_execute: saves park the RT core
        pico_sha256                     # RP2350 hw SHA256 (pico_ota.h image verify)
        hardware_flash
        pico_cyw43_arch_lwip_poll)      # CYW43 WiFi + lwIP (poll mode)
elseif(BOX_TARGET STREQUAL "usb")       # plain Pico 2, USB-CDC to a host dserv module
    target_sources(${CFG_NAME} PRIVATE usb_descriptors.c)
    target_compile_definitions(${CFG_NAME} PRIVATE
        BOX_NET_USB
        CFG_TUSB_CONFIG_FILE=\"box_tusb_config.h\")   # our config, not the SDK's (shared source dir)
    if(BOX_USB_FORWARD_REGISTER)        # box emits its %match forwards over USB (modules/usbio auto-wires)
        target_compile_definitions(${CFG_NAME} PRIVATE BOX_USB_FORWARD_REGISTER)
    endif()
    target_link_libraries(${CFG_NAME} PRIVATE
        pico_stdlib
        pico_multicore
        pico_flash
        pico_sha256                     # RP2350 hw SHA256 (pico_ota.h image verify)
        pico_unique_id
        hardware_flash
        tinyusb_device                  # we own TinyUSB; pico_stdio_usb is NOT used here
        tinyusb_board)
elseif(BOX_TARGET STREQUAL "pico2wusb") # Pico 2 W: the usb box + BLE central (the BLE.md receiver)
    target_sources(${CFG_NAME} PRIVATE usb_descriptors.c)
    target_compile_definitions(${CFG_NAME} PRIVATE
        BOX_NET_USB                     # transport = USB-CDC, identical to the usb target
        BOX_BLE                         # + the radio (box_ble_central.h, core 0)
        CFG_TUSB_CONFIG_FILE=\"box_tusb_config.h\"
        CYW43_LWIP=0                    # BLE only -- no lwIP (cyw43_configport.h defaults it ON)
        PICO_STACK_SIZE=0x1000          # core 0 hosts cyw43+btstack call chains; 2K default is thin
        BOX_BLE_BREADCRUMBS=1)          # slot-boot radio support -- see the handheld block below
    if(BOX_USB_FORWARD_REGISTER)
        target_compile_definitions(${CFG_NAME} PRIVATE BOX_USB_FORWARD_REGISTER)
    endif()
    if(BOX_BLE_HANG_FOR_DEBUG)          # J-Link hang build (BLE_DEBUG=1 in build.sh): watchdog off,
        target_compile_definitions(${CFG_NAME} PRIVATE BOX_BLE_HANG_FOR_DEBUG=1)   # bring-up forced at boot
    endif()
    if(BOX_BLE_NO_FORCE)                # BLE_DEBUG=2: hang build, but bring-up only on typed enable
        target_compile_definitions(${CFG_NAME} PRIVATE BOX_BLE_NO_FORCE=1)
    endif()
    target_link_libraries(${CFG_NAME} PRIVATE
        pico_stdlib
        pico_multicore
        pico_flash
        pico_sha256                     # RP2350 hw SHA256 (pico_ota.h image verify)
        pico_unique_id
        hardware_flash
        tinyusb_device                  # we own TinyUSB; pico_stdio_usb is NOT used here
        tinyusb_board
        # Radio: POLL arch, not _none/_background -- background dispatches btstack
        # callbacks from IRQ context on core 0, and a printf there would corrupt
        # the SPSC log ring (box_console.h). Poll runs them in core 0's mainline
        # inside box_ble_service()'s cyw43_arch_poll().
        pico_cyw43_arch_poll
        pico_btstack_ble
        pico_btstack_cyw43)             # CYW43_ENABLE_BLUETOOTH=1 + BT fw + TLV flash bank (banks at
                                        # top-of-flash -12K/-8K on RP2350 -- clear of the persist sector)
elseif(BOX_TARGET STREQUAL "handheld")  # BLE-peripheral handheld / bench TX: transport = the radio (BLE.md)
    target_sources(${CFG_NAME} PRIVATE usb_descriptors.c)
    target_compile_definitions(${CFG_NAME} PRIVATE
        BOX_NET_BLE                     # transport = box_net_ble.h; USB = console only
        CFG_TUSB_CONFIG_FILE=\"box_tusb_config.h\"
        CYW43_LWIP=0                    # BLE only -- no lwIP (cyw43_configport.h defaults it ON)
        PICO_STACK_SIZE=0x1000          # core 0 hosts cyw43+btstack call chains; 2K default is thin
        # Slot-boot radio support (BLE.md "slot-boot radio wedge" 2026-07-17):
        # the REAL fix is in the vendored SDK patch (wiznet-io/patches/ --
        # btstack TLV bank reads via the untranslated alias, applied by
        # build.sh); BREADCRUMBS = watchdog-scratch stage markers (scratch[0]
        # ONLY -- [2]/[3] park rom_reboot params) printed after a watchdog
        # reset. Absent the patches, BBC() compiles away. (A DMA-source rebase
        # was also tried and REMOVED 2026-07-17 -- a TLV-fix-only slot boot
        # proved RP2350 DMA honors ATRANS translation.)
        BOX_BLE_BREADCRUMBS=1)
    if(BOX_FUEL_MAX17048)               # thingplus handheld: battery telemetry
        target_compile_definitions(${CFG_NAME} PRIVATE BOX_FUEL_MAX17048)
    endif()
    if(BOX_STATUS_LED)                  # Thing Plus: onboard WS2812 on GP14 -> status LED (pico_status_led.h)
        target_compile_definitions(${CFG_NAME} PRIVATE BOX_STATUS_LED)
        pico_generate_pio_header(${CFG_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/ws2812.pio)
        target_link_libraries(${CFG_NAME} PRIVATE hardware_pio)
    endif()
    if(BOX_BLE_HANG_FOR_DEBUG)          # J-Link hang build (BLE_DEBUG=1): watchdog off during bring-up
        target_compile_definitions(${CFG_NAME} PRIVATE BOX_BLE_HANG_FOR_DEBUG=1)
    endif()
    target_link_libraries(${CFG_NAME} PRIVATE
        pico_stdlib
        pico_multicore
        pico_flash
        pico_sha256
        pico_unique_id
        hardware_flash
        tinyusb_device                  # console CDCs only; data rides the radio
        tinyusb_board
        pico_cyw43_arch_poll            # same POLL-not-background reasoning as pico2wusb above
        pico_btstack_ble
        pico_btstack_cyw43)
    pico_btstack_make_gatt_header(${CFG_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/extio_pipe.gatt)
elseif(BOX_TARGET STREQUAL "dual")      # W6300-EVB: Ethernet OR USB, boot-selected (BOX_NET_DUAL)
    target_sources(${CFG_NAME} PRIVATE
        usb_descriptors.c
        box_net_w6300_impl.c            # both backends compiled in; runtime vtable picks one
        box_net_usb_impl.c
        box_net_dual_impl.c)
    target_compile_definitions(${CFG_NAME} PRIVATE
        BOX_NET_DUAL
        BOX_USB_FORWARD_REGISTER        # USB mode self-declares its forwards (modules/usbio auto-wires)
        CFG_TUSB_CONFIG_FILE=\"box_tusb_config.h\")
    target_link_libraries(${CFG_NAME} PRIVATE   # UNION of the usb + w6300 link libs
        pico_stdlib
        pico_multicore
        pico_flash
        pico_sha256                     # RP2350 hw SHA256 (pico_ota.h image verify)
        pico_unique_id
        hardware_spi
        hardware_dma
        hardware_flash
        tinyusb_device                  # own TinyUSB (dual-CDC); NOT pico_stdio_usb
        tinyusb_board
        ETHERNET_FILES
        IOLIBRARY_FILES
        LOOPBACK_FILES
        DHCP_FILES)
else()
    target_link_libraries(${CFG_NAME} PRIVATE
        pico_stdlib
        pico_multicore
        pico_flash
        pico_sha256                     # RP2350 hw SHA256 (pico_ota.h image verify)
        pico_unique_id
        hardware_spi
        hardware_dma
        hardware_flash
        ETHERNET_FILES
        IOLIBRARY_FILES
        LOOPBACK_FILES
        DHCP_FILES)
endif()

# ADS1115 analog-in (pico_ain.h, I2C) + SSD1306 status OLED (pico_oled.h, SPI0)
# are always compiled in, runtime-enabled via ain_en / oled_en.
target_link_libraries(${CFG_NAME} PRIVATE hardware_i2c hardware_spi)

# Stage 2: run the whole image from SRAM (RP2350: ~150KB image, 520KB SRAM).
# Flash saves then no longer park core 1 (nothing executes from XIP at runtime;
# PICO_FLASH_ASSUME_CORE1_SAFE makes flash_safe_execute skip the lockout), and
# XIP cache misses disappear as an RT jitter source. pico2w/pico2wusb excluded:
# their image plus the cyw43 firmware blob doesn't fit in RAM, so they keep the
# lockout path (a flash save parks core 1 briefly -- and stalls the core-0
# radio poll, which BLE rides out; see BLE.md).
if(NOT BOX_TARGET STREQUAL "pico2w" AND NOT BOX_TARGET STREQUAL "pico2wusb" AND NOT BOX_TARGET STREQUAL "handheld" AND NOT BOX_XIP)
    pico_set_binary_type(${CFG_NAME} copy_to_ram)
    target_compile_definitions(${CFG_NAME} PRIVATE PICO_FLASH_ASSUME_CORE1_SAFE=1)
endif()
# --xip build (sh build.sh dual --xip): default XIP binary, NO copy_to_ram, so the
# image can boot from an A/B partition slot via the bootrom's address translation
# (Stage-1 test). Without PICO_FLASH_ASSUME_CORE1_SAFE, rt_main registers core 1 as
# a flash-lockout victim, so flash ops park it (the pre-Stage-2 behavior) -- fine
# for OTA writes (gated !in_obs) and rare saves. BOX_XIP is compiled in for a banner.
# --tbyb: flag the IMAGE_DEF as try-before-you-buy (PICOBIN_IMAGE_TYPE_EXE_TBYB).
# A FLASH_UPDATE boot of this image is then buy-pending: the firmware must
# rom_explicit_buy after its self-test, else the bootrom reverts (rollback). The
# crt0 embedded block reads this def, so set it project-wide, not just on the target.
if(BOX_TBYB)
    target_compile_definitions(${CFG_NAME} PRIVATE PICO_CRT0_IMAGE_TYPE_TBYB=1 BOX_TBYB=1)
endif()
if(BOX_XIP)
    target_compile_definitions(${CFG_NAME} PRIVATE BOX_XIP=1)
endif()
# Any A/B SLOT image (--xip OR --tbyb OR signed) needs a hash so the bootrom's
# slot boot verifies it (boot_diagnostic IMAGE_DEF_VERIFIED_OK), like the official
# pico-examples pico_w/wifi/ota_update. Optionally sign it (BOX_SIGN_KEY, secp256k1).
# NOTE: copy_to_ram A/B images (no BOX_XIP) also get this -- copy_to_ram's IRQ-only
# flash (ASSUME_CORE1_SAFE) makes rom_explicit_buy's flash op WORK, unlike XIP.
if(BOX_XIP OR BOX_TBYB OR BOX_SIGN_KEY OR BOX_HASH)
    pico_hash_binary(${CFG_NAME})
    if(BOX_SIGN_KEY)
        pico_sign_binary(${CFG_NAME} ${BOX_SIGN_KEY})
    endif()
endif()

# The usb target owns TinyUSB for the data/CLI CDCs, so its stdio (printf + CLI)
# goes to UART instead of clashing with pico_stdio_usb. All others: CLI on USB.
if(BOX_TARGET STREQUAL "usb" OR BOX_TARGET STREQUAL "dual" OR BOX_TARGET STREQUAL "pico2wusb" OR BOX_TARGET STREQUAL "handheld")
    pico_enable_stdio_usb(${CFG_NAME} 0)    # we own TinyUSB; console goes to USB CDC0 via
    pico_enable_stdio_uart(${CFG_NAME} 1)   # box_net_usb_console_init(). UART kept as a boot/fallback console.
else()
    pico_enable_stdio_usb(${CFG_NAME} 1)
    pico_enable_stdio_uart(${CFG_NAME} 0)
endif()
pico_add_extra_outputs(${CFG_NAME})

# ===================== W6300-only test firmwares =========================
# These call ioLibrary directly (not through box_net), so wired-only.
if(NOT BOX_TARGET STREQUAL "pico2w" AND NOT BOX_TARGET STREQUAL "pico2wusb" AND NOT BOX_TARGET STREQUAL "handheld" AND NOT BOX_TARGET STREQUAL "usb" AND NOT BOX_TARGET STREQUAL "dual")

    set(TARGET_NAME wizchip_udp_do)
    add_executable(${TARGET_NAME} wizchip_udp_do.c)
    target_link_libraries(${TARGET_NAME} PRIVATE
        pico_stdlib hardware_spi hardware_dma
        ETHERNET_FILES IOLIBRARY_FILES LOOPBACK_FILES DHCP_FILES)
    pico_enable_stdio_usb(${TARGET_NAME} 1)
    pico_enable_stdio_uart(${TARGET_NAME} 0)
    pico_add_extra_outputs(${TARGET_NAME})

    set(WDT_NAME wizchip_dserv_wdt)
    add_executable(${WDT_NAME} wizchip_dserv_wdt.c)
    target_link_libraries(${WDT_NAME} PRIVATE
        pico_stdlib hardware_spi hardware_dma
        ETHERNET_FILES IOLIBRARY_FILES LOOPBACK_FILES DHCP_FILES)
    pico_enable_stdio_usb(${WDT_NAME} 1)
    pico_enable_stdio_uart(${WDT_NAME} 0)
    pico_add_extra_outputs(${WDT_NAME})

endif()
