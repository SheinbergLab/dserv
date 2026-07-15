#!/usr/bin/env bash
# provision.sh -- ONE-TIME migration of a wiznet extio box to the OTA-capable A/B
# partition layout. After this the box updates over the air (no more BOOTSEL):
# `dservctl extio "extio_ota_push_shelf <box>"` or the extio-setup "OTA…" button.
#
# Portable: macOS or Linux. Needs `picotool` (>= 2.1) on PATH and the box in
# BOOTSEL. See wiznet-io/OTA.md "Provisioning a box".
#
# BOOTSEL first:
#   existing box:  console `bootsel`  OR  dservctl set extio/<box>/cmd/bootsel 1
#   blank board:   hold BOOTSEL while plugging in USB
#
# Usage:
#   ./provision.sh                          # local dist images (build them first, below)
#   ./provision.sh <slotA.uf2> [slotB.uf2]  # explicit local images (omit B -> first OTA fills it)
#   ./provision.sh --from-shelf             # pull the slot-A base from dserv.net + provision (slot B empty)
#                  [--channel <ch>]         #   default: dev
#                  [--build <target>]       #   default: dual (must match the box's board -- check state/build)
#                  [--version <ver>]        #   default: the channel's latest
#
# Remote one-liner (this is what dserv.net's /extio/setup serves): every flag
# above also has an env default so the piped-from-curl form needs no arguments --
#   curl -fsSL https://dserv.net/extio/setup | bash                 # dev / latest / dual
#   curl -fsSL https://dserv.net/extio/setup?build=pico2 | bash     # pick the board
# env knobs (flags still win): FW_SHELF_URL (default https://dserv.net),
#   PROVISION_FROM_SHELF=1 (make --from-shelf the default), PROVISION_CHANNEL,
#   PROVISION_BUILD, PROVISION_VERSION.
#
# Local images (the default paths) are built with -- signing/hashing is what makes
# a slot image bootable:
#   export SIGN_KEY=/path/to/extio-bench.pem          # any secp256k1 key (sig is advisory)
#   sh build.sh dual                                   # -> dist/wizchip_dserv_config_dual_signed.uf2       (slot A)
#   sh build.sh dual --tbyb                            # -> dist/wizchip_dserv_config_dual_tbyb_signed.uf2  (slot B)
#
# --from-shelf pulls the slot-A base that `sh build.sh <target> --tbyb --push` now
# publishes as the version's `uf2` (a hashed, non-TBYB image). Slot B is left empty;
# the first OTA fills it. (Older shelf versions whose `uf2` predates that change are
# a TBYB image -- still bootable in slot A, just not the intended committed base.)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
# PT_JSON may be supplied via the environment (the curl|bash remote flow has no
# script directory, so it writes the embedded spec to a temp file and points us
# at it); otherwise it sits next to this script.
PT_JSON="${PT_JSON:-$HERE/pt.json}"
DIST="$HERE/dist"
: "${FW_SHELF_URL:=https://dserv.net}"

die() { echo "!! $*" >&2; exit 1; }

# ---- args: flags in any order; up to two positional local-image paths ----
# Defaults come from the environment (so the curl|bash remote flow needs no
# arguments); explicit flags below still override them.
FROM_SHELF=${PROVISION_FROM_SHELF:-0}
SHELF_CH=${PROVISION_CHANNEL:-dev}; SHELF_BUILD=${PROVISION_BUILD:-dual}; SHELF_VER=${PROVISION_VERSION:-}
SLOT_A=""; SLOT_B=""; NPOS=0
while [ $# -gt 0 ]; do
  case "$1" in
    --from-shelf) FROM_SHELF=1 ;;
    --channel)    shift; SHELF_CH=${1:-} ;;
    --build)      shift; SHELF_BUILD=${1:-} ;;
    --version)    shift; SHELF_VER=${1:-} ;;
    -*)           die "unknown flag '$1' (see the header for usage)" ;;
    *)            NPOS=$((NPOS + 1)); if [ "$NPOS" = 1 ]; then SLOT_A="$1"; else SLOT_B="$1"; fi ;;
  esac
  shift
done

command -v picotool >/dev/null 2>&1 || die "picotool not on PATH (brew install picotool / build it with libusb)"
[ -f "$PT_JSON" ] || die "missing partition spec: $PT_JSON"

# In BOOTSEL, `picotool info` also prints the flash-resident image's metadata
# ("Program Information") -- that is NOT a running app, just what's stored in
# flash, so it can't be used to detect BOOTSEL. The reliable signal is the
# device's boot type (only `-a`/`-d` extended info carries it).
in_bootsel() { picotool info -a 2>/dev/null | grep -qiE 'boot type:[[:space:]]*bootsel'; }

# If the board isn't in BOOTSEL but is reachable (a cooperative app that exposes
# picotool's reset interface), bounce it; otherwise point at the physical button.
if ! in_bootsel; then
  echo ">> not in BOOTSEL -- trying to reboot the board into the bootloader ..."
  if picotool reboot -f -u >/dev/null 2>&1; then
    settled=0
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      sleep 1
      in_bootsel && { settled=1; break; }
    done
    [ "$settled" = 1 ] || die "board did not enter BOOTSEL after reboot (its firmware may not support software BOOTSEL).
   Hold the physical BOOTSEL button while plugging in USB, then re-run.
   existing extio box:  console 'bootsel'  OR  dservctl set extio/<box>/cmd/bootsel 1"
    echo ">> now in BOOTSEL"
  else
    die "no RP2350 in BOOTSEL (and none reachable to reboot).
   existing box:  console 'bootsel'  OR  dservctl set extio/<box>/cmd/bootsel 1
   blank board:   hold BOOTSEL while plugging in USB"
  fi
fi

TMPD=$(mktemp -d)
# PROVISION_PT_CLEANUP is set by the remote flow to the temp pt.json it wrote for
# us; clean it up here too since we own the only EXIT trap.
trap 'rm -rf "$TMPD"; [ -n "${PROVISION_PT_CLEANUP:-}" ] && rm -f "$PROVISION_PT_CLEANUP"' EXIT
PT_UF2="$TMPD/pt.uf2"

sha256_of() {   # portable sha256 (macOS shasum / Linux sha256sum)
  if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'
  else sha256sum "$1" | awk '{print $1}'; fi
}

# ---- resolve the two slot images (shelf pull, or local paths) ----
if [ "$FROM_SHELF" = 1 ]; then
  command -v curl    >/dev/null 2>&1 || die "curl not on PATH"
  command -v python3 >/dev/null 2>&1 || die "python3 not on PATH (used to read the shelf manifest)"
  echo ">> shelf: resolving $SHELF_BUILD from $FW_SHELF_URL (channel=$SHELF_CH${SHELF_VER:+ version=$SHELF_VER}) ..."
  RESOLVED=$(curl -fsS "$FW_SHELF_URL/api/firmware/extio" | python3 -c '
import sys, json
d = json.load(sys.stdin)
ch, build, want = sys.argv[1], sys.argv[2], sys.argv[3]
c = d.get("channels", {}).get(ch) or sys.exit("no such channel: " + ch)
ver = want or c.get("latest") or sys.exit("channel has no latest version")
m = next((v for v in c["versions"] if v["version"] == ver), None) or sys.exit("no such version: " + ver)
img = next((i for i in m["images"] if i.get("build") == build), None) or sys.exit("no " + build + " image in " + ver)
print(ver, img["file"], img["sha256"])
' "$SHELF_CH" "$SHELF_BUILD" "$SHELF_VER") || die "shelf query failed (channel=$SHELF_CH build=$SHELF_BUILD)"
  # shellcheck disable=SC2086
  set -- $RESOLVED
  [ $# -eq 3 ] || die "unexpected shelf response: $RESOLVED"
  VER=$1; FILE=$2; SHA=$3
  SLOT_A="$TMPD/$FILE"
  echo ">> shelf: downloading $FILE ($VER) ..."
  curl -fsS -o "$SLOT_A" "$FW_SHELF_URL/firmware/extio/$SHELF_CH/$VER/$FILE" || die "download failed"
  GOT=$(sha256_of "$SLOT_A")
  [ "$GOT" = "$SHA" ] || die "sha256 mismatch (got $GOT, manifest $SHA) -- NOT provisioning"
  echo ">> shelf: verified sha256 $SHA"
  SLOT_B=""                                            # slot B left empty; the first OTA fills it
else
  SLOT_A="${SLOT_A:-$DIST/wizchip_dserv_config_dual_signed.uf2}"
  SLOT_B="${SLOT_B:-$DIST/wizchip_dserv_config_dual_tbyb_signed.uf2}"
fi

[ -f "$SLOT_A" ] || die "slot-A image not found: $SLOT_A
   build it:  export SIGN_KEY=<key>; sh build.sh dual     (or pass --from-shelf)"

wait_bootsel() {                          # picotool needs the device to re-enumerate after reboot -u
  for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    in_bootsel && return 0                # true BOOTSEL, not just "app answered"
    sleep 1
  done
  die "device did not return to BOOTSEL after 'reboot -u'"
}

echo ">> 1/5  build the partition-table image from pt.json"
picotool partition create "$PT_JSON" "$PT_UF2" >/dev/null

echo ">> 2/5  write the partition table"
picotool load "$PT_UF2" >/dev/null

echo ">> 3/5  reboot -u  (make the new table resident; the bootrom's copy is stale until now)"
picotool reboot -u >/dev/null
wait_bootsel

echo ">> 4/5  load slot A (committed base): $(basename "$SLOT_A")"
picotool load "$SLOT_A" -p 0 >/dev/null
if [ -n "$SLOT_B" ] && [ -f "$SLOT_B" ]; then
  echo ">>       load slot B (trial):        $(basename "$SLOT_B")"
  picotool load "$SLOT_B" -p 1 >/dev/null
else
  echo ">>       slot B left empty (the first OTA fills it)"
fi

echo ">> 5/5  verify + boot the app"
picotool info -a 2>/dev/null | grep -iE 'partition [01]|tbyb' || true
picotool reboot >/dev/null

echo ">> done -- box is partitioned and OTA-capable."
echo "   It should reconnect to dserv; then set name / mode (eth|usb) / pins via"
echo "   extio-setup or the console. Future updates: extio_ota_push_shelf <box>."
