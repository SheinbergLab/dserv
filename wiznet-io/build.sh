#!/bin/sh
# build.sh -- build the box firmware from wiznet-io/ sources and refresh dist/.
#
#   sh build.sh              # dual (W6300-EVB: USB default / Ethernet via `mode eth`), DEFAULT
#   sh build.sh w6300        # W6300 wired-only target
#   sh build.sh pico2w       # Pico 2 W / WiFi target
#   sh build.sh dual --push  # build dual, then publish JUST that image to the shelf (dev channel)
#
# Env overrides:
#   WIZNET_PICO_C         path to a WIZnet-PICO-C clone (cloned here if unset/missing)
#   PICO_TOOLCHAIN_PATH   arm-none-eabi toolchain (needs 14.x for RP2350)
#   WIFI_SSID / WIFI_PASSWORD   (pico2w only)
#   --push publishing:
#     DSERV_AGENT_FIRMWARE_TOKEN  (required) Bearer token for the shelf publish endpoint
#     FW_SHELF_URL                shelf base URL (default https://dserv.net)
#     PUSH_CHANNEL                channel (default dev; stable/immutable refuses -dirty)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)                 # .../wiznet-io

# Args: a target name (positional) and/or --push [--channel <name>]. The target
# may appear before or after the flags (sh build.sh dual --push == --push dual).
TARGET=dual
PUSH=0
XIP=0                                               # --xip: build XIP (no copy_to_ram) for A/B slot-boot tests
TBYB=0                                              # --tbyb: flag the IMAGE_DEF try-before-you-buy (OTA trial image)
CHANNEL=${PUSH_CHANNEL:-dev}
while [ $# -gt 0 ]; do
  case "$1" in
    --push)        PUSH=1 ;;
    --xip)         XIP=1 ;;
    --tbyb)        TBYB=1 ;;
    --channel)     shift; CHANNEL=$1 ;;
    --channel=*)   CHANNEL=${1#--channel=} ;;
    -*)            echo "unknown flag '$1'" >&2; exit 1 ;;
    *)             TARGET=$1 ;;
  esac
  shift
done

# --xip: build as default (XIP, run-from-flash) instead of copy_to_ram, so the
# image boots from an A/B partition slot via the bootrom's address translation
# (Stage-1 experiment). Separate build dir + dist name so it never clobbers the
# normal copy_to_ram artifact. See OTA.md "Stage 1".
XIPSUF=""; XIPFLAG=""
if [ "$XIP" = 1 ]; then XIPSUF="_xip"; XIPFLAG="-DBOX_XIP=1"; fi
# --tbyb: set PICO_CRT0_IMAGE_TYPE_TBYB in the IMAGE_DEF (PICOBIN_IMAGE_TYPE_EXE_TBYB
# bit) so a FLASH_UPDATE boot of this image is buy-pending -> the box must
# rom_explicit_buy or the bootrom reverts. The OTA'd (trial) image is built --tbyb;
# the committed base image is not. (Same knob the cyw43 firmware TBYB variant uses.)
TBYBSUF=""; TBYBFLAG=""
if [ "$TBYB" = 1 ]; then TBYBSUF="_tbyb"; TBYBFLAG="-DBOX_TBYB=1"; fi
# SIGN_KEY=<secp256k1 .pem>: sign the image (pico_sign_binary). secp256k1 is
# mandatory (the RP2350 bootrom verifies only that curve). Signature is advisory
# until secure boot is enabled; today it just gives image consistency. Keep the
# real release key OUTSIDE the repo and pass its path here -- see the "Release
# signing & publishing" runbook in OTA.md for key generation/storage/OTP notes.
SIGNFLAG=""; SIGNSUF=""
[ -n "$SIGN_KEY" ] && { SIGNFLAG="-DBOX_SIGN_KEY=$SIGN_KEY"; SIGNSUF="_signed"; }

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
# Firmware version baked into the image: published as state/fw at every connect,
# shown in the console greeting + `show` -- fleet inventory for the status web
# page and, later, the OTA update check.
FWVER=${OTA_FWVER:-$(cd "$HERE" && git describe --always --dirty 2>/dev/null || echo dev)}   # OTA_FWVER overrides the state/fw string (distinguishable OTA test image; not the picobin A/B version)
# BOARD (= PICO_BOARD) is the shelf's hard compatibility key; VARIANT (= BOX_TARGET)
# is the descriptive role. Both ride the manifest on --push (build = $TARGET = the
# unique key). See dserv-agent/README.md "board matrix".
case "$TARGET" in
  w6300)                                              # W6300 wired (default)
    BOARD=pico2; VARIANT=w6300; FLAGS="-DPICO_BOARD=pico2" ;;
  pico2w)                                             # Raspberry Pi Pico 2 W
    BOARD=pico2_w; VARIANT=pico2w; FLAGS="-DBOX_TARGET=pico2w -DPICO_BOARD=pico2_w $WIFI" ;;
  picoplus2w)                                         # Pimoroni Pico Plus 2 W (RP2350B)
    BOARD=pimoroni_pico_plus2_w_rp2350; VARIANT=pico2w; FLAGS="-DBOX_TARGET=pico2w -DPICO_BOARD=$BOARD $WIFI" ;;
  thingplus)                                          # SparkFun Thing Plus RP2350 (RM2 + MAX17048 fuel gauge)
    BOARD=sparkfun_thingplus_rp2350; VARIANT=pico2w; FLAGS="-DBOX_TARGET=pico2w -DPICO_BOARD=$BOARD -DBOX_FUEL_MAX17048=1 $WIFI" ;;
  usb)                                                # plain Pico 2, USB-CDC to a host dserv (modules/usbio)
    BOARD=pico2; VARIANT=usb; FLAGS="-DBOX_TARGET=usb -DPICO_BOARD=pico2"
    [ -n "$USB_AUTOREG" ] && FLAGS="$FLAGS -DBOX_USB_FORWARD_REGISTER=1" ;;   # box self-declares its forwards
  dual)                                               # W6300-EVB: USB by default, Ethernet via `mode eth` (persisted)
    BOARD=pico2; VARIANT=dual; FLAGS="-DBOX_TARGET=dual -DPICO_BOARD=pico2" ;;
  *)
    echo "unknown target '$TARGET' (want: w6300 | pico2w | picoplus2w | thingplus | usb | dual)" >&2; exit 1 ;;
esac
# ADS1115 analog-in is always compiled in; activate at runtime with `ain enable 1`.
BUILD="$WIZ/build_${TARGET}${XIPSUF}${TBYBSUF}${SIGNSUF}"
mkdir -p "$BUILD"
( cd "$BUILD" && cmake .. -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBOX_FW_VERSION="$FWVER" -DBOX_BUILD_TARGET="$TARGET" -DBOX_BOARD_ID="$BOARD" $XIPFLAG $TBYBFLAG $SIGNFLAG $FLAGS >/dev/null \
  && ninja wizchip_dserv_config )

# 4. publish to dist/ under a target-specific name (no clobbering across targets).
#    The flat .bin is the OTA slot image (what the on-box updater pulls + writes
#    raw into the inactive A/B slot) -- for a --tbyb --sign build it's the sealed
#    try-before-you-buy trial image. The .uf2 is the bench-flash artifact.
OUT="wizchip_dserv_config_${TARGET}${XIPSUF}${TBYBSUF}${SIGNSUF}"
cp "$BUILD/examples/wiznet_io/wizchip_dserv_config.uf2" "$HERE/dist/$OUT.uf2"
cp "$BUILD/examples/wiznet_io/wizchip_dserv_config.elf" "$HERE/dist/$OUT.elf"
[ -f "$BUILD/examples/wiznet_io/wizchip_dserv_config.bin" ] && \
  cp "$BUILD/examples/wiznet_io/wizchip_dserv_config.bin" "$HERE/dist/$OUT.bin"
echo ">> dist/ updated: $OUT.uf2 ($(cd "$HERE" && ls -l dist/$OUT.uf2 | awk '{print $5" bytes"}'), fw $FWVER)"

# 5. optional: publish JUST this image to the dserv-agent firmware shelf. The
#    server computes the sha256 and gates dirty/immutability, so this is a plain
#    multipart POST of the one .uf2 we just built.
if [ "$PUSH" = 1 ]; then
  : "${FW_SHELF_URL:=https://dserv.net}"
  if [ -z "$DSERV_AGENT_FIRMWARE_TOKEN" ]; then
    echo "!! --push needs DSERV_AGENT_FIRMWARE_TOKEN in the environment" >&2
    exit 1
  fi
  DIRTY=0; case "$FWVER" in *-dirty) DIRTY=1 ;; esac
  URL="$FW_SHELF_URL/api/firmware/extio/$CHANNEL"
  # A --tbyb build is an OTA trial image: publish its sealed flat .bin (what the
  # box's on-box updater pulls) alongside the .uf2 and mark the manifest entry
  # ota-capable. A plain build ships only the bench-flash .uf2. See OTA.md.
  BINARGS=""
  if [ "$TBYB" = 1 ] && [ -f "$HERE/dist/$OUT.bin" ]; then
    BINARGS="-F ota=1 -F bin=@$HERE/dist/$OUT.bin"
    echo ">> (OTA trial: also publishing $OUT.bin, ota=1)"
  fi
  echo ">> push -> $URL (build=$TARGET board=$BOARD variant=$VARIANT version=$FWVER dirty=$DIRTY)"
  RESP=$(mktemp)
  CODE=$(curl -sS -o "$RESP" -w '%{http_code}' \
    -H "Authorization: Bearer $DSERV_AGENT_FIRMWARE_TOKEN" \
    -F "version=$FWVER" -F "build=$TARGET" -F "board=$BOARD" -F "variant=$VARIANT" \
    -F "dirty=$DIRTY" \
    -F "uf2=@$HERE/dist/$OUT.uf2" \
    $BINARGS \
    "$URL") || { echo "!! push failed (curl error)" >&2; rm -f "$RESP"; exit 1; }
  if [ "$CODE" = 200 ]; then
    echo ">> pushed OK:"; cat "$RESP"; echo
  else
    echo "!! push rejected: HTTP $CODE" >&2; cat "$RESP" >&2; echo >&2; rm -f "$RESP"; exit 1
  fi
  rm -f "$RESP"
fi
