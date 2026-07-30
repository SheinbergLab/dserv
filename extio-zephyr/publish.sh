#!/bin/sh
# publish.sh -- put a built Zephyr image on the dserv.net firmware shelf.
#
# The Zephyr-board counterpart of wiznet-io/build.sh --push. Same shelf, same
# endpoint, same manifest contract -- what differs is the ARTIFACT:
#
#   RP2350: .uf2 (bootrom mass-storage image) + optional flat .bin (OTA payload)
#   here  : MCUboot-signed slot image ONLY -- zephyr.signed.bin
#
# There is no UF2 on these parts, so this publishes `bin` with `ota=1` and no
# uf2. That required making uf2 optional in dserv-agent/firmware.go (2026-07-29);
# against an older agent this fails with `uf2: no upload`, which is the tell that
# the shelf host needs the newer agent deployed.
#
# THE BUILD KEY MUST MATCH WHAT THE BOX ANNOUNCES. extio_ota_push_shelf picks the
# shelf image whose `build` equals the box's state/build, so they have to agree
# exactly. The shelf validates that field against ^[a-z0-9][a-z0-9_-]*$ -- no
# slashes -- while Zephyr board targets all contain them, so both sides flatten
# `/` to `_`: frdm_mcxn947/mcxn947/cpu0 -> frdm_mcxn947_mcxn947_cpu0. That is also
# exactly Zephyr's own board-file naming, so the key is not a third convention.
# box_announce.c does the box side (box_build_key()); this does the host side.
#
#   sh publish.sh [-d build-dir] [-b board-target] [-c channel] [-v version] [-n]
#
#   -n  dry run: print what would be sent, upload nothing.
#
# Needs DSERV_AGENT_FIRMWARE_TOKEN (read is open; only publish is gated).
# Optional FW_SHELF_URL (default https://dserv.net).
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR="$HERE/build-mcxn-ota"
BOARD_TARGET="frdm_mcxn947/mcxn947/cpu0"
CHANNEL="${PUSH_CHANNEL:-dev}"
VERSION=""
DRY=0

while [ $# -gt 0 ]; do
  case "$1" in
    -d) BUILD_DIR="$2"; shift 2 ;;
    -b) BOARD_TARGET="$2"; shift 2 ;;
    -c) CHANNEL="$2"; shift 2 ;;
    -v) VERSION="$2"; shift 2 ;;
    -n) DRY=1; shift ;;
    -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
    *) echo "!! unknown arg: $1" >&2; exit 1 ;;
  esac
done

SHELF="${FW_SHELF_URL:-https://dserv.net}"

# The sysbuild app image. NOT zephyr.bin: that is the unsigned link output, which
# MCUboot refuses, and the failure lands on the BOX as a rejected trial boot
# rather than here -- so check the signed one exists explicitly.
BIN="$BUILD_DIR/extio-zephyr/zephyr/zephyr.signed.bin"
[ -f "$BIN" ] || { echo "!! no signed image at $BIN
   build it first:  west build -b $BOARD_TARGET . --sysbuild -d $(basename "$BUILD_DIR")" >&2; exit 1; }

# build key: flatten separators, exactly as box_announce.c's box_build_key() does.
BUILD_KEY=$(printf '%s' "$BOARD_TARGET" | tr '/' '_')
BOARD_ID=$(printf '%s' "$BOARD_TARGET" | cut -d/ -f1)

# Version from git, matching build.sh's convention (and its -dirty semantics:
# immutable channels refuse a dirty tree, which is enforced by the agent).
if [ -z "$VERSION" ]; then
  VERSION=$(cd "$HERE" && git describe --tags --always --dirty 2>/dev/null || echo "0.0.0-nogit")
fi
DIRTY=0; case "$VERSION" in *-dirty) DIRTY=1 ;; esac

SIZE=$(wc -c < "$BIN" | tr -d ' ')
SUM=$(shasum -a 256 "$BIN" 2>/dev/null | cut -d' ' -f1 || sha256sum "$BIN" | cut -d' ' -f1)

echo ">> shelf   $SHELF/api/firmware/extio/$CHANNEL"
echo ">> build   $BUILD_KEY   (board=$BOARD_ID, from $BOARD_TARGET)"
echo ">> version $VERSION  (dirty=$DIRTY)"
echo ">> bin     $BIN"
echo ">>          $SIZE bytes  sha256=$SUM"

[ "$DRY" = 1 ] && { echo ">> dry run, nothing sent"; exit 0; }

[ -n "$DSERV_AGENT_FIRMWARE_TOKEN" ] || {
  echo "!! need DSERV_AGENT_FIRMWARE_TOKEN (from the shelf host's /etc/dserv-agent/env)" >&2
  exit 1; }

RESP=$(mktemp)
CODE=$(curl -sS -o "$RESP" -w '%{http_code}' \
  -H "Authorization: Bearer $DSERV_AGENT_FIRMWARE_TOKEN" \
  -F "version=$VERSION" -F "build=$BUILD_KEY" -F "board=$BOARD_ID" \
  -F "variant=$BUILD_KEY" -F "dirty=$DIRTY" \
  -F "ota=1" -F "bin=@$BIN" \
  "$SHELF/api/firmware/extio/$CHANNEL") \
  || { echo "!! push failed (curl error)" >&2; rm -f "$RESP"; exit 1; }

if [ "$CODE" != 200 ]; then
  echo "!! push rejected: HTTP $CODE" >&2; cat "$RESP" >&2; echo >&2
  case $(cat "$RESP") in
    *"uf2"*) echo "   ^ this shelf host runs an agent that still REQUIRES uf2;
     deploy the newer dserv-agent there (see the header of this script)." >&2 ;;
  esac
  rm -f "$RESP"; exit 1
fi

echo ">> pushed OK:"; cat "$RESP"; echo
rm -f "$RESP"

# Verify the shelf agrees with the local file, rather than trusting "ok": true.
# The agent computes its own sha256 server-side, so a mismatch here means the
# upload was corrupted in transit and the box would fail its own verify later.
REMOTE=$(curl -sS --max-time 20 "$SHELF/api/firmware/extio/$CHANNEL/$VERSION" 2>/dev/null \
         | tr ',{}' '\n\n\n' | grep -A0 'binSha256' | head -1 | sed 's/.*: *"//;s/".*//')
if [ -n "$REMOTE" ]; then
  if [ "$REMOTE" = "$SUM" ]; then
    echo ">> shelf sha256 MATCHES the local image"
  else
    echo "!! shelf sha256 $REMOTE != local $SUM -- do NOT OTA this version" >&2; exit 1
  fi
fi

echo
echo "Next, on the box's dserv:"
echo "  dservctl extio \"extio_ota_push_shelf <box> $CHANNEL\""
