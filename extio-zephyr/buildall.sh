#!/bin/sh
# buildall.sh -- compile-check EVERY board target this app claims to support.
#
# Exists because of 2026-08-20: both Teensy targets had been broken for days
# (a function parked inside an OTA-only #if, a field that exists only under a
# driver-selected Kconfig) and nothing said so, because only build-mcxn-ota
# gets rebuilt routinely. A board file that stops compiling is this project's
# version of a config field that lies -- this is the cheap CI that makes it
# say so. Run it before publishing or tagging; it builds into throwaway
# build-check-* dirs and never touches the real build dirs or the hardware.
#
#   sh buildall.sh          build all targets, stop at the first failure
#   sh buildall.sh -k       keep going, report every failure at the end
#
# ~2 min warm, ~6 min cold, all local.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ZEPHYR_BASE="${ZEPHYR_BASE:-$HOME/zephyrproject/zephyr}"; export ZEPHYR_BASE
. "$HOME/zephyrproject/.venv/bin/activate"

KEEP_GOING=0
[ "$1" = "-k" ] && KEEP_GOING=1

# TARGET LIST: every board with files in boards/. The mcxn947 is checked in its
# sysbuild (MCUboot) form -- the form that ships -- exactly like build-mcxn-ota.
# rt1186 stays out until its overlay is committed (see git status).
FAILED=""
check() {
  name="$1"; shift
  printf '>> %-14s ' "$name"
  if west build "$@" >"/tmp/buildall-$name.log" 2>&1; then
    echo OK
  else
    echo "FAILED (/tmp/buildall-$name.log)"
    FAILED="$FAILED $name"
    [ "$KEEP_GOING" = 1 ] || exit 1
  fi
}

cd "$HERE"
check teensy40  -b teensy40                     -d build-check-teensy40  "$HERE"
check teensy41  -b teensy41                     -d build-check-teensy41  "$HERE"
check rw612     -b frdm_rw612                   -d build-check-rw612     "$HERE"
check mcxn947   -b frdm_mcxn947/mcxn947/cpu0    -d build-check-mcxn947   "$HERE" --sysbuild
check nrf52840  -b nrf52840dk/nrf52840          -d build-check-nrf52840  "$HERE"
check xiao54lm20 -b xiao_nrf54lm20a/nrf54lm20a/cpuapp -d build-check-xiao54lm20 "$HERE"

# The teensy40 in its OTA form, which is a DIFFERENT BUILD, not a variant: it
# pulls in MCUboot as a second image, relinks the app to slot0, and compiles in
# box_ota_flash.c plus the whole arm/confirm path that the plain build never
# touches. Checking only `-b teensy40` would leave every one of those unbuilt --
# exactly the blind spot that let both Teensy targets rot for days before +98,
# one layer up. The plain build stays in the list because it is what ships on a
# box with no bootloader, and the two must BOTH keep compiling.
check teensy40-ota -b teensy40 -d build-check-teensy40-ota "$HERE" --sysbuild
check teensy41-ota -b teensy41 -d build-check-teensy41-ota "$HERE" --sysbuild

# The nrf52840dk is a SCOUT target (2026-08-22): board files exist, no hardware
# has ever run them. It is in this list from its first day precisely because it
# has no bench to notice it rotting -- which is what happened to both Teensys
# before +98, and they at least had a box on a desk.
check nrf52840-ota -b nrf52840dk/nrf52840 -d build-check-nrf52840-ota "$HERE" --sysbuild
# xiao54lm20-ota is OUT, and the reason is upstream, not ours: our app image
# builds and signs fine (141,072 B), but the MCUboot image for this board fails
# to LINK -- it compiles drivers/mfd/mfd_npm13xx.c and drivers/regulator against
# a bootloader that has no k_work_submit / z_impl_k_mutex_* / z_impl_k_usleep.
# So the plain build above is the honest claim for this board; A/B OTA is not
# established on it. Same convention as rt1186: a target stays out of this list
# until it actually passes.
# check xiao54lm20-ota -b xiao_nrf54lm20a/nrf54lm20a/cpuapp -d build-check-xiao54lm20-ota "$HERE" --sysbuild

if [ -n "$FAILED" ]; then
  echo "!! broken targets:$FAILED" >&2
  exit 1
fi
echo ">> all targets build"
