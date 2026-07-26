#!/bin/sh
# ptp_anchor.sh -- push the PHC->dserv constant to a PTP-synced box.
#
#   dserv_us = phc_us - (phc_minus_mono_us) + dservClockEpochOffset
#   => D     = dserv_us - ptp_us = dservClockEpochOffset - phc_minus_mono_us
#
# D is CONSTANT: dserv time is CLOCK_MONOTONIC plus a fixed epoch offset, and
# phc2sys rate-locks the PHC to that same clock (0.002 ppm). So this is computed
# from two LOCAL clock reads -- no wire, no one-way delay, no asymmetry to model.
# That is the whole reason this is better than an obs anchor.
#
# The box re-anchors itself from PTP once a second using D, costing no packets.
# This only needs re-running when D itself moves: at 0.002 ppm that is roughly
# every 400 s to stay inside 1 us. Run it from cron/a timer, or once a session.
#
# PREREQUISITES (both on the host, see host/start_ptp.sh and start_phc2sys.sh):
#   ptp4l   -- disciplines the box's 1588 clock to this host's PHC
#   phc2sys -- rate-locks the PHC to the system clock; WITHOUT IT the PHC
#              free-runs at ~46 ppm and D is not constant at all.
#
#   sh ptp_anchor.sh <extio/name> [/dev/ptpN]
set -e
DEV=${1:?usage: ptp_anchor.sh <extio/name> [/dev/ptpN] [iface]}
PHC=${2:-}
IFACE=${3:-eth0}
HERE=$(dirname "$0")

# Resolve the PHC from the interface -- do NOT hardcode /dev/ptp0. The index is
# assigned at driver probe and is NOT stable across reboots: this rig moved from
# ptp0 to ptp1 after a Pi restart, and a hardcoded path then silently measured
# the wrong clock (or, here, failed outright).
if [ -z "$PHC" ]; then
  IDX=$(sudo ethtool -T "$IFACE" 2>/dev/null | awk '/provider index/ {print $NF}')
  [ -n "$IDX" ] || IDX=$(ethtool -T "$IFACE" 2>/dev/null | awk '/provider index/ {print $NF}')
  [ -n "$IDX" ] || { echo "cannot resolve PHC index for $IFACE" >&2; exit 1; }
  PHC=/dev/ptp$IDX
  echo "PHC for $IFACE: $PHC"
fi

command -v "$HERE/phc_offset" >/dev/null 2>&1 || [ -x "$HERE/phc_offset" ] || {
  echo "build it first:  cc -O2 -Wall -o $HERE/phc_offset $HERE/phc_offset.c" >&2
  exit 1
}

# PHC - CLOCK_MONOTONIC, in ns, min-filtered (least-interrupted sample).
# Only this step needs root (/dev/ptp0 is 0600 root:root); dservctl runs as you.
if [ -r "$PHC" ]; then
  PHC_MONO_NS=$("$HERE/phc_offset" --once "$PHC")
else
  PHC_MONO_NS=$(sudo "$HERE/phc_offset" --once "$PHC")
fi
PHC_MONO_US=$(( PHC_MONO_NS / 1000 ))

# dserv's own epoch constant. Exposed by dservClockEpochOffset.
K_US=$(dservctl -c 'dservClockEpochOffset' | tr -d '[:space:]')

D_US=$(( K_US - PHC_MONO_US ))

echo "phc - mono : $PHC_MONO_US us"
echo "dserv epoch: $K_US us"
echo "D          : $D_US us   (dserv_us - ptp_us)"

dservctl -c "dservSet $DEV/cmd/ptp/offset $D_US" >/dev/null
sleep 1

echo
echo "box reports:"
for k in ptp/offset_us sync/source sync/offset_us sync/ptp_window_us; do
  printf "  %-20s %s\n" "$k" "$(dservctl -c "dservGet $DEV/state/$k" 2>&1 | head -1)"
done
