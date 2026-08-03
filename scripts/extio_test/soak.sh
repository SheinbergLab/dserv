#!/bin/sh
# soak.sh -- Stage 5: long-run statistics under the FROZEN gates.
#
# Loops full certification cycles (all six variants, fresh datafiles,
# extract+analyze+verdict per session) until stopped, tallying per-cycle
# results and every failure line into one log. ~20 min per cycle; an
# overnight run is ~25 cycles / ~5500 trials across every axis.
#
#   ./soak.sh [--box NAME] [--cycles N] [--log FILE]
#
# Stop anytime:  touch ~/soak.stop     (finishes the current cycle)
# Watch:         tail -f <log>
# Summary line:  grep CYCLE <log> | tail -5

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BOX=box02
CYCLES=0            # 0 = until ~/soak.stop
LOG="$HOME/soak_$(date +%y%m%d_%H%M).log"

while [ $# -gt 0 ]; do
    case "$1" in
        --box) BOX="$2"; shift 2 ;;
        --cycles) CYCLES="$2"; shift 2 ;;
        --log) LOG="$2"; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

rm -f "$HOME/soak.stop"
echo "==== extio_test soak: box=$BOX cycles=${CYCLES:-until-stopped} log=$LOG ====" | tee -a "$LOG"
echo "started $(date)" | tee -a "$LOG"

pass=0; fail=0; n=0
while :; do
    n=$((n+1))
    [ "$CYCLES" -gt 0 ] && [ $n -gt "$CYCLES" ] && break
    [ -f "$HOME/soak.stop" ] && { echo "soak.stop seen -- ending" | tee -a "$LOG"; break; }

    echo "---- cycle $n starting $(date) ----" >> "$LOG"
    if "$HERE/certify.sh" --box "$BOX" --timeout 700 >> "$LOG" 2>&1; then
        pass=$((pass+1)); r=PASS
    else
        fail=$((fail+1)); r=FAIL
    fi
    # cumulative one-liner (the greppable heartbeat)
    echo "CYCLE $n: $r   (cum: $pass pass / $fail fail)   $(date)" | tee -a "$LOG"

    # box health snapshot between cycles
    {
        echo "  wd=$(dservctl get extio/$BOX/state/watchdog 2>/dev/null)"
        echo "  sync=$(dservctl get extio/$BOX/state/sync/source 2>/dev/null) offset=$(dservctl get extio/$BOX/state/sync/offset_us 2>/dev/null)"
        echo "  fifo_drop=$(dservctl get extio/$BOX/state/dbg/di_fifo_drop 2>/dev/null) late_gap_max=$(dservctl get extio/$BOX/state/ain/dbg/late_gap_max_us 2>/dev/null)"
        echo "  pub_ev_drop=$(dservctl get extio/$BOX/state/dbg/pub_ev_drop 2>/dev/null) loop_max=$(dservctl get extio/$BOX/state/dbg/loop_max_us 2>/dev/null)"
    } >> "$LOG"
done

echo "==== soak done: $n cycles, $pass pass / $fail fail  $(date) ====" | tee -a "$LOG"
[ $fail -eq 0 ]
