#!/bin/sh
# lb_queue.sh -- is the DI publish delayed by the DO echo publish ahead of it?
#
# Both frames are BOX-STAMPED at their physical instant (publish_do uses
# event_stamp(t_act) at the pin write; publish_di uses event_stamp(e.t_us) at the
# IRQ edge), but both are sent by blocking box_net_client_send() calls on the one
# service loop -- so the DI frame's TRANSMISSION queues behind the echo's.
#
#   BOX   = dservTimestamp(di) - dservTimestamp(do)   both box clock  -> wire+IRQ
#   ARR   = arrival(di)        - arrival(do)          both host clock -> delivery gap
#   QUEUE = ARR - BOX                                 the serialization cost
#
# Each term is a difference WITHIN one clock, so the box-clock offset (and its
# ~98 us sync jitter) cancels; only rate error over ~100 us survives, which is
# nothing. That makes QUEUE the trustworthy number here even though the two
# clocks differ.
DEV=${1:?usage: lb_queue.sh <extio/name> [opin] [ipin] [iters]}
OPIN=${2:-10}
IPIN=${3:-11}
N=${4:-60}
SETTLE=0.02
: > /tmp/q_box.txt
: > /tmp/q_arr.txt
: > /tmp/q_que.txt

dservctl -c "proc q_do {args} { dservSet test/q_do [now] }
proc q_di {args} { dservSet test/q_di [now] }
dservAddExactMatch $DEV/state/do/$OPIN
dservAddExactMatch $DEV/state/di/$IPIN
dpointSetScript $DEV/state/do/$OPIN q_do
dpointSetScript $DEV/state/di/$IPIN q_di" >/dev/null

i=0
while [ "$i" -lt "$N" ]; do
  dservctl -c "dservSet test/q_do 0; dservSet test/q_di 0" >/dev/null
  dservctl -c "dservSet $DEV/cmd/do/$OPIN $((i % 2))" >/dev/null
  sleep "$SETTLE"
  R=$(dservctl -c "list [dservTimestamp $DEV/state/do/$OPIN] [dservTimestamp $DEV/state/di/$IPIN] [dservGet test/q_do] [dservGet test/q_di]" 2>/dev/null)
  set -- $R
  DOTS=${1:-0}; DITS=${2:-0}; DOAR=${3:-0}; DIAR=${4:-0}
  if [ "$DOTS" -gt 0 ] && [ "$DITS" -gt 0 ] && [ "$DOAR" -gt 0 ] && [ "$DIAR" -gt 0 ]; then
    BOX=$((DITS - DOTS))
    ARR=$((DIAR - DOAR))
    QUE=$((ARR - BOX))
    [ "$BOX" -gt -100000 ] && [ "$BOX" -lt 100000 ] && echo "$BOX" >> /tmp/q_box.txt
    [ "$ARR" -gt -100000 ] && [ "$ARR" -lt 100000 ] && echo "$ARR" >> /tmp/q_arr.txt
    [ "$QUE" -gt -100000 ] && [ "$QUE" -lt 100000 ] && echo "$QUE" >> /tmp/q_que.txt
  fi
  i=$((i + 1))
done

dservctl -c "catch {dpointRemoveScript $DEV/state/do/$OPIN}
catch {dpointRemoveScript $DEV/state/di/$IPIN}
catch {dservRemoveMatch $DEV/state/do/$OPIN}
catch {dservRemoveMatch $DEV/state/di/$IPIN}
catch {dservClear test/q_do}; catch {dservClear test/q_di}" >/dev/null

for f in /tmp/q_box.txt /tmp/q_arr.txt /tmp/q_que.txt; do
  L=$(basename "$f" .txt)
  sort -n "$f" | awk -v l="$L" '{v[NR]=$1; s+=$1}
    END{ n=NR; if(n<2){ printf "  %s: no samples\n", l; exit }
      printf "  %-6s min %7d  med %7d  p90 %7d  p99 %7d  max %7d  mean %7.0f us  (n=%d)\n",
             l, v[1], v[int(n/2)+1], v[int(n*0.9)+1], v[int(n*0.99)+1], v[n], s/n, n; }'
done
