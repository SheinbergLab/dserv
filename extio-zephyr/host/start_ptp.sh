#!/bin/sh
# Restart ptp4l as grandmaster with hardware timestamping.
#
#   sh start_ptp.sh [iface]      (default eth0 -- the rig Pi)
#   sh start_ptp.sh enp86s0      (office-stim, Intel I226-V / igc)
#
# hwts_filter=full is REQUIRED on this NIC. The Pi 5's macb (RP1 Cadence GEM)
# advertises only `none` and `all` RX filter modes -- no per-protocol PTP filter
# -- so ptp4l's default request for HWTSTAMP_FILTER_PTP_V2_EVENT is not honoured
# and every received event message arrives with no timestamp
# ("received SYNC/DELAY_REQ without timestamp"), leaving mean_delay and
# offset_from_tt stuck at 0. `full` requests HWTSTAMP_FILTER_ALL instead.
IFACE=${1:-eth0}
LOG=/tmp/ptp4l.log
for p in $(pgrep -x ptp4l); do kill "$p" 2>/dev/null; done
sleep 1
: > "$LOG"
setsid /usr/sbin/ptp4l -i "$IFACE" -H -m --priority1 127 --hwts_filter full >> "$LOG" 2>&1 &
echo "started ptp4l on $IFACE, pid $! -- log $LOG"
