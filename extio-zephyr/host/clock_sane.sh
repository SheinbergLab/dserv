#!/bin/sh
# clock_sane.sh -- does the box's 1588 clock make SENSE? One question, one answer.
#
# This exists because on 2026-07-29 a box ran for hours with its 1588 counter
# free-running from boot -- 56 years wrong -- while every status field reported
# healthy and rig_check.sh scored 11/11. Every PTP field except this one is a
# proxy: sync/source=ptp means a paired READ succeeded, ptp/anchored means ptpconf
# PUSHED D, ptp/window_ns is HOST-side quality. state/ptp/ns is a raw
# ptp_clock_get() of the hardware counter, so it is the only one that cannot lie.
#
#   sh clock_sane.sh [extio/box1 extio/box2 ...]
#
# THREE things are checked, and each catches a failure the others cannot:
#
#   1 FRESH   -- the datapoint was published recently. This matters more than it
#                looks: state/ptp/ns is RETAINED, so after a box reboots dserv
#                still serves the PREVIOUS boot's value. A boot-loop harness that
#                skipped this check scored 8/8 "synced" while measuring nothing.
#                A stale value is reported as UNKNOWN, never as PASS.
#   2 EPOCH   -- an unsynced counter reads seconds-since-boot; a synced one reads
#                seconds-since-1970. 1e9 s is ~2001, so anything below it cannot
#                be a wall clock.
#   3 ~37 s   -- the offset from dserv time should be the TAI-UTC offset, because
#                ptp4l keeps the PHC in TAI while dserv time tracks UTC. Catches a
#                counter that is set but to the wrong time, or mis-scaled. Slop is
#                deliberately loose: this asks "does it make sense", not "how good
#                is the sync" -- for that, watch the offset's STABILITY over time.
#
# Quick check with no tooling at all: the box's boot banner prints
# "PTP hw clock: ready=1  now=<ns>". Synced reads epoch (1.78e18 ns);
# free-running reads single-digit seconds.
BOXES=${*:-"extio/box"}
TAI_UTC=37
SLOP=2
FRESH_S=5
rc=0

for B in $BOXES; do
  # NOTE: the script must RETURN its result. `puts` inside a dserv Tcl script goes
  # to dserv's OWN stdout, not back to the dservctl client, so it reads as empty
  # here and every box would report UNKNOWN.
  R=$(dservctl -c "
    set dp $B/state/ptp/ns
    if {[catch {dservGet \$dp} v]} { set r {NONE 0 0 0} } else {
      set t [dservTimestamp \$dp]
      set r [list OK \$v [expr {([now]-\$t)/1000}] [expr {[now]/1000000}]]
    }
    set r
  " 2>/dev/null | tr -d '\r')
  set -- $R
  ST=$1; NS=$2; AGE_MS=$3; HOST_S=$4

  if [ "$ST" != "OK" ]; then
    echo "  $B  UNKNOWN -- no state/ptp/ns (no 1588 clock, or box never registered)"
    rc=1; continue
  fi

  if [ "$AGE_MS" -gt $((FRESH_S * 1000)) ] 2>/dev/null; then
    echo "  $B  UNKNOWN -- ptp/ns is $((AGE_MS / 1000))s STALE (retained; box may be down)"
    rc=1; continue
  fi

  PS=$((NS / 1000000000))
  if [ "$PS" -lt 1000000000 ] 2>/dev/null; then
    # TOO EARLY is not the same as WRONG. Convergence takes 6-10 s from boot
    # (measured, 16 boots), so a young box legitimately reads uptime for a moment.
    # Reporting that as FAIL sends you debugging a healthy box -- which it did to me
    # about a minute after this script was written.
    UP=$(dservctl -c "catch {dservGet $B/state/watchdog} v; set v" 2>/dev/null | tr -d '\r')
    case "$UP" in ''|*[!0-9]*) UP=999;; esac
    if [ "$UP" -lt 20 ]; then
      echo "  $B  TOO EARLY -- up ${UP}s, ptp/ns still uptime. Sync lands at 6-10 s;"
      echo "        re-run in a few seconds before believing anything."
      rc=1; continue
    fi
    echo "  $B  FAIL -- ptp/ns = ${PS}s is UPTIME, not epoch: counter is FREE-RUNNING"
    echo "        after ${UP}s, which is long past the 6-10 s it should take."
    echo "        Every box-stamped timestamp is garbage and at_abs fires at an"
    echo "        arbitrary time. Most likely causes, in order:"
    echo "        1. patches/enet-qos-rx-timestamp-race.patch is NOT applied to the"
    echo "           Zephyr tree this image was built from (a west update erases it)."
    echo "        2. The PTP transport does not match on both ends. Box and ptp4l must"
    echo "           agree; a mismatch is SILENT. Default is UDP/IPv4 on both, i.e. no"
    echo "           CONFIG_PTP_*_PROTOCOL override and no -2 on ptp4l."
    echo "        3. No grandmaster reachable: check dserv-ptp4l@<iface> is running."
    echo "        Enable CONFIG_NET_LOG + CONFIG_PTP_LOG_LEVEL_INF and look for"
    echo "        'drops Sync without valid RX timestamp' -- that names cause 1."
    rc=1; continue
  fi

  D=$((PS - HOST_S)); [ "$D" -lt 0 ] && D=$((-D))
  LO=$((TAI_UTC - SLOP)); HI=$((TAI_UTC + SLOP))
  if [ "$D" -ge "$LO" ] && [ "$D" -le "$HI" ] 2>/dev/null; then
    echo "  $B  OK -- epoch, ${D}s ahead of dserv time (TAI-UTC = ${TAI_UTC}s), fresh ${AGE_MS}ms"
  else
    echo "  $B  FAIL -- epoch but ${D}s from dserv time, expected ~${TAI_UTC} (TAI-UTC)."
    echo "        Counter is SET but to the wrong time, or its rate is mis-scaled."
    rc=1
  fi
done

exit $rc
