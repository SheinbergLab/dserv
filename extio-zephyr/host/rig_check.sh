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
#   5 right firmware boot + fw_ver  -- is this the image someone CHOSE? A box
#                                      that rolled back is healthy by every
#                                      other check here and running old code.
#                                      AFTER 3-4 on purpose: these are RETAINED
#                                      datapoints written by the announce burst,
#                                      so on a box that is still booting they
#                                      describe the PREVIOUS connection -- read
#                                      too early they reported the pre-reflash
#                                      firmware. cmds_rx advancing is what proves
#                                      the connection that wrote them is the
#                                      current one.
#   6 anchored       sync/source    -- box_clock has a PTP anchor. Without it
#                                      at_abs REFUSES to fire (correctly), so an
#                                      unanchored box drops events rather than
#                                      firing late. Anchors do NOT survive a
#                                      box reboot.
#   7 anchoring SVC  ptp/*          -- is the thing that MAINTAINS anchors alive?
#                                      Step 6 is a box's own view at one instant
#                                      and says nothing about whether anchoring
#                                      will keep working. If ptpconf dies or
#                                      starts refusing on a bad window, boxes
#                                      drift silently until someone notices.
#                                      ptp/anchored (boxes actually reporting)
#                                      differing from ptp/boxes (boxes pushed to)
#                                      is the 2026-07-28 failure, made visible.
#   8 PTP quality    pmc offset     -- host-side truth, not the box's self-report.
#                                      Interface auto-resolved (RIG_IFACE overrides):
#                                      eth0 on the Pi, enp86s0 at the office.
#   9 fire           sync_fire      -- the whole chain, end to end
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

echo "== 5 firmware identity + boot outcome =="
for B in $BOXES; do
  FV=$(get "$B/state/fw_ver")
  BT=$(get "$B/state/boot")
  TR=$(get "$B/state/ota/trial")
  RV=$(get "$B/state/ota/reverts")
  AV=$(get "$B/state/ota/last_arm_ver")
  if [ -z "$FV" ]; then
    echo "  SKIP  $B has no bootloader (no state/fw_ver) -- firmware is whatever was flashed"
    continue
  fi
  case "$BT" in
    revert)
      bad "$B ROLLED BACK to v$FV -- trial v$AV was never confirmed ($RV lifetime)" ;;
    rejected)
      bad "$B REFUSED v$AV and is still v$FV -- bad signature, header, or offset" ;;
    *)
      ok "$B running v$FV (boot=$BT)" ;;
  esac
  # A box left mid-trial passes every other check in this script and then
  # reverts on the next reset, so the firmware a run assumed is not the
  # firmware it ends on.
  [ "$TR" = "1" ] && bad "$B is ON TRIAL -- next reset REVERTS it (send cmd/ota/confirm)" \
                  || ok "$B image confirmed"
done

echo "== 6 PTP anchored =="
for B in $BOXES; do
  S=$(get "$B/state/sync/source")
  [ "$S" = "ptp" ] && ok "$B sync/source=ptp" \
    || bad "$B sync/source=${S:-none} -- run ptp_anchor.sh (at_abs will REFUSE to fire)"
done

# 6b THE CLOCK IS ACTUALLY RIGHT -- added 2026-07-29 because THIS SCRIPT SCORED
# 11/11 ON A BOX WHOSE 1588 COUNTER WAS 56 YEARS OFF, free-running from boot.
#
# Every other PTP check here is a PROXY for the thing we care about:
#   sync/source=ptp  is set when a paired READ succeeds, not when the clock is right
#   ptp/anchored=1   means ptpconf successfully PUSHED D, not that D landed on a
#                    correct counter
#   ptp/window_ns    is HOST-side PHC read quality, nothing to do with the box
#   step 9 "fired"   checked THAT it fired, never WHEN
#
# state/ptp/ns is a raw ptp_clock_get() of the box's hardware counter, so it is the
# one field no amount of anchoring can fake. An unsynced counter reads
# seconds-since-boot; a synced one reads epoch. Comparing to [now] catches both the
# gross failure and a mis-scaled counter, and the tolerance is deliberately loose
# (2 s) because we are excluding "wrong by decades", not measuring sync quality --
# TAI-UTC alone puts a correct box 37 s ahead of dserv's UTC-based clock.
echo "== 6b PTP clock is ACTUALLY SET (not just anchored) =="
for B in $BOXES; do
  PN=$(get "$B/state/ptp/ns")
  if [ -z "$PN" ]; then
    echo "  SKIP  $B has no state/ptp/ns (no 1588 clock on this board?)"
    continue
  fi
  # seconds since boot vs seconds since the epoch -- 1e9 s ~= 2001, so anything
  # below it cannot be a real wall clock.
  PS=$((PN / 1000000000))
  HS=$(dservctl -c "expr {[now]/1000000}" 2>/dev/null | tr -d '[:space:]')
  if [ "$PS" -lt 1000000000 ] 2>/dev/null; then
    bad "$B ptp/ns=${PS}s is UPTIME, not epoch -- 1588 counter is FREE-RUNNING.
        Every box-stamped event timestamp is garbage and at_abs fires at an
        arbitrary time. Check the PTP TRANSPORT matches on both ends: this board
        needs CONFIG_PTP_IEEE_802_3_PROTOCOL + ptp4l -2 (RX timestamps are never
        valid over UDP/IPv4 on ENET_QOS). Enable CONFIG_PTP_LOG_LEVEL_INF and look
        for 'drops Sync without valid RX timestamp'."
  else
    D=$((PS - HS)); [ "$D" -lt 0 ] && D=$((-D))
    # 37 s of TAI-UTC is expected and correct; 2 s of slop on top of it.
    if [ "$D" -le 39 ] 2>/dev/null; then
      ok "$B ptp/ns is epoch, ${D}s from dserv time (TAI-UTC is 37s => correct)"
    else
      bad "$B ptp/ns is epoch but ${D}s from dserv time -- expected ~37 (TAI-UTC).
        Counter is set but to the WRONG time, or its rate is mis-scaled."
    fi
  fi
done

echo "== 7 anchoring service (ptpconf) =="
PTP_D=$(get "ptp/d_us")
if [ -z "$PTP_D" ]; then
  echo "  SKIP  ptpconf not running (no ptp/d_us) -- anchors are manual here"
else
  PTP_ERR=$(get "ptp/error")
  PTP_ANCH=$(get "ptp/anchored")
  PTP_STEP=$(get "ptp/step_us")
  PTP_WIN=$(get "ptp/window_ns")
  NBOX=$(echo "$BOXES" | wc -w | tr -d ' ')

  [ -z "$PTP_ERR" ] && ok "ptpconf healthy (D=$PTP_D us, window=${PTP_WIN}ns)" \
                    || bad "ptpconf error: $PTP_ERR"

  # Report BOTH numbers. anchored counts boxes reporting sync/source=ptp; boxes
  # counts LIVE boxes ptpconf pushed to. They differ exactly when a push is not
  # landing -- and until the 2026-07-28 soak, `boxes` silently included a box
  # that had been unplugged all night, which inflated `anchored` by the same
  # amount and would have masked a real anchor loss here.
  PTP_BOXES=$(get "ptp/boxes")
  if [ -z "$PTP_ANCH" ] || [ "$PTP_ANCH" -lt "$NBOX" ] 2>/dev/null; then
    bad "ptp/anchored=${PTP_ANCH:-none} < $NBOX under test -- a push is not landing"
  elif [ -n "$PTP_BOXES" ] && [ "$PTP_ANCH" -lt "$PTP_BOXES" ] 2>/dev/null; then
    bad "ptp/anchored=$PTP_ANCH of $PTP_BOXES live box(es) -- one is NOT anchored"
  else
    ok "ptp/anchored=$PTP_ANCH of ${PTP_BOXES:-?} live, covers the $NBOX under test"
  fi

  # Only ever published when D jumped past the step threshold. Its presence means
  # every box was anchored to a timeline that moved; drift never sets this.
  [ -z "$PTP_STEP" ] && ok "no clock step detected" \
                     || bad "CLOCK STEP of $PTP_STEP us -- re-anchor and distrust timestamps across it"
fi

echo "== 8 PTP quality (host-side, via pmc) =="
# The interface is NOT eth0 everywhere -- the rig Pi has eth0, office-stim's I226
# enumerates as enp86s0 -- and `-i eth0` there fails with "No such device" while
# the old message blamed sudo. Three causes (no pmc / no sudo / wrong iface)
# collapsed into one wrong answer, which is the exact failure this whole script
# exists to prevent. Resolve the interface, and name the real cause.
#
# Order of truth: an explicit RIG_IFACE, else the interface ptp4l is ACTUALLY
# running on (the unit is templated on it), else one that has a PHC, else the
# default route.
IFACE=${RIG_IFACE:-}
[ -z "$IFACE" ] && IFACE=$(systemctl list-units --no-legend --no-pager 'dserv-ptp4l@*' 2>/dev/null \
                           | sed -n 's/^[^a-zA-Z]*dserv-ptp4l@\([^.]*\)\.service.*/\1/p' | head -1)
# ...or the -i of a ptp4l that is running without our unit (manual bring-up).
[ -z "$IFACE" ] && IFACE=$(pgrep -af '[p]tp4l' 2>/dev/null \
                           | sed -n 's/.*-i[ =]\{1,\}\([^ ]*\).*/\1/p' | head -1)
if [ -z "$IFACE" ]; then
  for d in /sys/class/net/*/device/ptp*; do
    [ -e "$d" ] || continue
    IFACE=$(echo "$d" | cut -d/ -f5); break
  done
fi
# LAST resort, and deliberately last: the default route is WRONG on the rig Pi,
# whose default is wlan0 while PTP runs on eth0. Every source above names the
# interface PTP is actually on; this one only names the way out to the internet.
[ -z "$IFACE" ] && IFACE=$(ip -o -4 route show default 2>/dev/null | awk '{print $5}' | head -1)

PMC=$(command -v pmc 2>/dev/null || echo /usr/sbin/pmc)
if [ ! -x "$PMC" ]; then
  echo "  SKIP  pmc not installed (apt install linuxptp) -- no host-side PTP check"
elif [ -z "$IFACE" ]; then
  echo "  SKIP  no PTP interface found -- set RIG_IFACE=<iface> (see: ip -br link)"
else
  OUT=$(sudo -n "$PMC" -i "$IFACE" -b 1 -t 1 "GET CURRENT_DATA_SET" 2>&1)
  case "$OUT" in
    *offsetFromMaster*)
      echo "  (via $IFACE)"; echo "$OUT" | grep -E 'RESPONSE|offsetFromMaster' ;;
    *"No such device"*)
      echo "  SKIP  interface '$IFACE' does not exist -- set RIG_IFACE=<iface>" ;;
    *password*|*"no tty"*|*sudo:*)
      echo "  SKIP  pmc needs passwordless sudo -- add /etc/sudoers.d/99-rig" ;;
    *)
      echo "  SKIP  pmc on $IFACE: $(echo "$OUT" | head -1)" ;;
  esac
fi

echo "== 9 end-to-end scheduled fire =="
OUT=$(sh "$(dirname "$0")/sync_fire.sh" 150 18 $BOXES 2>&1)
for B in $BOXES; do
  ST=$(echo "$OUT" | awk -v b="$B" '$1==b {print $2}' | tail -1)
  [ "$ST" = "armed" ] && ok "$B armed and fired" || bad "$B fire status=${ST:-none}"
done

echo
echo "  $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
