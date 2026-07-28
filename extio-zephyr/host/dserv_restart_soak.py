#!/usr/bin/env python3
"""dserv_restart_soak.py -- when dserv restarts, do the boxes recover on their own?

This is the path that actually cost an evening on 2026-07-26, and it is NOT the
same as the box-reboot path: here the BOX never restarts. It keeps running with
whatever it already believed about its registration, while dserv forgets
everything. box_net_eth_server_up() -- the accepted connect-back socket -- is the
box's only signal that anything changed, and if that socket does not error the
box has no reason to re-register.

Three outcomes, and the middle one is the whole point of the test:

  PASS   uplink came back AND cmd/* lands again        (fully recovered)
  DEAF   state/* flows again but cmds_rx never moves   (publishing but deaf)
  GONE   never published again at all

Note the box does NOT reboot, so cmds_rx does not reset -- recovery is "it went
UP after the restart", not "it exists".

  dserv_restart_soak.py <cycles> <box> [box ...]
"""
import subprocess, sys, time

CYCLES = int(sys.argv[1])
BOXES = sys.argv[2:]
PI = "raspberrypi.local"
RECOVER_TIMEOUT = 90.0


def pi(cmd, timeout=60):
    try:
        r = subprocess.run(["ssh", "-o", "BatchMode=yes", PI, cmd],
                           capture_output=True, text=True, timeout=timeout)
        return r.stdout.strip()
    except Exception:
        return ""


def dp(box, key):
    out = pi("dservctl -c 'dservGet extio/%s/state/%s' 2>/dev/null | head -1" % (box, key))
    try:
        return int(out.split()[0])
    except Exception:
        return None


tally = {b: {"PASS": 0, "DEAF": 0, "GONE": 0, "times": []} for b in BOXES}

for cyc in range(1, CYCLES + 1):
    pi("sudo -n systemctl restart dserv")
    t0 = time.time()
    pending = set(BOXES)
    published = {}          # box -> seconds when state/* reappeared

    while pending and time.time() - t0 < RECOVER_TIMEOUT:
        time.sleep(3.0)
        for b in list(pending):
            c0 = dp(b, "cmds_rx")
            if c0 is None:
                continue                      # not publishing yet
            published.setdefault(b, time.time() - t0)
            # Uplink is back. Now prove the DOWNLINK independently.
            pi("dservctl -c 'dservSet extio/%s/cmd/do/18 1' >/dev/null 2>&1; "
               "dservctl -c 'dservSet extio/%s/cmd/do/18 0' >/dev/null 2>&1" % (b, b))
            time.sleep(1.5)
            c1 = dp(b, "cmds_rx")
            if c1 is not None and c1 > c0:
                el = time.time() - t0
                tally[b]["PASS"] += 1
                tally[b]["times"].append(el)
                print("cycle %2d %-5s PASS  published=%.0fs recovered=%.0fs"
                      % (cyc, b, published[b], el), flush=True)
                pending.discard(b)

    for b in pending:
        if b in published:
            tally[b]["DEAF"] += 1
            print("cycle %2d %-5s DEAF  published at %.0fs, cmd/* never landed in %ds"
                  % (cyc, b, published[b], int(RECOVER_TIMEOUT)), flush=True)
        else:
            tally[b]["GONE"] += 1
            print("cycle %2d %-5s GONE  never published within %ds"
                  % (cyc, b, int(RECOVER_TIMEOUT)), flush=True)

print()
for b in BOXES:
    t = tally[b]
    line = "==== %s: %d/%d PASS" % (b, t["PASS"], CYCLES)
    if t["DEAF"]:
        line += "  DEAF %d" % t["DEAF"]
    if t["GONE"]:
        line += "  GONE %d" % t["GONE"]
    print(line + " ====")
    if t["times"]:
        ts = sorted(t["times"])
        print("     recovery: min %.0fs  median %.0fs  max %.0fs"
              % (ts[0], ts[len(ts) // 2], ts[-1]))
