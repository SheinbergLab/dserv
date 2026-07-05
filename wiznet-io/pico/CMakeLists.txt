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
        pico_unique_id
        hardware_flash
        tinyusb_device                  # we own TinyUSB; pico_stdio_usb is NOT used here
        tinyusb_board)
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
        pico_unique_id
        hardware_spi
        hardware_dma
        hardware_flash
        ETHERNET_FILES
        IOLIBRARY_FILES
        LOOPBACK_FILES
        DHCP_FILES)
endif()

# ADS1115 analog-in (pico_ain.h) is always compiled in, runtime-enabled via ain_en.
target_link_libraries(${CFG_NAME} PRIVATE hardware_i2c)

# The usb target owns TinyUSB for the data/CLI CDCs, so its stdio (printf + CLI)
# goes to UART instead of clashing with pico_stdio_usb. All others: CLI on USB.
if(BOX_TARGET STREQUAL "usb" OR BOX_TARGET STREQUAL "dual")
    pico_enable_stdio_usb(${CFG_NAME} 0)    # we own TinyUSB; console goes to USB CDC0 via
    pico_enable_stdio_uart(${CFG_NAME} 1)   # box_net_usb_console_init(). UART kept as a boot/fallback console.
else()
    pico_enable_stdio_usb(${CFG_NAME} 1)
    pico_enable_stdio_uart(${CFG_NAME} 0)
endif()
pico_add_extra_outputs(${CFG_NAME})

# ===================== W6300-only test firmwares =========================
# These call ioLibrary directly (not through box_net), so wired-only.
if(NOT BOX_TARGET STREQUAL "pico2w" AND NOT BOX_TARGET STREQUAL "usb" AND NOT BOX_TARGET STREQUAL "dual")

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
