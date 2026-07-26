#!/bin/sh
# Rate-lock the NIC's PHC to the host system clock.
#
# The Pi is the PTP grandmaster, so its PHC is free-running -- measured at
# -46.4 ppm against CLOCK_MONOTONIC, i.e. ~46 us of divergence per second.
# dserv stamps from CLOCK_MONOTONIC + a constant, so without this the
# PHC <-> dserv mapping would need re-measuring every ~21 ms to hold 1 us.
#
# -c eth0 -s CLOCK_REALTIME: discipline the PHC (consumer) FROM the system clock
# (source). Linux applies the same frequency adjustment to CLOCK_MONOTONIC as to
# CLOCK_REALTIME, so locking the PHC to the system clock also rate-locks it to
# dserv's timebase -- which is the whole point. The box then follows the PHC.
LOG=/tmp/phc2sys.log
for p in $(pgrep -x phc2sys); do kill "$p" 2>/dev/null; done
sleep 1
: > "$LOG"
setsid /usr/sbin/phc2sys -c eth0 -s CLOCK_REALTIME -w -m >> "$LOG" 2>&1 &
echo "started pid $!"
