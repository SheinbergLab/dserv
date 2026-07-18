#!/bin/sh
# obs_soak.sh -- generate obs begin/end cycles WITHOUT running a state system,
# by driving ::ess::begin_obs / ::ess::end_obs in the ess interp via dservctl.
# Each edge is the real thing: rpioPinOn/Off toggles the rig's obs TTL and
# dservSet ess/in_obs follows -- i.e. genuine sync anchors for extio boxes.
#
# Pair with a capture + the analyzer:
#   dservctl --host <rig> listen --jsonl --for 10m \
#       "extio/*/state/sync/*" "ess/in_obs" > sync_run.jsonl &
#   sh obs_soak.sh -H <rig> -n 80 -o 4 -i 3
#   python3 sync_analyze.py sync_run.jsonl
#
# Options:
#   -H host     dserv host (default localhost)
#   -n cycles   number of obs periods (default 100)
#   -o secs     obs duration (default 4)
#   -i secs     inter-obs interval (default 3)
#
# Notes: requires ::ess::obs_pin configured on the rig (it is wherever the
# TTL is wired). Don't run while a state system is generating obs periods --
# a double-begin makes no TTL edge and pollutes the hw/sw stats. Cadence
# jitter from `sleep` is irrelevant: anchors care about edges, not spacing.

HOST=localhost; N=100; OBS=4; ITI=3
while getopts H:n:o:i: f; do
  case $f in
    H) HOST=$OPTARG;;
    n) N=$OPTARG;;
    o) OBS=$OPTARG;;
    i) ITI=$OPTARG;;
    *) echo "usage: obs_soak.sh [-H host] [-n cycles] [-o obs_s] [-i iti_s]" >&2; exit 2;;
  esac
done

# leave the rig out of an obs on Ctrl-C, whatever phase we were in
trap 'dservctl --host "$HOST" ess "::ess::end_obs 0" >/dev/null 2>&1; echo; echo "stopped (obs closed)." >&2; exit 0' INT TERM

i=0
while [ "$i" -lt "$N" ]; do
  dservctl --host "$HOST" ess "::ess::begin_obs $i $N" >/dev/null || { echo "begin_obs failed (obs_pin set? host up?)" >&2; exit 1; }
  sleep "$OBS"
  dservctl --host "$HOST" ess "::ess::end_obs 1" >/dev/null || exit 1
  sleep "$ITI"
  i=$((i + 1))
  printf '\rcycle %d/%d' "$i" "$N" >&2
done
echo >&2
