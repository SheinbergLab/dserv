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

if [ -n "$FAILED" ]; then
  echo "!! broken targets:$FAILED" >&2
  exit 1
fi
echo ">> all targets build"
