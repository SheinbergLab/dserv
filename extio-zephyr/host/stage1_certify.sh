#!/bin/sh
# stage1_certify.sh -- host-only sw-anchor bias correction, certified against the
# hardware sync line on the same box.
#
# THE IDEA. A sw anchor computes offset = dserv_us - receipt_box, but receipt_box
# really corresponds to dserv time (T_send + d). So publishing ess/in_obs with a
# timestamp FORWARD-DATED by d makes the naive anchor correct -- with no firmware
# change. dserv can do this: `dservSetData <var> <ts> <type> <bytes>` honours an
# explicit timestamp (Dataserver.cpp: `if (!ts) ts = ds->now()`).
#
# ESTIMATING d WITHOUT A WIRE. Round-trip the existing cmd/do -> state/do echo and
# halve the MINIMUM (least-queued sample, where the symmetry assumption is least
# wrong). This over-estimates d by roughly half the box's internal turnaround,
# since the echo includes dispatch + GPIO write + publish while d does not. The
# residual below measures exactly that error.
#
# CERTIFICATION. Anchors drift ~25 ppm, so bracket every sw sample between two hw
# samples and compare against their midpoint:
#     hw -> sw(uncorrected) -> hw -> sw(corrected) -> hw
DEV=extio/upstairs
OPIN=11
N=${1:-40}

off () { dservctl -c "dservGet $DEV/state/sync/offset_us" 2>/dev/null | tr -d '[:space:]'; }
hw_anchor () {
  dservctl -c "dservSet $DEV/config/sync/pin 26" >/dev/null 2>&1; sleep 1
  dservctl ess "gpioLineSetValue 26 0; dservSet ess/in_obs 0" >/dev/null 2>&1; sleep 1
  dservctl ess "gpioLineSetValue 26 1; dservSet ess/in_obs 1" >/dev/null 2>&1; sleep 1
}
sync_off () { dservctl -c "dservSet $DEV/config/sync/pin 99" >/dev/null 2>&1; sleep 1; }

# ---- phase A: estimate d = min(RTT)/2 over the cmd/do -> state/do echo ----
dservctl -c "proc s1_do {args} { dservSet test/s1_do [now] }
dservAddExactMatch $DEV/state/do/$OPIN
dpointSetScript $DEV/state/do/$OPIN s1_do" >/dev/null
: > /tmp/s1_rtt.txt
i=0
while [ "$i" -lt "$N" ]; do
  dservctl -c "dservSet test/s1_do 0" >/dev/null
  C=$(dservctl -c "dservSet $DEV/cmd/do/$OPIN $((i % 2)); dservTimestamp $DEV/cmd/do/$OPIN" | tr -d '[:space:]')
  sleep 0.02
  A=$(dservctl -c "set a 0; catch {set a [dservGet test/s1_do]}; set a" 2>/dev/null | tr -d '[:space:]')
  [ "$A" -gt 0 ] && [ "$C" -gt 0 ] && echo $((A - C)) >> /tmp/s1_rtt.txt
  i=$((i + 1))
done
dservctl -c "catch {dpointSetScript $DEV/state/do/$OPIN {}}
catch {dservRemoveExactMatch $DEV/state/do/$OPIN}
catch {dservClear test/s1_do}" >/dev/null

RTTMIN=$(sort -n /tmp/s1_rtt.txt | head -1)
RTTMED=$(sort -n /tmp/s1_rtt.txt | awk '{v[NR]=$1} END{print v[int(NR/2)+1]}')
D=$((RTTMIN / 2))
echo "echo RTT: min $RTTMIN  med $RTTMED us   ->  d_est = min/2 = $D us"
echo "---------------------------------------------------------------"

# ---- phase B: bracketed certification ----
hw_anchor;                                        H1=$(off)
sync_off
dservctl -c "dservSet ess/in_obs 0" >/dev/null 2>&1; sleep 1
dservctl -c "dservSet ess/in_obs 1" >/dev/null 2>&1; sleep 1
W0=$(off)                                          # uncorrected sw
hw_anchor;                                        H2=$(off)
sync_off
dservctl -c "dservSetData ess/in_obs [expr {[now] + $D}] 5 [binary format i 0]" >/dev/null 2>&1; sleep 1
dservctl -c "dservSetData ess/in_obs [expr {[now] + $D}] 5 [binary format i 1]" >/dev/null 2>&1; sleep 1
W1=$(off)                                          # corrected sw
hw_anchor;                                        H3=$(off)

awk -v h1="$H1" -v w0="$W0" -v h2="$H2" -v w1="$W1" -v h3="$H3" -v d="$D" 'BEGIN{
  m0 = (h1 + h2) / 2; m1 = (h2 + h3) / 2;
  printf "  sw UNCORRECTED  vs hw: %+7.0f us\n", w0 - m0;
  printf "  sw CORRECTED    vs hw: %+7.0f us   (forward-dated by d_est = %d)\n", w1 - m1, d;
  printf "  improvement          : %7.0f -> %.0f us of bias\n", (w0-m0<0?-(w0-m0):w0-m0), (w1-m1<0?-(w1-m1):w1-m1);
  printf "  hw drift over run    : %+7.0f us\n", h3 - h1;
}'

# restore
dservctl -c "dservSet $DEV/config/sync/pin 26" >/dev/null 2>&1; sleep 1
dservctl ess "gpioLineSetValue 26 0; dservSet ess/in_obs 0" >/dev/null 2>&1
