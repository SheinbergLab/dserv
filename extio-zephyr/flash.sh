#!/bin/sh
# flash.sh -- flash build-mcxn-ota onto an FRDM-MCXN947, footguns encoded.
#
#   sh flash.sh                  exactly one probe attached: flash it
#   sh flash.sh <PROBE-UID>      more than one attached: name the target
#   sh flash.sh -n [UID]         dry run -- resolve and report, flash nothing
#   sh flash.sh -v [UID]         flash, then wait ~40 s and check the RX
#                                timestamp engine is actually STAMPING
#
# Guard rails, each one earned on this bench:
#   * Pack pinning. PYOCD_PROJECT_DIR is forced to this directory so
#     pyocd.yaml's DFP 19.0.0 applies no matter where you run from. The
#     newest pack (26.06) dies PART WAY THROUGH ERASING and half-bricks the
#     board; a bare `west flash` from the wrong cwd loses the pin.
#   * build-mcxn-ota ONLY. build-mcxn947 links at offset 0 and flashing it
#     erases MCUboot.
#   * No guessing. Two probes attached and no UID is a refusal, not a coin
#     flip; a given UID is validated against what is actually attached.
#   * --dev-id is always passed, even with one probe, so a probe hot-plugged
#     between resolve and flash cannot swap the target.
#
# The -v check reads the extio_rxts counters (address re-derived from the elf
# every run -- it moves between builds). sv==pkts means every received frame
# carried a hardware RX timestamp. sv==0 while pkts climbs is the boot-time
# arming wedge (PORTING.md, "AUTOPSY: MCXN947 no RX timestamps") and means
# the image predates the carrier-up re-arm fix. TSU epoch-vs-uptime is
# reported as information only -- sync needs a grandmaster, which is the
# environment's job, not the flash's.

set -e
cd "$(dirname "$0")"
SELF="$(basename "$0")"          # $0 may be relative to the caller's cwd,
                                 # which the cd above just invalidated
PYOCD_PROJECT_DIR="$(pwd)"; export PYOCD_PROJECT_DIR
ZEPHYR_BASE="${ZEPHYR_BASE:-$HOME/zephyrproject/zephyr}"; export ZEPHYR_BASE
. "$HOME/zephyrproject/.venv/bin/activate"

usage() { sed -n '2,9p' "$SELF" | sed 's/^# \{0,1\}//'; }

DRY=0; VERIFY=0; TARGET=""
for a in "$@"; do
  case "$a" in
    -n|--dry-run) DRY=1 ;;
    -v|--verify)  VERIFY=1 ;;
    -h|--help)    usage; exit 0 ;;
    -*)           echo "unknown flag: $a" >&2; usage >&2; exit 2 ;;
    *)            TARGET="$a" ;;
  esac
done

ELF=build-mcxn-ota/extio-zephyr/zephyr/zephyr.elf
[ -f "$ELF" ] || { echo "no $ELF -- run west build first (see CHEATSHEET.md)" >&2; exit 1; }

PROBES=$(pyocd json --probes 2>/dev/null | python3 -c '
import json, sys
for b in json.load(sys.stdin).get("boards", []):
    print(b["unique_id"], b.get("board_name", "?"))')

N=$(printf '%s\n' "$PROBES" | grep -c . || true)

if [ "$N" -eq 0 ]; then
  echo "no debug probe attached" >&2; exit 1
elif [ -n "$TARGET" ]; then
  printf '%s\n' "$PROBES" | awk '{print $1}' | grep -qx "$TARGET" || {
    echo "probe '$TARGET' is not attached. Attached:" >&2
    printf '%s\n' "$PROBES" | sed 's/^/  /' >&2; exit 1; }
elif [ "$N" -eq 1 ]; then
  TARGET=$(printf '%s\n' "$PROBES" | awk '{print $1}')
else
  echo "$N probes attached -- refusing to guess. Name one:" >&2
  printf '%s\n' "$PROBES" | sed 's/^/  /' >&2
  echo "usage: sh flash.sh <PROBE-UID>" >&2; exit 2
fi

echo "target probe: $TARGET"
echo "image:        $ELF ($(stat -f '%Sm' "$ELF"))"
if [ "$DRY" -eq 1 ]; then
  echo "dry run -- would: west flash -r pyocd -d build-mcxn-ota --dev-id $TARGET"
  exit 0
fi

west flash -r pyocd -d build-mcxn-ota --dev-id "$TARGET"

[ "$VERIFY" -eq 1 ] || exit 0

echo "verify: waiting 40 s for boot + link + traffic..."
sleep 40
ADDR=0x$(nm "$ELF" | awk '$3=="extio_rxts"{print $1}')
[ "$ADDR" != "0x" ] || { echo "extio_rxts not in elf -- driver patch missing?" >&2; exit 1; }
pyocd cmd -t mcxn947 -u "$TARGET" -O connect_mode=attach \
    -c "read32 $ADDR 0x20" -c "read32 0x40100B08 4" 2>/dev/null | python3 -c '
import sys
words = []
for line in sys.stdin:
    if ":" in line:
        words += [int(w, 16) for w in line.split("|")[0].split(":")[1].split()]
if len(words) < 9:
    sys.exit("verify: SWD read failed or came back short")
pkts, sv, tsu = words[0], words[1], words[8]
print(f"verify: pkts={pkts} stamped={sv} tsu_seconds={tsu:#x}")
if pkts == 0:
    print("verify: INCONCLUSIVE -- no RX traffic yet (link? cable? network?)")
elif sv == 0:
    sys.exit("verify: FAIL -- receiving but not stamping: the arming wedge. "
             "Image predates the carrier-up re-arm fix?")
else:
    print(f"verify: STAMPING OK ({sv}/{pkts})")
    print("verify: TSU on epoch -- synced to a grandmaster" if tsu > 10**9
          else "verify: TSU still uptime -- no sync yet (grandmaster reachable?)")
'
