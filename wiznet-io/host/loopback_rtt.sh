#!/bin/sh
# loopback_rtt.sh -- DO->DI hardware loopback round-trip for an extio box.
#
# Wire the box's DO <opin> physically to DI <ipin> (e.g. GP1 -> GP2). Run on the
# dserv host the box is registered with (dserv on localhost:4620). Each iteration:
#   dserv sets  <dev>/cmd/do/<opin>  -> pushed to the box over Ethernet
#   box drives the output -> the wire toggles the input
#   box publishes <dev>/state/di/<ipin> -> dserv delivers it back to us
# RT = arrival[now] - command[dservTimestamp], both in dserv's microsecond clock:
#
#   RT = dserv -> box -> drive DO -> wire -> sense DI -> (debounce) -> event -> dserv
#
# This is the FULL closed I/O loop (unlike cmd_rtt, which reads the output's own
# state/do echo and doesn't need the loopback wire).
#
# SETUP on the box first (CLI or config), else you get no samples / inflated ones:
#   pin <opin> mode out
#   pin <ipin> mode in            (or in_pullup)
#   pin <ipin> debounce 0         <-- CRITICAL: a 25 ms debounce dominates every sample
#   pin <ipin> active_low 0       (polarity only matters for the value, not timing)
#
#   sh loopback_rtt.sh <extio/name> [opin] [ipin] [iters]
#   sh loopback_rtt.sh extio/office 1 2 200
#   (box name: `dservGet extio/boxes` or `dservGet extio/primary`)
DEV=${1:?usage: loopback_rtt.sh <extio/name> [opin] [ipin] [iters]}
OPIN=${2:-1}
IPIN=${3:-2}
N=${4:-1000}
SETTLE=0.02

echo "loopback RTT: $DEV   DO$OPIN -> DI$IPIN   n=$N"
echo "------------------------------------------------------------------"
dservctl -c "proc lb_capture {args} { dservSet test/lb_arrival [now] }
dservAddExactMatch $DEV/state/di/$IPIN
dpointSetScript $DEV/state/di/$IPIN lb_capture" >/dev/null

: > /tmp/lb_rtt.txt
i=0
while [ "$i" -lt "$N" ]; do
  dservctl -c "dservSet test/lb_arrival 0" >/dev/null
  D=$(dservctl -c "dservSet $DEV/cmd/do/$OPIN $((i % 2)); dservTimestamp $DEV/cmd/do/$OPIN" | tr -d '[:space:]')
  sleep "$SETTLE"
  A=$(dservctl -c 'dservGet test/lb_arrival' | tr -d '[:space:]')
  RT=$((A - D))
  [ "$RT" -gt 0 ] && [ "$RT" -lt 500000 ] && echo "$RT" >> /tmp/lb_rtt.txt
  i=$((i + 1))
done

dservctl -c "catch {dpointSetScript $DEV/state/di/$IPIN {}}
catch {dservRemoveExactMatch $DEV/state/di/$IPIN}
catch {dservClear test/lb_arrival}" >/dev/null

sort -n /tmp/lb_rtt.txt | awk '{v[NR]=$1; s+=$1}
END{ n=NR; if(n<2){ print "  (insufficient samples -- check: wire DO'"$OPIN"'->DI'"$IPIN"', pin modes, debounce 0)"; exit }
  printf "  min %d  med %d  p90 %d  p99 %d  max %d  mean %.0f us  (n=%d)\n",
         v[1], v[int(n/2)+1], v[int(n*0.9)+1], v[int(n*0.99)], v[n], s/n, n; }'
echo "------------------------------------------------------------------"
echo "min = latency floor; p99/max = the jitter that matters for a rig."
