#!/bin/sh
# soak.sh -- long-run stability of an extio box: delivery tail AND clock holdover,
# both from the SAME physical edge.
#
# Short runs (~150 samples over minutes) said the wiznet box is excellent. The
# one thing in the record that would breach a 1 ms budget is a single 25,966 us
# outlier seen once on a sibling box and never explained. That is a tail
# question, and tails need hours, not minutes.
#
# Per sample, one edge on the Pi's output, teed to a Pi input and the box's DI:
#   t_host = dservTimestamp gpio/input/<hpin>    kernel irq stamp (the edge)
#   t_box  = dservTimestamp <dev>/state/di/<ipin>  box irq stamp, box_clock-mapped
#   t_arr  = [now] in the arrival callback         when dserv LEARNED
#
#   clock_err = t_box - t_host    drift/holdover of the box clock
#   delivery  = t_arr  - t_host   the latency a rig actually feels
#
# ANCHORS ONCE at the start and then leaves the clock alone, deliberately: that
# measures HOLDOVER, i.e. how long a box can go between obs boundaries before its
# rate correction stops carrying it. Re-anchoring every loop would measure
# something easier and less useful.
#
# Logs raw values so the analysis is not baked in; summarise with soak_report.sh.
# Detached run:  nohup sh soak.sh extio/upstairs > /tmp/soak.out 2>&1 &
DEV=${1:?usage: soak.sh <extio/name> [ipin] [opin] [hpin] [period_s]}
IPIN=${2:-10}
OPIN=${3:-27}
HPIN=${4:-23}
PERIOD=${5:-2}
LOG=${LOG:-/tmp/soak_$(basename "$DEV").log}

echo "# soak $DEV di/$IPIN via GPIO$OPIN, host ref gpio/input/$HPIN, period ${PERIOD}s" > "$LOG"
echo "# epoch_s  t_host_us  t_box_us  t_arr_us  clock_err_us  delivery_us  box_uptime_us" >> "$LOG"

dservctl ess "gpioLineRequestOutput $OPIN 0" >/dev/null 2>&1
dservctl ess "gpioLineRequestInput $HPIN BOTH 0 PULL_NONE ACTIVE_HIGH" >/dev/null 2>&1
dservctl -c "proc soak_hit {args} { dservSet test/soak_arr [now] }
dservAddExactMatch $DEV/state/di/$IPIN
dpointSetScript $DEV/state/di/$IPIN soak_hit" >/dev/null 2>&1

# one hardware anchor, then hands off -- see the holdover note above
dservctl ess "gpioLineSetValue 26 0; dservSet ess/in_obs 0" >/dev/null 2>&1; sleep 1
dservctl ess "gpioLineSetValue 26 1; dservSet ess/in_obs 1" >/dev/null 2>&1; sleep 1
dservctl ess "gpioLineSetValue 26 0; dservSet ess/in_obs 0" >/dev/null 2>&1; sleep 1
SRC=$(dservctl -c "set r ?; catch {set r [dservGet $DEV/state/sync/source]}; set r" 2>/dev/null | tr -d '[:space:]')
echo "# anchored: source=$SRC at $(date -Is)" >> "$LOG"

i=0
bad=0
while true; do
  dservctl -c "dservSet test/soak_arr 0" >/dev/null 2>&1
  dservctl ess "gpioLineSetValue $OPIN $((i % 2))" >/dev/null 2>&1
  sleep 0.05
  R=$(dservctl -c "set a 0; catch {set a [dservTimestamp gpio/input/$HPIN]}
set b 0; catch {set b [dservTimestamp $DEV/state/di/$IPIN]}
set c 0; catch {set c [dservGet test/soak_arr]}
set d 0; catch {set d [dservGet $DEV/state/uptime_us]}
list \$a \$b \$c \$d" 2>/dev/null)
  set -- $R
  TH=${1:-0}; TB=${2:-0}; TA=${3:-0}; UP=${4:-0}
  if [ "$TH" -gt 0 ] && [ "$TB" -gt 0 ] && [ "$TA" -gt 0 ]; then
    printf '%s %s %s %s %s %s %s\n' "$(date +%s)" "$TH" "$TB" "$TA" \
           $((TB - TH)) $((TA - TH)) "$UP" >> "$LOG"
  else
    bad=$((bad + 1))
    # a missing sample is itself signal (box gone, dserv restarted, edge lost)
    echo "# MISS at $(date -Is) th=$TH tb=$TB ta=$TA (total $bad)" >> "$LOG"
  fi
  i=$((i + 1))
  sleep "$PERIOD"
done
