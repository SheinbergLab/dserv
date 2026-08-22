# sysbuild.cmake -- per-board wiring for the multi-image build.

# ---- hand MCUboot the SAME flash layout the app gets ----
#
# The FRDM boards need nothing here: their partitions are in the board's own
# devicetree, so every image sees them. The PJRC boards define none, and
# boards/<board>.overlay reaches only the APP image -- MCUboot builds out of
# bootloader/mcuboot/boot/zephyr and never sees it. Without this the mcuboot
# image fails at devicetree time with
#
#     devicetree error: /chosen: undefined node label 'boot_partition'
#
# because MCUboot's own overlay points zephyr,code-partition at a node that does
# not exist on such a board.
#
# BY CONVENTION, NOT BY BOARD NAME: any board that carves its own layout puts it
# in boards/<board>_partitions.dtsi and includes that from its overlay; finding
# the file is what opts the board in. That is why this is an EXISTS test rather
# than the `MATCHES "^teensy40"` it started as -- adding the teensy41 needed no
# edit here, and the next board will not either. Boards whose partitions come
# from their own DTS simply have no such file and are untouched.
#
# EXTRA_ (not DTC_OVERLAY_FILE) so this ADDS to whatever MCUboot already
# applies rather than replacing it. An unconditional sysbuild/mcuboot.overlay
# would instead hit EVERY board and fail on any part lacking that flash node.
#
# Both images therefore include the same file, which is the single definition of
# the layout. See the note in it for why sharing is not optional: a bootloader
# and an application that disagree about where slot0 is fail only at the first
# swap, and look nothing like their cause.
if(EXISTS ${APP_DIR}/boards/${BOARD}_partitions.dtsi)
  set(mcuboot_EXTRA_DTC_OVERLAY_FILE ${APP_DIR}/boards/${BOARD}_partitions.dtsi
      CACHE INTERNAL "${BOARD} flash layout, shared with the app image")
endif()

# ---- per-board MCUboot Kconfig ----
#
# Same EXISTS-not-board-name convention as the partitions block above: a board
# that needs to say something to MCUboot puts it in
# sysbuild/mcuboot_<board>.conf and finding the file is what opts it in. The
# BOARD string carries qualifiers (`xiao_nrf54lm20a/nrf54lm20a/cpuapp`), so
# match on the leading board name rather than the whole thing.
#
# This is NOT the same lever as boards/<board>.conf, and the difference is the
# thing worth remembering: those reach the APPLICATION image only. MCUboot is a
# separate Zephyr application and never sees them, so a board whose devicetree
# turns a driver on by default turns it on for the BOOTLOADER too -- where
# MULTITHREADING=n means half the kernel API that driver calls does not exist.
# See sysbuild/mcuboot_xiao_nrf54lm20a.conf for the link failure that taught us.
string(REGEX REPLACE "/.*" "" _board_base "${BOARD}")
if(EXISTS ${APP_DIR}/sysbuild/mcuboot_${_board_base}.conf)
  set(mcuboot_EXTRA_CONF_FILE ${APP_DIR}/sysbuild/mcuboot_${_board_base}.conf
      CACHE INTERNAL "${_board_base} MCUboot Kconfig")
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
