#!/bin/sh
# rig_check.sh -- is the rig actually ready to take a measurement?
#
# Every check here exists because its absence cost real time on 2026-07-26/27.
# The ordering is deliberate: each step only makes sense if the previous passed,
# and each one distinguishes a failure that the previous one CANNOT see.
#
#   1 reachable      ping           -- box is on the wire at all
#   2 registered     key count      -- dserv knows it
#   3 UPLINK alive   watchdog MOVES -- box's service loop is running.
#                                      A present-but-frozen watchdog means the
#                                      loop is starved while the net stack still
#                                      answers pings (that is what a 1 us tick
#                                      did). Presence is not enough; it must
#                                      ADVANCE.
#   4 DOWNLINK alive cmds_rx MOVES  -- cmd/* actually arrives. This is the
#                                      "publishing but deaf" check: state/*
#                                      flows, every status field reads healthy,
#                                      and commands silently never land. Cost
#                                      hours twice before cmds_rx existed.
#   5 anchored       sync/source    -- box_clock has a PTP anchor. Without it
#                                      at_abs REFUSES to fire (correctly), so an
#                                      unanchored box drops events rather than
#                                      firing late. Anchors do NOT survive a
#                                      box reboot.
#   6 PTP quality    pmc offset     -- host-side truth, not the box's self-report
#   7 fire           sync_fire      -- the whole chain, end to end
#
#   sh rig_check.sh [extio/box1 extio/box2 ...]
BOXES=${*:-"extio/box1 extio/box2"}
PASS=0; FAIL=0
ok()   { echo "  PASS  $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL  $1"; FAIL=$((FAIL+1)); }

get() { _v=$(dservctl -c "dservGet $1" 2>/dev/null | head -1)
        case "$_v" in *"not found"*) return 0;; esac; printf '%s' "$_v"; }

echo "== 1-2 reachable + registered =="
for B in $BOXES; do
  IP=$(get "$B/state/net/ip")
  N=$(dservctl -c "dservKeys $B/*" 2>/dev/null | tr ' ' '\n' | grep -c "$B/")
  if [ -n "$IP" ] && ping -c1 -W1 "$IP" >/dev/null 2>&1; then ok "$B reachable at $IP"
  else bad "$B not reachable (ip=${IP:-none})"; fi
  [ "$N" -gt 20 ] && ok "$B registered ($N keys)" || bad "$B only $N keys -- not registered"
done

echo "== 3-4 uplink + downlink ALIVE (both must ADVANCE, not merely exist) =="
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
for B in $BOXES; do
  K=$(echo "$B" | tr '/' '_')
  get "$B/state/watchdog" > "$TMP/$K.w0"
  get "$B/state/cmds_rx"  > "$TMP/$K.c0"
done
sleep 3
for B in $BOXES; do dservctl -c "dservSet $B/cmd/do/18 0" >/dev/null 2>&1; done
sleep 1
for B in $BOXES; do
  K=$(echo "$B" | tr '/' '_')
  W0=$(cat "$TMP/$K.w0"); C0=$(cat "$TMP/$K.c0")
  W1=$(get "$B/state/watchdog"); C1=$(get "$B/state/cmds_rx")

  if [ -n "$W0" ] && [ -n "$W1" ] && [ "$W1" -gt "$W0" ] 2>/dev/null; then
    ok "$B watchdog $W0->$W1 (service loop RUNNING)"
  else
    bad "$B watchdog stuck at ${W1:-none} -- loop starved or box gone (ping may still work!)"
  fi

  if [ -n "$C0" ] && [ -n "$C1" ] && [ "$C1" -gt "$C0" ] 2>/dev/null; then
    ok "$B cmds_rx $C0->$C1 (downlink DELIVERING)"
  else
    bad "$B cmds_rx stuck at ${C1:-none} -- PUBLISHING BUT DEAF"
  fi
done

echo "== 5 PTP anchored =="
for B in $BOXES; do
  S=$(get "$B/state/sync/source")
  [ "$S" = "ptp" ] && ok "$B sync/source=ptp" \
    || bad "$B sync/source=${S:-none} -- run ptp_anchor.sh (at_abs will REFUSE to fire)"
done

echo "== 6 PTP quality (host-side, via pmc) =="
if sudo -n /usr/sbin/pmc -i eth0 -b 1 -t 1 "GET CURRENT_DATA_SET" 2>/dev/null \
     | grep -E 'RESPONSE|offsetFromMaster'; then :; else
  echo "  (pmc needs sudo -- see the 99-rig sudoers drop-in)"
fi

echo "== 7 end-to-end scheduled fire =="
OUT=$(sh "$(dirname "$0")/sync_fire.sh" 150 18 $BOXES 2>&1)
for B in $BOXES; do
  ST=$(echo "$OUT" | awk -v b="$B" '$1==b {print $2}' | tail -1)
  [ "$ST" = "armed" ] && ok "$B armed and fired" || bad "$B fire status=${ST:-none}"
done

echo
echo "  $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
