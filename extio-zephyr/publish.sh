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
#   sh publish.sh -d build-mcxn-ota          # frdm_mcxn947/mcxn947/cpu0
#   sh publish.sh -d build-teensy40-ota      # teensy40/mimxrt1062
#
#   -b is DERIVED from the build dir's CONFIG_BOARD_TARGET and only needs giving
#      if you want it cross-checked; a mismatch is refused. See below.
#   -n  dry run: print what would be sent, upload nothing.
#
# Needs DSERV_AGENT_FIRMWARE_TOKEN (read is open; only publish is gated).
# Optional FW_SHELF_URL (default https://dserv.net).
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR="$HERE/build-mcxn-ota"
BOARD_TARGET=""
BOARD_TARGET_SET=0
CHANNEL="${PUSH_CHANNEL:-dev}"
VERSION=""
DRY=0

while [ $# -gt 0 ]; do
  case "$1" in
    -d) BUILD_DIR="$2"; shift 2 ;;
    -b) BOARD_TARGET="$2"; BOARD_TARGET_SET=1; shift 2 ;;
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

# THE BOARD TARGET COMES OUT OF THE BUILD, not off the command line.
#
# The shelf lookup is an exact string match: extio_ota_push_shelf picks the image
# whose `build` equals the box's state/build, which box_announce.c derives from
# CONFIG_BOARD_TARGET. So the ONE authoritative source for this key is the
# .config of the very image being published -- reading it here makes the two
# sides structurally incapable of disagreeing.
#
# This used to default to a hard-coded frdm_mcxn947/mcxn947/cpu0 and trust -b,
# which is a quiet footgun the moment a second board exists: `-b teensy40` is
# the obvious thing to type (it is what west and buildall.sh take) and yields
# the key "teensy40", while the box announces "teensy40_mimxrt1062" from its
# full target. The upload succeeds, the shelf looks right, and the image is
# simply invisible to every box -- a failure with no error anywhere.
#
# -b is still accepted, but now it is CHECKED rather than believed.
DOTCONFIG="$BUILD_DIR/extio-zephyr/zephyr/.config"
[ -f "$DOTCONFIG" ] || { echo "!! no $DOTCONFIG -- is $BUILD_DIR a --sysbuild dir?" >&2; exit 1; }
BUILT_TARGET=$(sed -n 's/^CONFIG_BOARD_TARGET="\(.*\)"$/\1/p' "$DOTCONFIG")
[ -n "$BUILT_TARGET" ] || { echo "!! no CONFIG_BOARD_TARGET in $DOTCONFIG" >&2; exit 1; }

if [ "$BOARD_TARGET_SET" = 1 ] && [ "$BOARD_TARGET" != "$BUILT_TARGET" ]; then
  echo "!! -b says '$BOARD_TARGET' but $BUILD_DIR was built as '$BUILT_TARGET'.
   The shelf key must equal what the box announces, so publishing '$BOARD_TARGET'
   would upload an image no box can see. Drop -b (it is derived) or rebuild." >&2
  exit 1
fi
BOARD_TARGET="$BUILT_TARGET"

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

# THE IMAGE VERSION, READ OUT OF THE IMAGE.
#
# $VERSION above is `git describe` -- it names a publish, and no box has ever
# heard of it. What a box reports is state/fw_ver, which box_boot.c renders as
# "%u.%u.%u+%u" from the sem_ver in the MCUboot header it booted. Those are the
# same four fields sitting in this file's header, so reading them here produces
# the exact string the box will say about itself, and "is this box up to date?"
# becomes answerable instead of a guess. (Before this, the only comparable-
# looking strings were from different namespaces entirely -- and on 2026-08-14
# the newest shelf image for a build was OLDER than the flashed box, so
# comparing them would have advertised a downgrade.)
#
# NOT from extio-zephyr/VERSION, deliberately. That file is an input to the
# build and can be edited afterwards; the header is an output and cannot. If
# they ever disagree, the binary is right.
#
# Header layout, v1 (bootutil image.h), all little-endian: magic u32 =
# 0x96f3b83d, load_addr u32, hdr_size u16, protect_tlv_size u16, img_size u32,
# flags u32, then sem_ver { major u8, minor u8, revision u16, build_num u32 }
# at offset 20. Empty output (bad magic, short file, no python3) is not fatal:
# the field is optional and the shelf simply records nothing.
IMGVER=$(python3 - "$BIN" 2>/dev/null <<'PY' || true
import struct, sys
with open(sys.argv[1], 'rb') as f:
    h = f.read(28)
if len(h) == 28:
    magic, = struct.unpack_from('<I', h, 0)
    if magic == 0x96f3b83d:
        maj, mnr, rev, bld = struct.unpack_from('<BBHI', h, 20)
        print(f"{maj}.{mnr}.{rev}+{bld}")
PY
)

echo ">> shelf   $SHELF/api/firmware/extio/$CHANNEL"
echo ">> build   $BUILD_KEY   (board=$BOARD_ID, from $BOARD_TARGET)"
echo ">> version $VERSION  (dirty=$DIRTY)"
echo ">> bin     $BIN"
echo ">>          $SIZE bytes  sha256=$SUM"
if [ -n "$IMGVER" ]; then
  echo ">> imgver  $IMGVER  (from the MCUboot header -- what the box will report as fw_ver)"
else
  echo ">> imgver  (none -- no MCUboot header in this image; shelf records nothing)"
fi

[ "$DRY" = 1 ] && { echo ">> dry run, nothing sent"; exit 0; }

[ -n "$DSERV_AGENT_FIRMWARE_TOKEN" ] || {
  echo "!! need DSERV_AGENT_FIRMWARE_TOKEN (from the shelf host's /etc/dserv-agent/env)" >&2
  exit 1; }

RESP=$(mktemp)
CODE=$(curl -sS -o "$RESP" -w '%{http_code}' \
  -H "Authorization: Bearer $DSERV_AGENT_FIRMWARE_TOKEN" \
  -F "version=$VERSION" -F "build=$BUILD_KEY" -F "board=$BOARD_ID" \
  -F "variant=$BUILD_KEY" -F "dirty=$DIRTY" -F "imgver=$IMGVER" \
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
#
# MATCH THE IMAGE BY BUILD KEY. A channel holds one entry PER BUILD, so
# `grep binSha256 | head -1` -- what this did -- reads whichever image the agent
# happens to list first and compares it against a local file from a different
# board. That was correct only while exactly one board published here, and it
# fired the moment a second did: publishing the mcxn947 was checked against the
# teensy40's hash and reported "do NOT OTA this version" for an upload that was
# in fact perfect (2026-08-21). A verifier that fails on healthy input is worse
# than none -- it teaches you to ignore it.
#
# Parsed as JSON rather than shredded with tr/grep, for the same reason: picking
# a field out of the right OBJECT is not a line-oriented problem. python3 is
# already required above for IMGVER.
REMOTE=$(curl -sS --max-time 20 "$SHELF/api/firmware/extio/$CHANNEL/$VERSION" 2>/dev/null \
         | python3 -c '
import json, sys
key = sys.argv[1]
try:
    d = json.load(sys.stdin)
except Exception:
    sys.exit(0)                      # unparseable -> treated as "no answer" below
imgs = d.get("images") or (d.get("manifest") or {}).get("images") or []
for im in imgs:
    if im.get("build") == key:
        print(im.get("binSha256", ""))
        break
else:
    print("NO_SUCH_BUILD")
' "$BUILD_KEY")
if [ "$REMOTE" = "NO_SUCH_BUILD" ]; then
  echo "!! shelf has no image for build '$BUILD_KEY' at $VERSION -- the upload
   did not land where a box will look for it." >&2; exit 1
fi
if [ -n "$REMOTE" ]; then
  if [ "$REMOTE" = "$SUM" ]; then
    echo ">> shelf sha256 MATCHES the local image ($BUILD_KEY)"
  else
    echo "!! shelf sha256 $REMOTE != local $SUM for build '$BUILD_KEY'
   -- do NOT OTA this version" >&2; exit 1
  fi
fi

echo
echo "Next, on the box's dserv:"
echo "  dservctl extio \"extio_ota_push_shelf <box> $CHANNEL\""
