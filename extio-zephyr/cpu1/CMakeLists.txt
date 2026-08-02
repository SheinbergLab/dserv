# cpu1 -- the MCXN947 second-core image (heartbeat bring-up).
#
# Built ONLY as a sysbuild sibling of the main app (see ../sysbuild.cmake);
# never flashed on its own. CONFIG_XIP=n links the whole image into SRAMG,
# cpu0 carries the resulting zephyr.bin as an embedded blob and copies it
# there at boot (src/platform/box_cpu1.c) -- flash slot1 stays the OTA
# staging area, untouched.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(extio_cpu1)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ../src/core)
