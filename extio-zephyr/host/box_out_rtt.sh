#!/bin/sh
# box_out_rtt.sh -- the dserv -> box OUTPUT path, measured entirely in dserv's clock.
#
# The mirror of host_gpio_rtt.sh. That one has the Pi drive an edge and the box
# observe it (input direction); this one has dserv issue a command and the PI
# observe the box's pin move (output direction):
#
#   t0 = dservTimestamp <dev>/cmd/do/<opin>   -- dserv issued the command
#   t1 = arrival of gpio/input/<hpin>         -- the Pi saw the box's pin move
#   RT = t1 - t0 = dserv -> transport -> box service loop -> GPIO write
#                  -> wire -> Pi kernel IRQ -> dserv publish
#
# NO HARDWARE SYNC LINE IS NEEDED, and that is the point of measuring the
# actuation on the PI rather than asking the box when it acted. Both timestamps
# are dserv's own, so the box's clock -- its offset, its ppm drift, whether it
# has ever obs-synced -- never enters the result. A sync line anchors the box
# CLOCK; that is a timestamp-accuracy question, not a latency one. Keeping the
# box clock out of the measurement is what makes this number trustworthy.
#
# THE RESULT INCLUDES THE PI'S OWN INPUT PATH, which is not free: gpio_input.c
# stamps with tclserver_now() after an epoll wakeup and a publish, not with the
# kernel's hardware edge timestamp. To isolate the box, measure the host
# baseline on the SAME edge and subtract:
#
#   sh host_gpio_rtt.sh <dev> <ipin> 27 150 23    -> "delivery (native)" column
#                                                    = Pi out -> Pi in, host only
#   sh box_out_rtt.sh   <dev> <opin> 22 150       -> this script
#   box output path ~= (this) - (host baseline)
#
# WIRING (3 wires; GPIO27 drives two INPUTS, GPIO22 is driven only by the box --
# never tee the Pi's output onto the pin the box drives, that is two drivers):
#   Pi phys 13 (GPIO27) --+-- box DI <ipin>                 [input direction]
#                         +-- Pi phys 16 (GPIO23)           [host baseline]
#   box DO <opin> ----------- Pi phys 15 (GPIO22)           [output direction]
#   Pi phys 14 (GND) -------- box GND
#
# All 3.3 V. Neither the RT1062 nor the RP2350 is 5 V tolerant.
#
# The box DO pin is configured here (config/pin/<n>/mode = 1). NOTE this exercises
# the INBOUND path, so on an Ethernet box it only works if %reg/%match
# self-registration is in place -- before that fix, cmd/* never reached the box
# at all and this script would time out on every sample.
#
#   sh box_out_rtt.sh <extio/name> [opin] [hpin] [iters]
#   sh box_out_rtt.sh extio/teensy41 3 22 150
DEV=${1:?usage: box_out_rtt.sh <extio/name> [opin] [hpin] [iters]}
OPIN=${2:-3}
HPIN=${3:-22}
N=${4:-150}
SETTLE=0.02
OUT=/tmp/box_out_rtt.txt

echo "box output RTT: $DEV/cmd/do/$OPIN -> gpio/input/$HPIN   n=$N"
echo "------------------------------------------------------------------"

# box DO pin as output (mode 1). Inbound path: needs %match on <pfx>/cmd/* .
dservctl -c "dservSet $DEV/config/pin/$OPIN/mode 1" >/dev/null 2>&1
sleep 1

# host input capture. gpio* commands live in the `ess` subprocess interp; the
# dpoint script and datapoint reads are the main interp's.
dservctl -c "proc __bo_hit {args} { dservSet test/bo_hit [now] }
dservAddExactMatch gpio/input/$HPIN
dpointSetScript gpio/input/$HPIN __bo_hit" >/dev/null \
  || { echo "  setup failed -- is dserv up?"; exit 1; }
dservctl ess "gpioLineRequestInput $HPIN BOTH 0 PULL_NONE ACTIVE_HIGH" >/dev/null \
  || { echo "  gpioLineRequestInput failed -- gpioInputInit not done in ess?"; exit 1; }

: > "$OUT"
i=0
while [ "$i" -lt "$N" ]; do
  dservctl -c "dservSet test/bo_hit 0" >/dev/null
  # alternate the level so every iteration is a real edge
  dservctl -c "dservSet $DEV/cmd/do/$OPIN $((i % 2))" >/dev/null
  sleep "$SETTLE"
  # catch-guarded: cmd/do and the arrival may not exist on the first pass
  R=$(dservctl -c "set a 0; catch {set a [dservTimestamp $DEV/cmd/do/$OPIN]}
set b 0; catch {set b [dservGet test/bo_hit]}
list \$a \$b" 2>/dev/null)
  set -- $R
  T0=${1:-0}; A=${2:-0}
  RT=$((A - T0))
  if [ "$A" -ne 0 ] && [ "$T0" -ne 0 ] && [ "$RT" -gt 0 ] && [ "$RT" -lt 500000 ]; then
    echo "$RT" >> "$OUT"
  fi
  i=$((i + 1))
done

dservctl -c "catch {dpointSetScript gpio/input/$HPIN {}}
catch {dservRemoveExactMatch gpio/input/$HPIN}
catch {dservClear test/bo_hit}" >/dev/null

sort -n "$OUT" | awk '{v[NR]=$1; s+=$1}
  END{ n=NR; if(n<2){ print "  NO SAMPLES -- check: box DO'"$OPIN"' -> Pi GPIO'"$HPIN"', box pin mode=out,"
                      print "                and that cmd/* actually REACHES the box (Ethernet needs %match)"; exit }
    printf "  box output       min %6d  med %6d  p90 %6d  p99 %6d  max %7d  mean %6.0f us  (n=%d)\n",
           v[1], v[int(n/2)+1], v[int(n*0.9)+1], v[int(n*0.99)+1], v[n], s/n, n; }'
echo "------------------------------------------------------------------"
echo "Includes the Pi's own input path -- subtract the host baseline from"
echo "host_gpio_rtt.sh's \"delivery (native)\" column to isolate the box."
