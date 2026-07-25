#!/bin/sh
# lb_budget.sh -- decompose the loopback using the box's own edge stamps.
#
# Five instants per iteration, three clocks' worth of care:
#   cmd_ts  dservTimestamp(cmd/do/<o>)   host  -- dserv issued the command
#   do_ts   dservTimestamp(state/do/<o>) BOX   -- the pin physically moved
#   do_arr  [now] in the echo callback   host  -- dserv learned of the actuation
#   di_ts   dservTimestamp(state/di/<i>) BOX   -- the input edge
#   di_arr  [now] in the di callback     host  -- dserv learned of the edge
#
#   OUT = do_ts  - cmd_ts    dserv issue -> pin moved      (CLOCK-CROSSING)
#   RET = do_arr - do_ts     pin moved   -> dserv knew     (CLOCK-CROSSING)
#   L1  = OUT + RET          offset cancels -- trustworthy
#   QUE = (di_arr-do_arr) - (di_ts-do_ts)  the DI's wait behind the echo
#   TOT = di_arr - cmd_ts    the whole loop
#
# CAVEAT that governs OUT/RET: with a `sw` sync anchor the box clock is set from
# a frame ARRIVAL, so it sits ~one-way-transport BEHIND true dserv time. Every
# box stamp then reads early by that d, which UNDER-states OUT and OVER-states
# RET by exactly d. Their SUM is unaffected. Do not read the split as physical
# until the box has a hw (TTL-edge) anchor.
DEV=${1:?usage: lb_budget.sh <extio/name> [opin] [ipin] [iters]}
OPIN=${2:-10}
IPIN=${3:-11}
N=${4:-60}
SETTLE=0.02
for f in out ret l1 que tot; do : > /tmp/b_$f.txt; done

dservctl -c "proc b_do {args} { dservSet test/b_do [now] }
proc b_di {args} { dservSet test/b_di [now] }
dservAddExactMatch $DEV/state/do/$OPIN
dservAddExactMatch $DEV/state/di/$IPIN
dpointSetScript $DEV/state/do/$OPIN b_do
dpointSetScript $DEV/state/di/$IPIN b_di" >/dev/null

i=0
while [ "$i" -lt "$N" ]; do
  dservctl -c "dservSet test/b_do 0; dservSet test/b_di 0" >/dev/null
  C=$(dservctl -c "dservSet $DEV/cmd/do/$OPIN $((i % 2)); dservTimestamp $DEV/cmd/do/$OPIN" | tr -d '[:space:]')
  sleep "$SETTLE"
  R=$(dservctl -c "list [dservTimestamp $DEV/state/do/$OPIN] [dservTimestamp $DEV/state/di/$IPIN] [dservGet test/b_do] [dservGet test/b_di]" 2>/dev/null)
  set -- $R
  DOTS=${1:-0}; DITS=${2:-0}; DOAR=${3:-0}; DIAR=${4:-0}
  if [ "$C" -gt 0 ] && [ "$DOTS" -gt 0 ] && [ "$DITS" -gt 0 ] && [ "$DOAR" -gt 0 ] && [ "$DIAR" -gt 0 ]; then
    OUT=$((DOTS - C)); RET=$((DOAR - DOTS)); L1=$((DOAR - C))
    QUE=$(( (DIAR - DOAR) - (DITS - DOTS) )); TOT=$((DIAR - C))
    for p in "out $OUT" "ret $RET" "l1 $L1" "que $QUE" "tot $TOT"; do
      set -- $p
      [ "$2" -gt -500000 ] && [ "$2" -lt 500000 ] && echo "$2" >> /tmp/b_$1.txt
    done
  fi
  i=$((i + 1))
done

dservctl -c "catch {dpointSetScript $DEV/state/do/$OPIN {}}
catch {dpointSetScript $DEV/state/di/$IPIN {}}
catch {dservRemoveExactMatch $DEV/state/do/$OPIN}
catch {dservRemoveExactMatch $DEV/state/di/$IPIN}
catch {dservClear test/b_do}; catch {dservClear test/b_di}" >/dev/null

for f in out ret l1 que tot; do
  sort -n /tmp/b_$f.txt | awk -v l="$f" '{v[NR]=$1; s+=$1}
    END{ n=NR; if(n<2){ printf "  %-4s no samples\n", l; exit }
      printf "  %-4s min %7d  med %7d  p90 %7d  p99 %7d  max %7d  mean %7.0f us  (n=%d)\n",
             l, v[1], v[int(n/2)+1], v[int(n*0.9)+1], v[int(n*0.99)+1], v[n], s/n, n; }'
done
