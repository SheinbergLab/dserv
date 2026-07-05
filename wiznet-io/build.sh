#!/bin/sh
# build.sh -- build the box firmware from wiznet-io/ sources and refresh dist/.
#
#   sh build.sh              # W6300 (wired) target, default
#   sh build.sh pico2w       # Pico 2 W / WiFi target
#   sh build.sh dual         # W6300-EVB: Ethernet OR USB, auto-selected at boot
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

# 3. configure + build. Each target has its OWN build dir + dist artifact name so
#    they never clobber each other. WiFi targets bake creds only as a fallback;
#    prefer setting them at runtime over the USB CLI (wifi ssid/pass).
WIFI="-DWIFI_SSID=${WIFI_SSID:-change-me} -DWIFI_PASSWORD=${WIFI_PASSWORD:-change-me}"
case "$TARGET" in
  w6300)                                              # W6300 wired (default)
    FLAGS="-DPICO_BOARD=pico2" ;;
  pico2w)                                             # Raspberry Pi Pico 2 W
    FLAGS="-DBOX_TARGET=pico2w -DPICO_BOARD=pico2_w $WIFI" ;;
  picoplus2w)                                         # Pimoroni Pico Plus 2 W (RP2350B)
    FLAGS="-DBOX_TARGET=pico2w -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350 $WIFI" ;;
  thingplus)                                          # SparkFun Thing Plus RP2350 (RM2 + MAX17048 fuel gauge)
    FLAGS="-DBOX_TARGET=pico2w -DPICO_BOARD=sparkfun_thingplus_rp2350 -DBOX_FUEL_MAX17048=1 $WIFI" ;;
  usb)                                                # plain Pico 2, USB-CDC to a host dserv (modules/usbio)
    FLAGS="-DBOX_TARGET=usb -DPICO_BOARD=pico2"
    [ -n "$USB_AUTOREG" ] && FLAGS="$FLAGS -DBOX_USB_FORWARD_REGISTER=1" ;;   # box self-declares its forwards
  dual)                                               # W6300-EVB: Ethernet OR USB, auto-selected at boot
    # Pass DUAL_STAGE1_ETH ALWAYS (0/1), never conditionally: build_dual's CMake cache
    # would otherwise retain a stale =1 from a prior ETH build and silently flip AUTO.
    FLAGS="-DBOX_TARGET=dual -DPICO_BOARD=pico2 -DDUAL_STAGE1_ETH=${DUAL_STAGE1_ETH:-0}" ;;   # Stage-1 bench: set =1 for AUTO->Ethernet
  *)
    echo "unknown target '$TARGET' (want: w6300 | pico2w | picoplus2w | thingplus | usb | dual)" >&2; exit 1 ;;
esac
# ADS1115 analog-in is always compiled in; activate at runtime with `ain enable 1`.
BUILD="$WIZ/build_$TARGET"
mkdir -p "$BUILD"
( cd "$BUILD" && cmake .. -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5 $FLAGS >/dev/null \
  && ninja wizchip_dserv_config )

# 4. publish to dist/ under a target-specific name (no clobbering across targets)
OUT="wizchip_dserv_config_$TARGET"
cp "$BUILD/examples/wiznet_io/wizchip_dserv_config.uf2" "$HERE/dist/$OUT.uf2"
cp "$BUILD/examples/wiznet_io/wizchip_dserv_config.elf" "$HERE/dist/$OUT.elf"
echo ">> dist/ updated: $OUT.uf2 ($(cd "$HERE" && ls -l dist/$OUT.uf2 | awk '{print $5" bytes"}'))"
