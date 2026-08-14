#!/bin/sh
# whichboard.sh -- plug ONE FRDM-RW612 into this Mac's MCU-Link USB-C, run this.
#
# Tier 1 of identifying the RMA board: the onboard MCU-Link probe's USB serial
# number is per-board and was recorded when board 3 was condemned.
#   board 3 (DEAD PHY, RMA)      probe 1067513969
#   board 4 == box3 (known good) probe 1060261978
# Any other serial is a board whose probe id we never wrote down -> tier 2
# (flash mdio_health and read the PHY IDs; see README.md).
#
# Straight into the machine, never a hub -- an unpowered hub reads as dead
# silicon.  A serial printed here only says WHICH board it is; it says nothing
# about the PHY.

FIRST=1069813330   # board 1
SECOND=1063771898  # board 2
RMA=1067513969     # board 3
GOOD=1060261978    # board 4

# ioreg, not system_profiler: the latter returns an empty tree under some
# sandboxes, which reads exactly like "no board plugged in".
dump=$(ioreg -p IOUSB -w0 -l 2>/dev/null)

# Only debug-probe nodes, or hub/NIC serials get reported as candidate boards.
# The probe's vendor string is the stable part: SEGGER for the J-Link OEM
# firmware these carry, NXP for a board still on stock MCU-Link CMSIS-DAP.
# macOS reports the serial zero-padded ("001067513969") -- strip that.
serials=$(printf '%s\n' "$dump" |
          grep -B8 -A8 '"USB Vendor Name" = "\(SEGGER\|NXP\)"' |
          sed -n 's/.*"USB Serial Number" = "0*\([0-9]\{6,12\}\)".*/\1/p' |
          sort -u)

if [ -z "$serials" ]; then
  echo "no SEGGER/NXP debug probe found on USB."
  echo "  - is the cable in the MCU-Link port (not the OTG USB-C)?"
  echo "  - plugged straight into the Mac, not through a hub?"
  echo
  echo "USB devices seen, for reference:"
  printf '%s\n' "$dump" |
    sed -n 's/.*"USB Product Name" = "\(.*\)"/  \1/p' | sort -u
  exit 1
fi

for s in $serials; do
  case "$s" in
    "$RMA")  echo "probe $s  ==> *** THIS IS BOARD 3 -- the dead-PHY RMA board ***" ;;
    "$GOOD")  echo "probe $s  ==> board 4 / box3 -- known good (200/200 baseline)" ;;
    "$FIRST") echo "probe $s  ==> board 1 -- the first RW612 (PTP bring-up 2026-07-25," ;
              echo "                 PORTING.md's /dev/cu.usbmodem0010698133301)" ;;
    "$SECOND") echo "probe $s  ==> board 2 -- good (the 2026-07-28 near-RMA; fault was a" ;
              echo "                 partly-seated USB-C, not the board)" ;;
    *)        echo "probe $s  ==> UNKNOWN board -- not one of the four inventoried"
              echo "                 2026-08-14. Not the RMA one by serial; run"
              echo "                 mdio_health before trusting its PHY." ;;
  esac
done
