#!/bin/sh
# sw_bias.sh -- how wrong is a sw anchor, measured against a hw anchor on the
# same box minutes apart? Alternate hw / sw / hw and interpolate.
#
#   hw: offset = dserv_us - EDGE_box          (latched TTL edge)
#   sw: offset = dserv_us - RECEIPT_box       (frame arrival, delivery later)
#   receipt = edge + d  =>  offset_sw = offset_hw - d
#
# so the sw anchor should sit BELOW the hw trend by d = one-way delivery
# (~206 us on Ethernet, per state/sync/transport_us). The box clock drifts
# ~25 ppm between anchors, so bracket the sw sample with two hw samples and
# compare against their midpoint rather than against either one.
DEV=extio/upstairs
off () { dservctl -c "dservGet $DEV/state/sync/offset_us" 2>/dev/null | tr -d '[:space:]'; }
src () { dservctl -c "dservGet $DEV/state/sync/source" 2>/dev/null | tr -d '[:space:]'; }

anchor_hw () {   # $1 = obs level; drive the matching edge WITH the datapoint
  dservctl ess "gpioLineSetValue 26 $1; dservSet ess/in_obs $1" >/dev/null 2>&1
  sleep 1
}
anchor_sw () {   # sync pin disabled; the pin state is irrelevant
  dservctl -c "dservSet ess/in_obs $1" >/dev/null 2>&1
  sleep 1
}

# --- bracket 1: hw ---
dservctl -c "dservSet $DEV/config/sync/pin 26" >/dev/null 2>&1; sleep 1
anchor_hw 0; anchor_hw 1
H1=$(off); S1=$(src)

# --- middle: sw (sync pin off => 99 is out of range => sync_input_off) ---
dservctl -c "dservSet $DEV/config/sync/pin 99" >/dev/null 2>&1; sleep 1
anchor_sw 0; anchor_sw 1
W=$(off); S2=$(src)

# --- bracket 2: hw ---
dservctl -c "dservSet $DEV/config/sync/pin 26" >/dev/null 2>&1; sleep 1
anchor_hw 0; anchor_hw 1
H2=$(off); S3=$(src)

echo "hw1 = $H1  ($S1)"
echo "sw  = $W  ($S2)"
echo "hw2 = $H2  ($S3)"
awk -v h1="$H1" -v w="$W" -v h2="$H2" 'BEGIN{
  mid = (h1 + h2) / 2;
  printf "hw midpoint (drift-interpolated) = %.0f\n", mid;
  printf "sw - hw = %.0f us   <- the sw anchor bias\n", w - mid;
  printf "hw drift across the bracket      = %.0f us\n", h2 - h1;
}'

# leave the box as we found it
dservctl -c "dservSet $DEV/config/sync/pin 26" >/dev/null 2>&1; sleep 1
dservctl ess "gpioLineSetValue 26 0; dservSet ess/in_obs 0" >/dev/null 2>&1
