#!/bin/sh
# box_transport_test.sh -- characterize the wired box transport from THIS host.
#
# Run ON a dserv host that the box is registered with (localhost:4620). Measures
# three things and prints a summary you can compare against the 1 ms timing budget:
#
#   A) RAW LINK        ping RTT to the box (link + box IP-stack, no dserv)
#   B) SYNC ROUND-TRIP dserv sets ess/in_obs -> box snaps + echoes state/sync/* back;
#                      a trigger script records dserv now() on arrival. RT = A - D.
#                      one-way in_obs push  Δnet <~ min(RT)/2  (bound on the constant
#                      early-bias every box timestamp carries).
#   C) ANCHOR JITTER   offset across many obs edges, linearly detrended: crystal drift
#                      (ppm) + per-snap jitter sd (the one-way in_obs delivery jitter,
#                      which governs per-obs alignment quality).
#
# SAFE: refuses to drive ess/in_obs if an experiment is running; restores it to 0
# and removes its trigger script on exit.
#
#   sh box_transport_test.sh [name] [box_ip]      # defaults: macbook 192.168.11.2
#   ssh pi4dev.local 'sh -s' < box_transport_test.sh            # run on a remote rig
#
# Reference (pi4dev, native GbE, dserv-local, 2026): ping 0.11 ms, RT med 0.70 ms /
# max 1.02 ms, anchor jitter sd 27 us -- ~40x inside a 1 ms budget.
NAME=${1:-macbook}
BOXIP=${2:-192.168.11.2}
DEV=extio/$NAME
N_PING=30
N_RT=40
N_OBS=24
SETTLE=0.10

echo "host: $(hostname)   box: $DEV @ $BOXIP"
echo "=================================================================="
echo "A) RAW LINK  --  ping RTT to the box"
echo "------------------------------------------------------------------"
ping -c "$N_PING" -i 0.2 "$BOXIP" 2>&1 | tail -3

# guard: don't disturb a live experiment
ST=$(dservctl -c 'dservGet ess/status' 2>/dev/null | tr -d '[:space:]')
IO=$(dservctl -c 'dservGet ess/in_obs'  2>/dev/null | tr -d '[:space:]')
if [ "$ST" = "running" ] || [ "$IO" = "1" ]; then
  echo; echo ">> ess appears active (status=$ST in_obs=$IO) -- skipping in_obs tests to avoid disruption."
  exit 0
fi

echo
echo "=================================================================="
echo "B) SYNC ROUND-TRIP  --  dserv sets in_obs -> box echoes sync back (us)"
echo "------------------------------------------------------------------"
dservctl -c "proc rt_capture {args} { dservSet test/rt_arrival [now] }
dservSet test/rt_arrival 0
dservAddExactMatch $DEV/state/sync/dserv_us
dpointSetScript $DEV/state/sync/dserv_us rt_capture" >/dev/null
: > /tmp/bt_rt.txt
i=0
while [ "$i" -lt "$N_RT" ]; do
  D=$(dservctl -c "dservSet ess/in_obs $((i%2)); dservTimestamp ess/in_obs" | tr -d '[:space:]')
  sleep "$SETTLE"
  A=$(dservctl -c 'dservGet test/rt_arrival' | tr -d '[:space:]')
  RT=$((A - D))
  [ "$RT" -gt 0 ] && [ "$RT" -lt 500000 ] && echo "$RT" >> /tmp/bt_rt.txt
  i=$((i + 1))
done
dservctl -c "catch {dpointSetScript $DEV/state/sync/dserv_us {}}
catch {dservRemoveExactMatch $DEV/state/sync/dserv_us}
catch {dservClear test/rt_arrival}
dservSet ess/in_obs 0" >/dev/null
sort -n /tmp/bt_rt.txt | awk '{v[NR]=$1; s+=$1}
END{ n=NR; if(n<2){print "  (insufficient samples)"; exit}
  printf "  min %d  med %d  p90 %d  p99 %d  max %d  mean %.0f  (n=%d)\n",
         v[1], v[int(n/2)+1], v[int(n*0.9)+1], v[int(n*0.99)], v[n], s/n, n;
  printf "  one-way in_obs push  Delta_net <~ %d us  (min RT/2 -- upper bound on the constant stamp bias)\n", v[1]/2; }'

echo
echo "=================================================================="
echo "C) SYNC-ANCHOR JITTER  --  offset across $N_OBS obs cycles (detrended)"
echo "------------------------------------------------------------------"
: > /tmp/bt_off.txt
i=0
while [ "$i" -lt "$N_OBS" ]; do
  dservctl set ess/in_obs 1 >/dev/null; sleep "$SETTLE"
  dservctl -c "list [dservTimestamp ess/in_obs] [dservGet $DEV/state/sync/offset_us]" >> /tmp/bt_off.txt
  dservctl set ess/in_obs 0 >/dev/null; sleep "$SETTLE"
  dservctl -c "list [dservTimestamp ess/in_obs] [dservGet $DEV/state/sync/offset_us]" >> /tmp/bt_off.txt
  i=$((i + 1))
done
dservctl set ess/in_obs 0 >/dev/null
awk '{x[NR]=$1; y[NR]=$2}
END{ c=NR; if(c<3){print "  (insufficient samples)"; exit}
  x0=x[1]; y0=y[1];
  for(i=1;i<=c;i++){ xi=(x[i]-x0)/1e6; yi=y[i]-y0; X[i]=xi; Y[i]=yi; sx+=xi; sy+=yi; sxx+=xi*xi; sxy+=xi*yi }
  m=(c*sxy - sx*sy)/(c*sxx - sx*sx); b=(sy - m*sx)/c;
  for(i=1;i<=c;i++){ r=Y[i]-(m*X[i]+b); ss+=r*r; if(i==1||r<rmn)rmn=r; if(i==1||r>rmx)rmx=r }
  rsd=sqrt(ss/(c-1)); span=X[c];
  printf "  edges %d  span %.1fs  crystal-drift %.1f ppm  per-snap jitter sd %.1f us  p2p %.1f us\n",
         c, span, m, rsd, rmx-rmn; }' /tmp/bt_off.txt
echo "=================================================================="
echo "done. (ess/in_obs restored to 0; trigger removed)"
