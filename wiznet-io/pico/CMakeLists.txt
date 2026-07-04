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

pico_enable_stdio_usb(${CFG_NAME} 1)
pico_enable_stdio_uart(${CFG_NAME} 0)
pico_add_extra_outputs(${CFG_NAME})

# ===================== W6300-only test firmwares =========================
# These call ioLibrary directly (not through box_net), so wired-only.
if(NOT BOX_TARGET STREQUAL "pico2w")

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
