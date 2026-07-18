#!/bin/sh
# echo_soak.sh -- self-driven ground-truth stimulus for echo-sync (BLE.md "Time").
# The RECEIVER drives one output pin that is wired to a handheld DI pin, so both
# boards observe the SAME edge: the receiver's own state/do (near-truth, local)
# is the reference, the handheld's state/di (stamped -> radioed -> rewritten) is
# the thing under test. Pair + measure the delta with echo_analyze.py.
#
# WIRING (one signal wire + common ground; both boxes share GND through the USB
# hub already): receiver GP<out>  ->  handheld GP26 (A0, the validated DI).
#
#   # capture both datapoints while this runs:
#   dservctl --json listen --for 3m \
#       "extio/<rx>/state/do/<out>" "extio/<hh>/state/di/26" > echo_run.jsonl &
#   sh echo_soak.sh -r <rx> -H <hh> -p <out> -n 200
#   python3 echo_analyze.py --ref  extio/<rx>/state/do/<out> --ref-val 1 \
#                           --test extio/<hh>/state/di/26   --test-val 1 echo_run.jsonl
#
# We drive explicit level SETs (do <pin> 1 / 0), NOT pulse_us: only a GPIO_OP_SET
# publishes state/do (box_clock-stamped at actuation = the reference edge); the
# scheduled-pulse path is silent. Measure the RISING edge (both boards report
# value 1 at the onset), so --ref-val 1 --test-val 1.
#
# Options:
#   -r rx     receiver box name   (default pico)
#   -H hh     handheld box name   (default hh1)
#   -p pin    receiver output pin (default 14 -- free on pico2_w per PINMAP)
#   -d dipin  handheld DI pin     (default 26)
#   -n count  number of edges     (default 200)
#   -w us     high dwell us       (default 20000 -- > DI debounce and > one BLE
#                                   interval, so the onset edge is unambiguous)
#   -i ms     inter-edge gap ms   (default 300 -- BLE conn interval is ~15ms, so
#                                   this is many intervals: independent samples)
#   -s host   dserv host for dservctl (default localhost)
#
# Notes: leaves the pins configured (pin <out> mode out / pin 26 mode in) so a
# follow-up run needs no re-setup. `pulse_us` gives a rising THEN falling edge
# ~w apart; analyze one polarity with --test-val. Cadence jitter is irrelevant
# (we pair edges, not spacing). Do NOT run mid-session -- it drives a real pin.
set -eu

RX=pico; HH=hh1; PIN=14; DIPIN=26; N=200; W=20000; GAP=300; SHOST=localhost
while getopts r:H:p:d:n:w:i:s: f; do
  case $f in
    r) RX=$OPTARG;; H) HH=$OPTARG;; p) PIN=$OPTARG;; d) DIPIN=$OPTARG;;
    n) N=$OPTARG;; w) W=$OPTARG;; i) GAP=$OPTARG;; s) SHOST=$OPTARG;;
    *) echo "see header for usage" >&2; exit 1;;
  esac
done

dc() { dservctl --host "$SHOST" "$@"; }

echo ">> configuring: $RX GP$PIN = output, $HH GP$DIPIN = input"
dc set extio/$RX/config/pin/$PIN/mode out    >/dev/null    # receiver drives the edge
dc set extio/$HH/config/pin/$DIPIN/mode in   >/dev/null    # handheld reads it (driven, no pull)
sleep 0.5

echo ">> driving $N rising edges: GP$PIN high ${W}us, low gap ${GAP}ms  (~$(( N * (GAP + W/1000) / 1000 ))s)"
msleep() { perl -e "select undef,undef,undef,$1" 2>/dev/null || sleep 1; }   # fractional-second sleep
i=0
while [ "$i" -lt "$N" ]; do
  dc set extio/$RX/cmd/do/$PIN 1 >/dev/null                # RISING edge: state/do/$PIN=1 (ref) + wire edge -> di
  msleep "$(( W ))e-6"                                     # dwell high
  dc set extio/$RX/cmd/do/$PIN 0 >/dev/null                # falling edge (resets for next sample)
  msleep "$(( GAP ))e-3"                                   # inter-sample gap (many BLE intervals)
  i=$((i + 1))
  [ $((i % 25)) -eq 0 ] && echo "   $i/$N"
done
dc set extio/$RX/cmd/do/$PIN 0 >/dev/null                  # leave the line low
echo ">> done -- $N edges driven. Analyze the capture with echo_analyze.py."
