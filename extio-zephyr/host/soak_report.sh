#!/bin/sh
# soak_report.sh -- summarise a soak.sh log.
#
# Reports the two distributions separately, plus the things a median hides: the
# worst samples, how many breached a budget, and whether the clock TRENDED (which
# a percentile summary cannot show, since holdover drift is monotonic rather than
# scattered).
#
#   sh soak_report.sh [/tmp/soak_upstairs.log] [budget_us]
LOG=${1:-/tmp/soak_upstairs.log}
BUDGET=${2:-1000}

[ -r "$LOG" ] || { echo "no such log: $LOG"; exit 1; }

echo "=================================================================="
echo "soak report: $LOG"
grep '^#' "$LOG" | head -4
MISS=$(grep -c '^# MISS' "$LOG" 2>/dev/null || echo 0)
echo "missed samples: $MISS"
echo "=================================================================="

for col in 5:clock_err 6:delivery; do
  N=${col%%:*}; NAME=${col##*:}
  grep -v '^#' "$LOG" | awk -v c="$N" '{print $c}' | sort -n | awk -v name="$NAME" -v b="$BUDGET" '
    {v[NR]=$1; s+=$1}
    END{
      n=NR; if(n<2){ printf "  %-10s no samples\n", name; exit }
      over=0; for(i=1;i<=n;i++) if(v[i]>b || v[i]<-b) over++
      printf "  %-10s min %7d  med %7d  p90 %7d  p99 %7d  p999 %7d  max %8d  mean %7.0f  (n=%d)\n",
             name, v[1], v[int(n/2)+1], v[int(n*0.9)+1], v[int(n*0.99)+1], v[int(n*0.999)+1], v[n], s/n, n
      printf "  %-10s outside +/-%d us: %d  (%.3f%%)\n", "", b, over, 100*over/n
    }'
done

echo "------------------------------------------------------------------"
echo "worst 5 delivery samples (time, delivery_us):"
grep -v '^#' "$LOG" | sort -k6 -n | tail -5 | awk '{printf "  %s  %8d us\n", strftime("%H:%M:%S", $1), $6}'

echo "------------------------------------------------------------------"
echo "clock trend -- holdover shows up here, not in the percentiles:"
grep -v '^#' "$LOG" | awk 'NR==1{t0=$1; e0=$5}
  {t=$1; e=$5; n++; last_t=t; last_e=e}
  END{ if(n<10){ print "  too few samples"; exit }
    dt=last_t-t0; de=last_e-e0
    printf "  first %d us -> last %d us over %d s  (delta %+d us", e0, last_e, dt, de
    if (dt>0) printf ", %+.2f ppm equivalent", de/dt
    printf ")\n"
    if (dt>0 && (de>500 || de<-500))
      printf "  ^ TRENDING: rate correction is not holding across this interval\n"
  }'

echo "------------------------------------------------------------------"
echo "box reboots during soak (uptime_us going backwards):"
grep -v '^#' "$LOG" | awk '$7>0 { if (prev>0 && $7 < prev) printf "  %s  uptime %d -> %d\n", strftime("%H:%M:%S",$1), prev, $7; prev=$7 }' | head -5
echo "  (none listed = box ran continuously)"
