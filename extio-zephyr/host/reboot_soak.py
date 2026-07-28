#!/usr/bin/env python3
"""reboot_soak.py -- how reliably does a box get from cold boot to a WORKING
dserv registration?

Pass condition is deliberately not "it published something": state/* flowing
while cmd/* never arrives is the exact failure this is measuring. So a cycle
only passes when the DOWNLINK is proven -- we send commands and require
state/cmds_rx to actually move.

Boot is detected by state/watchdog going BACKWARDS (it resets to 0 and climbs at
1 Hz), which distinguishes the new instance from leftovers in dserv's last-value
store. Without that check a stale pre-reboot value reads as instant success.

  reboot_soak.py <box> <vcom> <cycles>
"""
import subprocess, sys, time, os
import serial

BOX, VCOM, CYCLES = sys.argv[1], sys.argv[2], int(sys.argv[3])
PI = "raspberrypi.local"
BOOT_TIMEOUT = 60.0


def pi(cmd, timeout=45):
    try:
        r = subprocess.run(["ssh", "-o", "BatchMode=yes", PI, cmd],
                           capture_output=True, text=True, timeout=timeout)
        return r.stdout.strip()
    except Exception:
        return ""


def dp(key):
    """One datapoint as int, or None if absent/unparsable."""
    out = pi("dservctl -c 'dservGet extio/%s/state/%s' 2>/dev/null | head -1" % (BOX, key))
    try:
        return int(out.split()[0])
    except Exception:
        return None


def send_reboot():
    """Cold-reboot over the MCU-Link VCOM, tolerating USB re-enumeration."""
    t0 = time.time()
    while time.time() - t0 < 30:
        if os.path.exists(VCOM):
            try:
                s = serial.Serial(VCOM, 115200, timeout=0.2, exclusive=True)
                time.sleep(0.2)
                s.write(b"kernel reboot cold\r\n")
                s.flush()
                time.sleep(1.0)
                s.close()
                return True
            except Exception:
                time.sleep(0.5)
        else:
            time.sleep(0.5)
    return False


results = []
for cyc in range(1, CYCLES + 1):
    w_before = dp("watchdog")
    if not send_reboot():
        results.append((cyc, "NO-CONSOLE", None, None))
        print("cycle %2d: NO-CONSOLE (could not open %s)" % (cyc, VCOM), flush=True)
        continue

    t0 = time.time()
    booted_at = None
    passed_at = None
    while time.time() - t0 < BOOT_TIMEOUT:
        time.sleep(2.0)
        w = dp("watchdog")
        # Boot detected: watchdog reset (went backwards or reappeared small).
        if booted_at is None:
            if w is not None and (w_before is None or w < w_before or w < 15):
                booted_at = time.time() - t0
        if booted_at is not None:
            # Downlink test: drive the pin and require cmds_rx to move.
            c0 = dp("cmds_rx")
            pi("dservctl -c 'dservSet extio/%s/cmd/do/18 1' >/dev/null 2>&1; "
               "dservctl -c 'dservSet extio/%s/cmd/do/18 0' >/dev/null 2>&1" % (BOX, BOX))
            time.sleep(1.5)
            c1 = dp("cmds_rx")
            if c0 is not None and c1 is not None and c1 > c0:
                passed_at = time.time() - t0
                break

    if passed_at is not None:
        results.append((cyc, "PASS", booted_at, passed_at))
        print("cycle %2d: PASS   boot=%.0fs registered=%.0fs" % (cyc, booted_at, passed_at), flush=True)
    elif booted_at is not None:
        results.append((cyc, "DEAF", booted_at, None))
        print("cycle %2d: DEAF   booted at %.0fs but cmd/* never landed in %ds"
              % (cyc, booted_at, int(BOOT_TIMEOUT)), flush=True)
    else:
        results.append((cyc, "NO-BOOT", None, None))
        print("cycle %2d: NO-BOOT no watchdog reset within %ds" % (cyc, int(BOOT_TIMEOUT)), flush=True)

npass = sum(1 for r in results if r[1] == "PASS")
times = [r[3] for r in results if r[3] is not None]
print("\n==== %s: %d/%d PASS ====" % (BOX, npass, CYCLES))
for kind in ("DEAF", "NO-BOOT", "NO-CONSOLE"):
    n = sum(1 for r in results if r[1] == kind)
    if n:
        print("  %-10s %d" % (kind, n))
if times:
    times.sort()
    print("  time-to-registered: min %.0fs  median %.0fs  max %.0fs"
          % (times[0], times[len(times) // 2], times[-1]))
