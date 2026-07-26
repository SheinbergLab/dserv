#!/bin/sh
# Split the loopback into its two legs, off the SAME edge:
#   L1 = arrival(state/do/<opin>) - dservTimestamp(cmd/do/<opin>)   "cmd -> do_echo"
#   L2 = arrival(state/di/<ipin>) - arrival(state/do/<opin>)        "do_echo -> di"
# PORTING.md records 720 / 3-96 against a 783 us total. This says which leg moved.
#
# TEARDOWN USES dpointRemoveScript, NOT `dpointSetScript <dp> {}`. The latter
# does NOT remove the script -- TclServer.cpp does dpoint_scripts.insert(), so an
# EMPTY script is stored and the delivery path still evaluates it on EVERY publish
# of that datapoint, permanently, until dserv restarts. Every harness here used to
# do that, so repeated runs silently accumulated per-publish Tcl dispatch on
# exactly the datapoints being measured. Fixed 2026-07-26.
DEV=${1:?usage: lb_split.sh <extio/name> [opin] [ipin] [iters]}
OPIN=${2:-3}
IPIN=${3:-1}
N=${4:-100}
SETTLE=0.02
: > /tmp/lb_l1.txt
: > /tmp/lb_l2.txt
: > /tmp/lb_tot.txt

dservctl -c "proc lb_do  {args} { dservSet test/lb_do  [now] }
proc lb_di {args} { dservSet test/lb_di [now] }
dservAddExactMatch $DEV/state/do/$OPIN
dservAddExactMatch $DEV/state/di/$IPIN
dpointSetScript $DEV/state/do/$OPIN lb_do
dpointSetScript $DEV/state/di/$IPIN lb_di" >/dev/null

i=0
while [ "$i" -lt "$N" ]; do
  dservctl -c "dservSet test/lb_do 0; dservSet test/lb_di 0" >/dev/null
  D=$(dservctl -c "dservSet $DEV/cmd/do/$OPIN $((i % 2)); dservTimestamp $DEV/cmd/do/$OPIN" | tr -d '[:space:]')
  sleep "$SETTLE"
  R=$(dservctl -c "list [dservGet test/lb_do] [dservGet test/lb_di]" 2>/dev/null)
  set -- $R
  O=${1:-0}; I=${2:-0}
  if [ "$O" -gt 0 ] && [ "$I" -gt 0 ]; then
    L1=$((O - D)); L2=$((I - O))
    [ "$L1" -gt 0 ] && [ "$L1" -lt 500000 ] && echo "$L1" >> /tmp/lb_l1.txt
    [ "$L2" -ge 0 ] && [ "$L2" -lt 500000 ] && echo "$L2" >> /tmp/lb_l2.txt
    T=$((I - D))   # the SAME quantity loopback_rtt.sh reports, measured in this run
    [ "$T" -gt 0 ] && [ "$T" -lt 500000 ] && echo "$T" >> /tmp/lb_tot.txt
  fi
  i=$((i + 1))
done

dservctl -c "catch {dpointRemoveScript $DEV/state/do/$OPIN}
catch {dpointRemoveScript $DEV/state/di/$IPIN}
catch {dservRemoveExactMatch $DEV/state/do/$OPIN}
catch {dservRemoveExactMatch $DEV/state/di/$IPIN}
catch {dservClear test/lb_do}; catch {dservClear test/lb_di}" >/dev/null

for f in /tmp/lb_l1.txt /tmp/lb_l2.txt /tmp/lb_tot.txt; do
  L=$(basename "$f" .txt)
  sort -n "$f" | awk -v l="$L" '{v[NR]=$1; s+=$1}
    END{ n=NR; if(n<2){ printf "  %s: no samples\n", l; exit }
      printf "  %-6s min %6d  med %6d  p90 %6d  p99 %6d  max %7d  mean %6.0f us  (n=%d)\n",
             l, v[1], v[int(n/2)+1], v[int(n*0.9)+1], v[int(n*0.99)+1], v[n], s/n, n; }'
done
