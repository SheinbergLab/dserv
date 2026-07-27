#!/usr/bin/env python3
"""scope_skew.py -- measure two-box output simultaneity on a Digilent, N times.

Reads the ELECTRICAL truth: two box DO pins wired to DIO0 and DIO1, captured by
the Analog Discovery's logic analyzer, delta computed between the two rising
edges. Everything sync_fire.sh prints is a box describing itself; this is the
only thing in the project that isn't.

Why automate it: the skew is a DISTRIBUTION, not a number. Reading single
captures off the WaveForms screen on 2026-07-26 gave "~7 us" -- which was luck;
the spread was +/-100 us and it was the Zephyr tick. A shape cannot be seen one
shot at a time. This also makes the measurement a regression test: change the
tick or the fire path, re-run, compare distributions.

Runs on the machine the Digilent is plugged into, and fires the boxes over ssh.
The capture must be ARMED BEFORE the fire, so this script has to own both sides.

  scope_skew.py [-n 100] [-l 150] [--pin 18] [--host raspberrypi.local]
                [--boxes extio/box1 extio/box2]

NOTE: the WaveForms GUI holds the device exclusively -- close it first, or this
reports "no device found".
"""
import argparse
import ctypes
import subprocess
import sys
import time

# The SDK framework, installed from the WaveForms .dmg. Prefer the system copy:
# the one inside WaveForms.app resolves its Digilent Adept dependencies through
# @rpath relative to the app's own executable, so loading it from python gets a
# library that reports a version happily and then enumerates ZERO devices --
# a working-looking handle to nothing.
DWF_PATHS = [
    "/Library/Frameworks/dwf.framework/dwf",
    "/Applications/WaveForms.app/Contents/Frameworks/dwf.framework/dwf",
]

# dwf constants (see dwf.h)
TRIGSRC_DETECTOR_DIGITAL_IN = 3
STATE_DONE = 2


def open_device():
    d = None
    for p in DWF_PATHS:
        try:
            d = ctypes.cdll.LoadLibrary(p)
            break
        except OSError:
            continue
    if d is None:
        sys.exit("cannot load dwf. Install the SDK from the WaveForms .dmg:\n"
                 "  sudo ditto /Volumes/WaveForms/dwf.framework /Library/Frameworks/dwf.framework")
    n = ctypes.c_int()
    d.FDwfEnum(ctypes.c_int(0), ctypes.byref(n))
    if n.value == 0:
        sys.exit("no Digilent device found.\n"
                 "The WaveForms GUI holds the device exclusively -- close it and retry.")
    hdwf = ctypes.c_int()
    if d.FDwfDeviceOpen(ctypes.c_int(-1), ctypes.byref(hdwf)) == 0 or hdwf.value == 0:
        err = ctypes.create_string_buffer(512)
        d.FDwfGetLastErrorMsg(err)
        sys.exit("FDwfDeviceOpen failed: %s" % err.value.decode())
    return d, hdwf


def configure(d, hdwf, want_window_us):
    """Pick the fastest divider whose buffer still spans want_window_us."""
    hz = ctypes.c_double()
    d.FDwfDigitalInInternalClockInfo(hdwf, ctypes.byref(hz))
    nmax = ctypes.c_int()
    d.FDwfDigitalInBufferSizeInfo(hdwf, ctypes.byref(nmax))
    nsamp = nmax.value

    div = 1
    while (nsamp * div) / hz.value * 1e6 < want_window_us:
        div *= 2
    rate = hz.value / div

    d.FDwfDigitalInDividerSet(hdwf, ctypes.c_int(div))
    d.FDwfDigitalInSampleFormatSet(hdwf, ctypes.c_int(8))     # DIO0-7, 1 byte/sample
    d.FDwfDigitalInBufferSizeSet(hdwf, ctypes.c_int(nsamp))
    # acqmodeSingle. Without it the instrument completes and immediately RE-ARMS,
    # so a poll that starts after the fire finds Armed and reads "never
    # triggered" -- the capture is long gone. Cost an hour; the state trace was
    # Armed -> Done -> Armed within 800 ms.
    d.FDwfDigitalInAcquisitionModeSet(hdwf, ctypes.c_int(0))
    d.FDwfDigitalInTriggerAutoTimeoutSet(hdwf, ctypes.c_double(0))   # no auto-trigger
    # Samples kept AFTER the trigger; the rest is pre-trigger context.
    d.FDwfDigitalInTriggerPositionSet(hdwf, ctypes.c_int(nsamp - nsamp // 8))
    d.FDwfDigitalInTriggerSourceSet(hdwf, ctypes.c_ubyte(TRIGSRC_DETECTOR_DIGITAL_IN))
    # (low, high, rise, fall) bitmasks -- rising edge on DIO0 only.
    d.FDwfDigitalInTriggerSet(hdwf, ctypes.c_int(0), ctypes.c_int(0),
                              ctypes.c_int(1 << 0), ctypes.c_int(0))
    return nsamp, rate


def first_rise(buf, bit, start=0):
    """Index of the first 0->1 transition of `bit`, or None."""
    mask = 1 << bit
    prev = buf[start] & mask
    for i in range(start + 1, len(buf)):
        cur = buf[i] & mask
        if not prev and cur:
            return i
        prev = cur
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", "--shots", type=int, default=100)
    ap.add_argument("-l", "--lead-ms", type=int, default=150)
    ap.add_argument("--pin", type=int, default=18)
    ap.add_argument("--host", default="raspberrypi.local")
    ap.add_argument("--boxes", nargs=2, default=["extio/box1", "extio/box2"])
    ap.add_argument("--window-us", type=float, default=400.0,
                    help="capture span; must comfortably exceed the largest skew")
    ap.add_argument("--same-box", metavar="BOX", default=None,
                    help="fire TWO PINS on ONE box instead of one pin on two boxes. "
                         "Both pins share a clock and a tick phase, so PTP and tick "
                         "quantisation contribute exactly ZERO -- whatever skew "
                         "remains is pure fire-path variance. Use with --pins.")
    ap.add_argument("--pins", nargs=2, type=int, metavar=("P1", "P2"), default=[18, 20],
                    help="the two pins for --same-box (DIO0=P1, DIO1=P2)")
    a = ap.parse_args()

    d, hdwf = open_device()
    nsamp, rate = configure(d, hdwf, a.window_us)
    ns_per = 1e9 / rate
    print("device open: %d samples @ %.1f MS/s (%.1f ns/sample, %.0f us window)"
          % (nsamp, rate / 1e6, ns_per, nsamp / rate * 1e6))
    print("firing %s and %s, pin %d, lead %d ms\n" % (a.boxes[0], a.boxes[1], a.pin, a.lead_ms))

    if a.same_box:
        # sync_fire.sh is one-pin/N-boxes; this is one-box/two-pins, so build it
        # inline. sched/abs_err is per-BOX (the second at_abs overwrites the
        # first), so it cannot verify two pins -- verify from each pin's own
        # state/do/<n> and its stamp instead, which the box sets to the INTENDED
        # time T. A leftover from an earlier round shows as a wild delta.
        p1, p2 = a.pins
        fire = (
            "D=%s; "
            "dservctl -c \"dservSet $D/cmd/do/%d 0\" >/dev/null 2>&1; "
            "dservctl -c \"dservSet $D/cmd/do/%d 0\" >/dev/null 2>&1; sleep 0.2; "
            "N=$(dservctl -c now|tr -d '[:space:]'); T=$((N + %d)); "
            "dservctl -c \"dservSet $D/cmd/do/%d/at_abs $T\" >/dev/null 2>&1; "
            "dservctl -c \"dservSet $D/cmd/do/%d/at_abs $T\" >/dev/null 2>&1; "
            "sleep %.2f; "
            "V1=$(dservctl -c \"dservGet $D/state/do/%d\" 2>/dev/null|head -1); "
            "V2=$(dservctl -c \"dservGet $D/state/do/%d\" 2>/dev/null|head -1); "
            "S1=$(dservctl -c \"dservTimestamp $D/state/do/%d\" 2>/dev/null|tr -d '[:space:]'); "
            "S2=$(dservctl -c \"dservTimestamp $D/state/do/%d\" 2>/dev/null|tr -d '[:space:]'); "
            "echo RESULT $V1 $((S1-T)) $V2 $((S2-T))"
            % (a.same_box, p1, p2, a.lead_ms * 1000, p1, p2,
               a.lead_ms / 1000.0 + 0.6, p1, p2, p1, p2))
        print("SAME-BOX mode: %s pins %d (DIO0) and %d (DIO1)" % (a.same_box, p1, p2))
        print("  PTP and tick quantisation contribute ZERO here -- any skew is the fire path.\n")
    else:
        fire = ("cd ~/extio-bench && sh sync_fire.sh %d %d %s %s"
                % (a.lead_ms, a.pin, a.boxes[0], a.boxes[1]))

    deltas, skipped = [], 0
    buf = (ctypes.c_uint8 * nsamp)()
    sts = ctypes.c_ubyte()

    for shot in range(1, a.shots + 1):
        d.FDwfDigitalInConfigure(hdwf, ctypes.c_int(1), ctypes.c_int(1))   # reconfigure + arm

        # Fire ASYNCHRONOUSLY and poll while it happens. The edge lands in the
        # middle of the ssh call, so polling only after it returns misses the
        # window entirely.
        proc = subprocess.Popen(["ssh", "-o", "BatchMode=yes", a.host, fire],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        triggered = False
        t0 = time.time()
        # Poll with fReadData=0: asking for the buffer on every pass drags it
        # over USB while the instrument is still armed and the acquisition never
        # completes. Fetch the data ONCE, after Done.
        while time.time() - t0 < a.lead_ms / 1000.0 + 15.0:
            d.FDwfDigitalInStatus(hdwf, ctypes.c_int(0), ctypes.byref(sts))
            if sts.value == STATE_DONE:
                triggered = True
                break
            if proc.poll() is not None and time.time() - t0 > a.lead_ms / 1000.0 + 3.0:
                break
            time.sleep(0.01)
        if triggered:
            d.FDwfDigitalInStatus(hdwf, ctypes.c_int(1), ctypes.byref(sts))

        out = proc.communicate(timeout=30)[0] or ""
        # Only trust a shot where BOTH boxes said armed -- sync_fire.sh reports
        # late/unsynced/NO-REPLY otherwise, and a scope delta from a half-fired
        # shot is not a measurement of anything.
        #
        # Parse the TABLE ROWS, not the whole output: sync_fire.sh's trailing
        # NOTE explains what "armed" means, so a substring count over stdout
        # matches the help text and passes every shot. That bug reported
        # "both armed" for boxes that were answering `unsynced` and never fired.
        if a.same_box:
            ok, why = False, "no RESULT line"
            for line in out.splitlines():
                f = line.split()
                if len(f) == 5 and f[0] == "RESULT":
                    try:
                        v1, d1, v2, d2 = f[1], int(f[2]), f[3], int(f[4])
                    except ValueError:
                        why = "unparsable RESULT"
                        break
                    if v1 != "1" or v2 != "1":
                        why = "pin levels %s/%s (a pin did not fire)" % (v1, v2)
                    elif abs(d1) > 1000000 or abs(d2) > 1000000:
                        why = "stale stamps (%d/%d us from T)" % (d1, d2)
                    else:
                        ok = True
                    break
            if not ok:
                skipped += 1
                print("shot %3d: SKIP (%s)" % (shot, why))
                continue
            armed = {1, 2}          # satisfy the shared check below
        else:
            armed = set()
            for line in out.splitlines():
                f = line.split()
                if len(f) >= 2 and f[0] in a.boxes and f[1] == "armed":
                    armed.add(f[0])
        if len(armed) != 2:
            skipped += 1
            status = {}
            for line in out.splitlines():
                f = line.split()
                if len(f) >= 2 and f[0] in a.boxes:
                    status[f[0]] = f[1]
            print("shot %3d: SKIP (not both armed: %s)"
                  % (shot, ", ".join("%s=%s" % (b, status.get(b, "?")) for b in a.boxes)))
            continue
        if not triggered:
            skipped += 1
            print("shot %3d: SKIP (capture never triggered)" % shot)
            continue

        d.FDwfDigitalInStatusData(hdwf, ctypes.byref(buf), ctypes.c_int(nsamp))
        i0 = first_rise(buf, 0)
        i1 = first_rise(buf, 1)
        if i0 is None or i1 is None:
            skipped += 1
            print("shot %3d: SKIP (edge missing: DIO0=%s DIO1=%s)" % (shot, i0, i1))
            continue

        dns = (i1 - i0) * ns_per
        deltas.append(dns)
        print("shot %3d: %+9.0f ns" % (shot, dns))

    d.FDwfDeviceClose(hdwf)

    if not deltas:
        sys.exit("\nno usable shots (%d skipped)" % skipped)

    s = sorted(deltas)
    n = len(s)
    absd = sorted(abs(x) for x in s)
    print("\n==== %d shots (%d skipped) ====" % (n, skipped))
    print("  min %+.0f ns   median %+.0f ns   max %+.0f ns"
          % (s[0], s[n // 2], s[-1]))
    print("  |skew|: median %.0f ns  p90 %.0f ns  p99 %.0f ns  max %.0f ns"
          % (absd[n // 2], absd[int(n * 0.90)], absd[min(int(n * 0.99), n - 1)], absd[-1]))
    print("  sign: DIO1 later %d, DIO0 later %d  (a consistent sign is a fixed\n"
          "        offset; random signs are quantisation or jitter)"
          % (sum(1 for x in s if x > 0), sum(1 for x in s if x < 0)))

    # Histogram over |skew| -- the shape is the point.
    print("\n  |skew| histogram:")
    hi = absd[-1] or 1.0
    nb = 12
    w = hi / nb
    for b in range(nb):
        lo, up = b * w, (b + 1) * w
        c = sum(1 for x in absd if lo <= x < up or (b == nb - 1 and x == up))
        print("   %7.1f-%7.1f us | %-40s %d"
              % (lo / 1000, up / 1000, "#" * min(40, c), c))


if __name__ == "__main__":
    main()
