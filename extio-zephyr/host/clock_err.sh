#!/bin/sh
# clock_err.sh -- box clock error against the host, both ends HARDWARE-stamped.
#
# One physical edge, two independent hardware timestamps of it:
#
#   t_host = dservTimestamp gpio/input/<hpin>   Pi kernel, gpio_v2_line_event
#                                               .timestamp_ns taken in the GPIO
#                                               irq handler
#   t_box  = dservTimestamp <dev>/state/di/<ipin>
#                                               box IRQ, event_stamp(e.t_us)
#                                               mapped through box_clock
#
#   err = t_box - t_host        <- pure clock error. No software stamp, no
#                                  delivery time, no Tcl callback in the path.
#
# This is the measurement every earlier harness approximated. host_gpio_rtt.sh
# used gpio/output/<opin> as t0, stamped just AFTER the ioctl returned, which
# carried the host's own ~72 us input path as a silent bias -- fine for
# comparing configurations, useless for asking "is the box clock right".
#
# REQUIRES dserv with kernel GPIO timestamps (tclserver_clock_epoch_offset_us +
# gpio_input.c stamping from event.timestamp_ns). Against an older dserv,
# t_host is software-stamped and this silently measures the old thing again --
# there is no way to detect that from here, so check the dserv version.
#
# WIRING (the tee is the whole point: one edge, two observers):
#   Pi <opin> out --+-- Pi <hpin> in        [host hardware reference]
#                   +-- box DI <ipin>       [box under test]
#   box DI <ipin> needs debounce 0, else the box stamps the debounced edge.
#
#   sh clock_err.sh <extio/name> [ipin] [opin] [hpin] [iters]
#   sh clock_err.sh extio/upstairs 10 27 23 100
DEV=${1:?usage: clock_err.sh <extio/name> [ipin] [opin] [hpin] [iters]}
IPIN=${2:-10}
OPIN=${3:-27}
HPIN=${4:-23}
N=${5:-100}
SETTLE=0.02
OUT=/tmp/clock_err.txt

SRC=$(dservctl -c "set r ?; catch {set r [dservGet $DEV/state/sync/source]}; set r" 2>/dev/null | tr -d '[:space:]')
echo "clock error: $DEV/state/di/$IPIN vs gpio/input/$HPIN   n=$N   sync/source=$SRC"
echo "------------------------------------------------------------------"

dservctl ess "gpioLineRequestOutput $OPIN 0" >/dev/null 2>&1
dservctl ess "gpioLineRequestInput $HPIN BOTH 0 PULL_NONE ACTIVE_HIGH" >/dev/null 2>&1

: > "$OUT"
i=0
while [ "$i" -lt "$N" ]; do
  dservctl ess "gpioLineSetValue $OPIN $((i % 2))" >/dev/null 2>&1
  sleep "$SETTLE"
  # Both are datapoint TIMESTAMPS, not arrival times -- that is what keeps
  # delivery and dispatch out of the number.
  R=$(dservctl -c "set a 0; catch {set a [dservTimestamp gpio/input/$HPIN]}
set b 0; catch {set b [dservTimestamp $DEV/state/di/$IPIN]}
list \$a \$b" 2>/dev/null)
  set -- $R
  TH=${1:-0}; TB=${2:-0}
  if [ "$TH" -gt 0 ] && [ "$TB" -gt 0 ]; then
    E=$((TB - TH))
    # can legitimately be negative (box clock behind); only reject the absurd
    [ "$E" -gt -1000000 ] && [ "$E" -lt 1000000 ] && echo "$E" >> "$OUT"
  fi
  i=$((i + 1))
done

sort -n "$OUT" | awk -v s="$SRC" '{v[NR]=$1; t+=$1}
  END{ n=NR; if(n<2){ print "  NO SAMPLES -- check the tee, pin modes, debounce 0"; exit }
    printf "  clock error  min %7d  med %7d  p90 %7d  p99 %7d  max %7d  mean %7.0f us  (n=%d)\n",
           v[1], v[int(n/2)+1], v[int(n*0.9)+1], v[int(n*0.99)+1], v[n], t/n, n;
    printf "  spread (p99-min) %d us   <- sync QUALITY; the median is standing bias [%s]\n",
           v[int(n*0.99)+1]-v[1], s; }'
echo "------------------------------------------------------------------"
echo "Run once per sync mode (hw / swc / sw) to compare them on equal terms."
