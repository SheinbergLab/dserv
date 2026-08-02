#!/bin/sh
# ethrx_soak.sh -- reboot soak for the eth RX-wake thread fix.
#
# The failure this hunts: the extio_ethrx wake thread wedging silently at
# boot (~50%/boot on box02 @ v0.4.0+11), leaving dbg/wake_us dead and every
# dserv->box command +~1 ms on the service loop's timeout fallback. The fix
# (deferred srv_conn close + heartbeat self-heal, box_net_eth.c) ships in
# v0.4.0+12; this script is how it gets judged, since one good boot proves
# nothing at 50%.
#
# Per cycle: hardware-reset the box over SWD, wait out DHCP+registration,
# stream no-op commands (cmd/pubq/gather 8 -- 8 is the default, so it only
# exercises the downlink), then read the box's own verdicts:
#   wake_us      in-box signal->recv split. PASS: small number (~30 us on
#                box02). -1 means NO wake was consumed -- with commands
#                flowing that is the dead-wake signature.
#   ethrx_age_ms ms since the wake thread's heartbeat advanced. PASS: <~3 s.
#                Grows without bound when the thread is wedged.
#   ethrx_respawns  0 = never wedged; 1 = wedged AND the self-heal caught it
#                (a heal is a fix working, but log it loudly -- it means the
#                deferred-close did NOT remove the trigger on that boot).
#
# Usage: sh tools/ethrx_soak.sh [n_boots] [dserv_host] [box_prefix] [probe_uid]
# Needs: dservctl on PATH; pyocd in the zephyrproject venv; the MCU-Link USB
# plugged into this machine. PYOCD_PROJECT_DIR is set here so the DFP pin in
# pyocd.yaml (19.0.0 -- newer packs half-brick the board) is in effect.

N=${1:-10}
H=${2:-raspberrypi.local}
B=${3:-extio/box02}
PROBE=${4:-PO1TRFYZDJBU3}      # not "UID": readonly in bash

TOOLS_DIR=$(cd "$(dirname "$0")" && pwd)
export PYOCD_PROJECT_DIR="$TOOLS_DIR/.."

get() { dservctl -H "$H" get "$B/$1" 2>/dev/null; }

pass=0; heal=0; fail=0
for i in $(seq 1 "$N"); do
	pyocd reset -t mcxn947 -u "$PROBE" >/dev/null 2>&1 || {
		echo "boot $i: pyocd reset FAILED"; fail=$((fail+1)); continue; }
	sleep 14                       # DHCP (~1 s fixed) + %reg + first status ticks

	fw=$(get state/fw_ver)
	# 16 commands over ~5 s, so the 1 Hz tick preceding the reads is
	# guaranteed to cover a window WITH traffic (wake_us is windowed and
	# reads -1 for a quiet second -- that is its idle value, not a failure).
	j=0; while [ $j -lt 16 ]; do
		dservctl -H "$H" set "$B/cmd/pubq/gather" 8 >/dev/null 2>&1
		sleep 0.25; j=$((j+1))
	done
	wu=$(get state/dbg/wake_us)
	age=$(get state/dbg/ethrx_age_ms)
	rs=$(get state/dbg/ethrx_respawns)
	sf=$(get state/dbg/ethrx_stack_free)
	cr=$(get state/cmds_rx)
	wd=$(get state/watchdog)

	verdict=FAIL
	if [ -n "$wu" ] && [ "$wu" -ge 0 ] 2>/dev/null && [ "$wu" -lt 1000 ] &&
	   [ "$age" -lt 3000 ] 2>/dev/null; then
		if [ "$rs" = "0" ]; then verdict=PASS; pass=$((pass+1));
		else verdict=HEALED; heal=$((heal+1)); fi
	else
		fail=$((fail+1))
	fi
	echo "boot $i: $verdict fw=$fw wd=$wd wake_us=$wu age_ms=$age" \
	     "respawns=$rs stack_free=$sf cmds_rx=$cr"
done
echo "---"
echo "$N boots: $pass clean, $heal healed-by-respawn, $fail failed"
[ "$fail" -eq 0 ]
