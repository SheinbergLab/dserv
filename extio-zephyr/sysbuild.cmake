# sysbuild.cmake -- add the cpu1 image on dual-core boards.
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
