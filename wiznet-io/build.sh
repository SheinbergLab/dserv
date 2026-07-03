#!/bin/sh
# build.sh -- build the box firmware from wiznet-io/ sources and refresh dist/.
#
#   sh build.sh              # W6300 (wired) target, default
#   sh build.sh pico2w       # Pico 2 W / WiFi target
#
# Env overrides:
#   WIZNET_PICO_C         path to a WIZnet-PICO-C clone (cloned here if unset/missing)
#   PICO_TOOLCHAIN_PATH   arm-none-eabi toolchain (needs 14.x for RP2350)
#   WIFI_SSID / WIFI_PASSWORD   (pico2w only)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)                 # .../wiznet-io
TARGET=${1:-w6300}
WIZ=${WIZNET_PICO_C:-$HERE/.wiznet-pico-c}
: "${PICO_TOOLCHAIN_PATH:=/Applications/ArmGNUToolchain/14.3.rel1/arm-none-eabi}"
export PICO_TOOLCHAIN_PATH

# 1. WIZnet-PICO-C tree (bundles pico-sdk + ioLibrary); clone once if missing
if [ ! -d "$WIZ/.git" ]; then
  echo ">> cloning WIZnet-PICO-C into $WIZ (one-time) ..."
  git clone --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/WIZnet-ioNIC/WIZnet-PICO-C.git "$WIZ"
fi

# 2. drop our sources in as one flattened example
DST="$WIZ/examples/wiznet_io"
rm -rf "$DST"; mkdir -p "$DST"
cp "$HERE"/pico/* "$HERE"/common/*.h "$HERE"/net/*.h "$DST"/
grep -q 'add_subdirectory(wiznet_io)' "$WIZ/examples/CMakeLists.txt" 2>/dev/null \
  || printf '\nadd_subdirectory(wiznet_io)\n' >> "$WIZ/examples/CMakeLists.txt"

# 3. configure + build
if [ "$TARGET" = "pico2w" ]; then
  FLAGS="-DBOX_TARGET=pico2w -DPICO_BOARD=pico2_w \
         -DWIFI_SSID=${WIFI_SSID:-change-me} -DWIFI_PASSWORD=${WIFI_PASSWORD:-change-me}"
else
  FLAGS="-DPICO_BOARD=pico2"
fi
BUILD="$WIZ/build_$TARGET"
mkdir -p "$BUILD"
( cd "$BUILD" && cmake .. -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5 $FLAGS >/dev/null \
  && ninja wizchip_dserv_config )

# 4. publish to dist/
cp "$BUILD/examples/wiznet_io/wizchip_dserv_config.uf2" \
   "$BUILD/examples/wiznet_io/wizchip_dserv_config.elf" "$HERE/dist/"
echo ">> dist/ updated ($TARGET): $(cd "$HERE" && ls -l dist/wizchip_dserv_config.uf2 | awk '{print $5" bytes"}')"
