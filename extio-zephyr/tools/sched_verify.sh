#!/bin/sh
# sched_verify.sh -- post-flash rig check for the k_timer pulse + scheduled
# events (PORTING.md autopsy, 2026-07-23). Run on the dserv host with the
# teensy40 jumpered phys13 -> phys12 (box pin 3 out -> pin 1 in) and pin modes
# set (config/pin/3/mode out, config/pin/1/mode in / in_pullup).
#
# Expected, in the listen output:
#   [set]    di/1=1 then di/1=0                      (baseline, always worked)
#   [pulse]  di/1=1 and di/1=0 per pulse, ~2 ms apart by timestamp -- the pair
#            that NEVER appeared on the counter-based firmware
#   [pulse4s] di/1=1, then di/1=0 four seconds later; watchdog keeps its 1 Hz
#            cadence throughout (nothing blocks)
#   [sched]  state/timer/3 and state/timer/7 stamped +500000 us / +1000000 us
#            after the beginobs anchor, and a di/1 pulse pair at the +500 ms fire
#   [guard]  a sched sent with no beginobs is ignored (console: "no beginobs")
#
# The box name defaults to "box"; pass another as $1.
BOX=${1:-box}
P=extio/$BOX

log=$(mktemp /tmp/sched_verify.XXXXXX)
echo "listening 30s -> $log"
dservctl listen --jsonl --for 30s "$P/state/*" > "$log" 2>&1 &
LPID=$!
sleep 1

echo "[set]     SET 1 / SET 0"
dservctl -c "dservSet $P/cmd/do/3 1";  sleep 0.5
dservctl -c "dservSet $P/cmd/do/3 0";  sleep 0.5

echo "[pulse]   pulse_us 2000 (x2)"
dservctl -c "dservSet $P/cmd/do/3/pulse_us 2000"; sleep 0.5
dservctl -c "dservSet $P/cmd/do/3/pulse_us 2000"; sleep 0.5

echo "[pulse4s] pulse_us 4000000 (watchdog must not stall)"
dservctl -c "dservSet $P/cmd/do/3/pulse_us 4000000"; sleep 5

echo "[sched]   beginobs anchor + do/3/at 500000 + timer/7/at 1000000"
dservctl -c "dservSet $P/config/pin/3/pulse_us 2000"
dservctl -c "dservSet ess/in_obs 1"; sleep 0.2
dservctl -c "dservSet $P/cmd/do/3/at 500000"
dservctl -c "dservSet $P/cmd/timer/7/at 1000000"
sleep 2
dservctl -c "dservSet ess/in_obs 0"

wait $LPID
echo "---- events (di / timer keys) ----"
grep -E 'state/(di/1|timer/)' "$log"
echo "---- watchdog cadence (want ~1.0s gaps, incl. through the 4s pulse) ----"
grep watchdog "$log" | python3 -c "
import sys, json
ts = [json.loads(l)['t'] for l in sys.stdin]
print([round((b-a)/1e6, 2) for a, b in zip(ts, ts[1:])])"
echo "full log: $log"
