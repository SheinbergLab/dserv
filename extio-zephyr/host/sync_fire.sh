#!/bin/sh
# sync_fire.sh -- tell N PTP-synced boxes to drive a line HIGH at the SAME
# absolute instant, then report what actually happened.
#
# THE POINT
#
# Every other trigger path in this project is EVENT-triggered: dserv says "now"
# and each box fires whenever its packet lands, so simultaneity is bounded by
# transport jitter (~300 us, p99 ~900). This is TIME-triggered: each box is told
# an absolute time T well in advance and schedules locally against its own
# PTP-synced clock. Delivery jitter stops mattering -- a frame only has to arrive
# BEFORE T -- and simultaneity is bounded by clock sync instead (~us).
#
# That is the capability a W6300 box cannot have at any price: its transport is a
# hardwired TCP/IP offload chip with no 1588 clock, so it has nothing to schedule
# against.
#
# PREREQUISITES
#   ptp4l + phc2sys running on this host   (host/start_ptp.sh, start_phc2sys.sh)
#   ptp_anchor.sh run for EACH box         (so each has D = dserv_us - ptp_us)
#   each box: pin <p> mode out
#
#   sh sync_fire.sh <lead_ms> <pin> <extio/name> [<extio/name> ...]
#   sh sync_fire.sh 50 18 extio/box extio/box2
set -e
LEAD_MS=${1:?usage: sync_fire.sh <lead_ms> <pin> <box> [box ...]}
PIN=${2:?usage: sync_fire.sh <lead_ms> <pin> <box> [box ...]}
shift 2
BOXES="$*"
[ -n "$BOXES" ] || { echo "give at least one box" >&2; exit 1; }

# Read a datapoint ONLY if it was stamped at/after <since>; otherwise print
# nothing. Everything this script reports is a box describing itself through a
# last-value store, so "did this value arrive because of what I just sent?" is
# the only question that separates a reply from a leftover.
fresh_get() {   # fresh_get <key> <since_us>
  _ts=$(dservctl -c "dservTimestamp $1" 2>/dev/null | tr -d '[:space:]')
  [ -n "$_ts" ] || return 0
  [ "$_ts" -ge "$2" ] 2>/dev/null || return 0
  dservctl -c "dservGet $1" 2>/dev/null | head -1
}

# dservGet prints a Tcl error ('dpoint "..." not found') on stdout for a missing
# key. Left raw it blows the column layout apart and, worse, reads like data.
get() {         # get <key>  -- value, or empty if absent
  _v=$(dservctl -c "dservGet $1" 2>/dev/null | head -1)
  case "$_v" in *"not found"*|*"no such"*) return 0;; esac
  printf '%s' "$_v"
}
STALE=0

echo "== pre-flight =="
for B in $BOXES; do
  SRC=$(get "$B/state/sync/source")
  OFF=$(get "$B/state/ptp/offset_us")
  printf "  %-16s sync/source=%-6s ptp/offset_us=%s\n" "$B" "${SRC:-?}" "${OFF:-?}"
  case "$SRC" in
    ptp) ;;
    *) echo "    WARNING: not PTP-anchored -- run host/ptp_anchor.sh $B first" >&2 ;;
  esac
done

# Clear the previous level so the rising edge is unambiguous. This must happen
# BEFORE T is chosen: the clear plus its settle costs ~200 ms, and taking T
# first spends that out of the lead. With the old ordering `sync_fire.sh 50`
# handed the boxes a T that was already 150 ms in the past and every box
# correctly answered "late" -- which read as a clock fault rather than a
# harness bug.
for B in $BOXES; do dservctl -c "dservSet $B/cmd/do/$PIN 0" >/dev/null; done
sleep 0.2

# T is chosen ONCE, in dserv time, and sent to every box unchanged. Everything
# downstream is each box converting that same number through its own clock.
NOW=$(dservctl -c 'now' | tr -d '[:space:]')
T=$(( NOW + LEAD_MS * 1000 ))
echo
echo "== firing pin $PIN at T = $T  (now + ${LEAD_MS} ms) =="

# SENT is the cutoff that separates this round's replies from the last round's
# leftovers. dserv is a last-value store with no expiry: a box that never answers
# leaves its PREVIOUS reply sitting in state/sched/*, and reading it back without
# a timestamp check reports a confident "armed" for a box whose Ethernet is
# unplugged. That is not hypothetical -- it happened on 2026-07-26 and cost real
# time, because the stale lead even matched (same lead_ms as the earlier run).
SENT=$(dservctl -c 'now' | tr -d '[:space:]')

for B in $BOXES; do
  dservctl -c "dservSet $B/cmd/do/$PIN/at_abs $T" >/dev/null
done

# Wait past T, plus slack for the reports to come back.
sleep $(echo "$LEAD_MS" | awk '{print ($1/1000.0) + 0.6}')

echo
echo "== what each box reports =="
printf "  %-16s %-8s %-12s %-10s %s\n" BOX STATUS LEAD_us LEVEL "do-stamp - T (us)"
for B in $BOXES; do
  # sched/* is published with timestamp 0, so dserv ARRIVAL-stamps it: a genuine
  # reply to this round must be stamped at or after SENT.
  ERR=$(fresh_get "$B/state/sched/abs_err"    "$SENT")
  LEAD=$(fresh_get "$B/state/sched/abs_lead_us" "$SENT")
  [ -n "$ERR" ] || { ERR="NO-REPLY"; STALE=1; }

  # do/<pin> is different: the box stamps it with the INTENDED time (T), not
  # arrival, so freshness is |stamp - T| rather than a comparison against SENT.
  # A leftover from an earlier round shows up as a wild delta, not a small one.
  LVL=$(get "$B/state/do/$PIN")
  TS=$(dservctl -c "dservTimestamp $B/state/do/$PIN" 2>/dev/null | tr -d '[:space:]')
  if [ -n "$TS" ] && [ "$TS" -gt 0 ] 2>/dev/null; then
    D=$(( TS - T ))
    if [ "$D" -gt 1000000 ] || [ "$D" -lt -1000000 ] 2>/dev/null; then
      D="$D (STALE)"; LVL="?"; STALE=1
    fi
  else
    D="?"
  fi
  printf "  %-16s %-8s %-12s %-10s %s\n" "$B" "$ERR" "${LEAD:--}" "${LVL:-?}" "$D"
done

if [ "$STALE" != "0" ]; then
  cat >&2 <<'WARN'

  *** NO-REPLY / STALE above: at least one box did not answer THIS round. What
  *** is printed for it is whatever was left in dserv from a previous round, not
  *** a result. Do not treat this shot as data. Check the box is registered
  *** (dservKeys extio/<box>/*) and that cmd/* still reaches it.
WARN
fi

cat <<'NOTE'

Reading this:
  STATUS armed  -> the box accepted T and scheduled it
         NO-REPLY -> nothing arrived from this box AFTER the command was sent.
                   The box is gone or deaf; any values shown are leftovers.
         late   -> T had already passed on arrival; the box REFUSED to fire.
                   Increase lead_ms. Firing late silently would be worse.
         unsynced -> box_clock has no anchor yet; run ptp_anchor.sh.
  LEAD_us       -> margin the box actually had. Shrinking lead is the early
                   warning long before anything is actually late.
  do-stamp - T  -> should be ~0: the box stamps the edge with the INTENDED
                   time, so this checks the bookkeeping, NOT the electrical
                   simultaneity.

THE ELECTRICAL TRUTH IS NOT IN THIS TABLE. Everything above is each box
reporting on itself, and this project keeps producing cases where a confident
self-report was a failed measurement in disguise: the PTP counter at 0.15x,
save FAILED(-1), "PHY is still in factory mode" (an all-ones MDIO read),
offset_from_tt 0 ns, pmc's zeroed data sets (a packed/unpacked memcpy), at_abs
truncating T to INT32_MAX, and this script itself reporting a previous round's
"armed" for a box whose cable was out. Every one looked plausible.

Even the scope needs the same discipline: a single reading of ~7 us here looked
fine and was luck -- the distribution was +/-100 us. Take a spread, not a shot.

To measure real simultaneity you need one of:

  * a scope on both pins                       (gold standard)
  * both pins -> two Pi GPIO inputs, which dserv stamps with the kernel's
    gpio_v2_line_event.timestamp_ns (commit b0b1f8d) -- no scope needed, though
    the two IRQ paths add their own jitter to the DIFFERENCE
  * box A's DI input wired to box B's output: A timestamps B's edge at its own
    IRQ, in dserv time, and the error is |t_reported - T| with no host involved
NOTE
