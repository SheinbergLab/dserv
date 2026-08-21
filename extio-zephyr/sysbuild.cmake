# sysbuild.cmake -- per-board wiring for the multi-image build.

# ---- Teensy 4.0: hand MCUboot the SAME flash layout the app gets ----
#
# The FRDM boards need nothing here: their partitions are in the board's own
# devicetree, so every image sees them. The PJRC board DTS defines none, and
# boards/teensy40.overlay reaches only the APP image -- MCUboot builds out of
# bootloader/mcuboot/boot/zephyr and never sees it. Without this the mcuboot
# image fails at devicetree time with
#
#     devicetree error: /chosen: undefined node label 'boot_partition'
#
# because MCUboot's own overlay points zephyr,code-partition at a node that does
# not exist on this board.
#
# EXTRA_ (not DTC_OVERLAY_FILE) so this ADDS to whatever MCUboot already applies
# rather than replacing it, and scoped to the board so the MCXN947 build is
# untouched -- an unconditional sysbuild/mcuboot.overlay would be applied to
# every board and would fail on any part that has no &w25q16jvuxim node.
#
# Both images therefore include boards/teensy40_partitions.dtsi, which is the
# single definition of the layout. See the note in that file for why sharing it
# is not optional: a bootloader and an application that disagree about where
# slot0 is fail only at the first swap.
if("${BOARD}" MATCHES "^teensy40")
  set(mcuboot_EXTRA_DTC_OVERLAY_FILE ${APP_DIR}/boards/teensy40_partitions.dtsi
      CACHE INTERNAL "teensy40 flash layout, shared with the app image")
endif()

# ---- cpu1 image on dual-core boards ----
#
# MCXN947 only: the cpu1 heartbeat app builds as a sibling image and the main
# app EMBEDS its zephyr.bin (CMakeLists.txt, BOX_HAVE_CPU1 branch), so nothing
# extra is flashed and OTA stays one artifact. The dependency matters: the
# blob must exist before the app image compiles the include that carries it.
if("${BOARD}" MATCHES "^frdm_mcxn947")
  ExternalZephyrProject_Add(
    APPLICATION cpu1
    SOURCE_DIR  ${APP_DIR}/cpu1
    BOARD       frdm_mcxn947/mcxn947/cpu1
  )
  add_dependencies(${DEFAULT_IMAGE} cpu1)
endif()
