#!/bin/sh
# dispatch_test.sh -- dserv's OWN datapoint->script dispatch latency, no box,
# no transport, no wire. Tests the claim that a callback with nothing dispatched
# just before it pays a large cold-wakeup cost, while one following ~us behind
# another is nearly free. That is the asymmetry the loopback harnesses disagree
# over; if it is real it must show up here, where the only moving part is dserv.
#
#   COLD: only test/ping carries a script.        latency = pong - t0
#   WARM: test/warm carries a script and is set   latency = pong - t0
#         immediately before test/ping, so ping's
#         dispatch follows another by microseconds.
#
# Both measure the same span, t0 -> the [now] taken inside ping's callback.
N=${1:-60}
: > /tmp/disp_cold.txt
: > /tmp/disp_warm.txt

run () {   # $1 = mode (cold|warm), $2 = outfile
  MODE=$1; OUT=$2
  if [ "$MODE" = warm ]; then
    dservctl -c "proc dt_warm {args} { dservSet test/dt_warm_at [now] }
proc dt_ping {args} { dservSet test/dt_pong [now] }
dservAddExactMatch test/dt_warm
dservAddExactMatch test/dt_ping
dpointSetScript test/dt_warm dt_warm
dpointSetScript test/dt_ping dt_ping" >/dev/null
  else
    dservctl -c "proc dt_ping {args} { dservSet test/dt_pong [now] }
dservAddExactMatch test/dt_ping
dpointSetScript test/dt_ping dt_ping" >/dev/null
  fi

  i=0
  while [ "$i" -lt "$N" ]; do
    dservctl -c "dservSet test/dt_pong 0" >/dev/null
    if [ "$MODE" = warm ]; then
      dservctl -c "set t0 [now]; dservSet test/dt_warm 1; dservSet test/dt_ping $i; dservSet test/dt_t0 \$t0" >/dev/null
    else
      dservctl -c "set t0 [now]; dservSet test/dt_ping $i; dservSet test/dt_t0 \$t0" >/dev/null
    fi
    sleep 0.05
    R=$(dservctl -c "list [dservGet test/dt_t0] [dservGet test/dt_pong]" 2>/dev/null)
    set -- $R
    T0=${1:-0}; P=${2:-0}
    if [ "$P" -gt 0 ] && [ "$T0" -gt 0 ]; then
      L=$((P - T0))
      [ "$L" -gt 0 ] && [ "$L" -lt 500000 ] && echo "$L" >> "$OUT"
    fi
    i=$((i + 1))
  done

  dservctl -c "catch {dpointSetScript test/dt_ping {}}
catch {dpointSetScript test/dt_warm {}}
catch {dservRemoveExactMatch test/dt_ping}
catch {dservRemoveExactMatch test/dt_warm}" >/dev/null
}

run cold /tmp/disp_cold.txt
run warm /tmp/disp_warm.txt

for f in /tmp/disp_cold.txt /tmp/disp_warm.txt; do
  L=$(basename "$f" .txt)
  sort -n "$f" | awk -v l="$L" '{v[NR]=$1; s+=$1}
    END{ n=NR; if(n<2){ printf "  %s: no samples\n", l; exit }
      printf "  %-10s min %6d  med %6d  p90 %6d  p99 %6d  max %7d  mean %6.0f us  (n=%d)\n",
             l, v[1], v[int(n/2)+1], v[int(n*0.9)+1], v[int(n*0.99)+1], v[n], s/n, n; }'
done
