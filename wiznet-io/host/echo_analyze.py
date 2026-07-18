#!/usr/bin/env python3
"""
echo_analyze.py -- measure the handheld radio path's timestamp error against a
local reference, so echo-sync (BLE.md "Time") has a ground-truth yardstick.

THE METHOD (shared physical edge): one wire carries the SAME edge to a receiver
pin (near-zero, local -> the reference) and a handheld DI pin (stamped at the
handheld, radioed back, rewritten at the receiver -> the thing under test). Both
land in dserv as datapoints; the DIFFERENCE of their dserv timestamps for the
same edge is the end-to-end error of the handheld path relative to the receiver.

    # drive the shared edge + capture (see echo_soak.sh):
    dservctl --json listen --for 3m \
        "extio/pico/state/do/14" "extio/hh1/state/di/26" > echo_run.jsonl &
    sh echo_soak.sh -n 200
    python3 echo_analyze.py --ref extio/pico/state/do/14 \
                            --test extio/hh1/state/di/26 echo_run.jsonl

  ref  = the near-truth local event (receiver DO actuation, box_clock-stamped,
         OR a receiver DI on the same wire). test = the handheld's report.
  delta = test_ts - ref_ts, per matched edge.  Positive => handheld path lags
         (today: by the radio latency; after echo-sync: toward ~0 +/- ms).

Reports N, median/mean, stdev, IQR, min/max, and the p10..p90 spread -- the
median is the residual offset error, the spread is the jitter echo-sync must
also tame (min-RTT filtering targets the FLOOR, so watch min and p10 too).

Dependency-free (stdlib only), same decode contract as sync_analyze.py.
"""
import argparse
import base64
import json
import statistics
import struct
import sys


def decode_value(dtype, raw):
    """dserv --json data field -> int/float/str (number, ascii, or base64 of
    ascii / raw LE int32|int64). Same best-effort ladder as sync_analyze.py."""
    if isinstance(raw, (int, float)):
        return raw
    if not isinstance(raw, str):
        return raw
    try:
        b = base64.b64decode(raw, validate=True)
    except Exception:
        b = None
    if b is not None:
        try:
            return int(b.decode().strip())
        except Exception:
            pass
        try:
            return float(b.decode().strip())
        except Exception:
            pass
        if len(b) == 4:
            return struct.unpack("<i", b)[0]
        if len(b) == 8:
            return struct.unpack("<q", b)[0]
    try:
        return int(raw.strip())
    except Exception:
        pass
    try:
        return float(raw.strip())
    except Exception:
        return raw


def load_events(path, name):
    """Return [(timestamp_us, value), ...] for one datapoint name, time-sorted.
    A timestamp of 0 means dserv arrival-stamping -- still a real dserv time on
    the wire (the daemon fills now()), so we keep whatever the capture recorded."""
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except Exception:
                continue
            if d.get("name") != name:
                continue
            ts = d.get("timestamp")
            if ts is None:
                continue
            out.append((int(ts), decode_value(d.get("dtype"), d.get("data"))))
    out.sort(key=lambda e: e[0])
    return out


def pair(ref, test, window_us, ref_val, test_val):
    """Nearest-neighbour match each test event to a ref event within +/-window.
    Optional value filters isolate one edge polarity (e.g. the falling edge)."""
    if ref_val is not None:
        ref = [e for e in ref if e[1] == ref_val]
    if test_val is not None:
        test = [e for e in test if e[1] == test_val]
    deltas, used = [], set()
    j = 0
    for tt, _ in test:
        # advance a window pointer; ref is sorted
        while j < len(ref) and ref[j][0] < tt - window_us:
            j += 1
        best, bi = None, -1
        k = j
        while k < len(ref) and ref[k][0] <= tt + window_us:
            if k not in used:
                dt = tt - ref[k][0]
                if best is None or abs(dt) < abs(best):
                    best, bi = dt, k
            k += 1
        if bi >= 0:
            used.add(bi)
            deltas.append(best)
    return deltas


def pct(xs, p):
    if not xs:
        return float("nan")
    s = sorted(xs)
    i = max(0, min(len(s) - 1, round((p / 100.0) * (len(s) - 1))))
    return s[i]


def main():
    ap = argparse.ArgumentParser(description="handheld radio-path timestamp error vs a local reference")
    ap.add_argument("capture", help="dservctl --json listen JSON-lines file")
    ap.add_argument("--ref", required=True, help="reference datapoint name (near-truth local edge)")
    ap.add_argument("--test", required=True, help="datapoint under test (handheld report)")
    ap.add_argument("--window", type=float, default=100.0, help="match window, ms (default 100)")
    ap.add_argument("--ref-val", type=int, default=None, help="only ref events with this value (edge select)")
    ap.add_argument("--test-val", type=int, default=None, help="only test events with this value (edge select)")
    a = ap.parse_args()

    ref = load_events(a.capture, a.ref)
    test = load_events(a.capture, a.test)
    if not ref or not test:
        sys.exit(f"no events: ref={len(ref)} ({a.ref}), test={len(test)} ({a.test}) -- check names/capture")

    d_us = pair(ref, test, a.window * 1000.0, a.ref_val, a.test_val)
    if not d_us:
        sys.exit(f"0 pairs within +/-{a.window}ms -- widen --window or check edge alignment")
    d_ms = [x / 1000.0 for x in d_us]

    print(f"ref  {a.ref}: {len(ref)} events")
    print(f"test {a.test}: {len(test)} events")
    print(f"paired: {len(d_ms)} edges (window +/-{a.window:.0f}ms)")
    print(f"\ndelta = test_ts - ref_ts  (ms; +ve => handheld path lags):")
    print(f"  median {statistics.median(d_ms):+8.3f}   mean {statistics.fmean(d_ms):+8.3f}")
    if len(d_ms) > 1:
        print(f"  stdev  {statistics.stdev(d_ms):8.3f}   IQR  {pct(d_ms,75)-pct(d_ms,25):8.3f}")
    print(f"  min    {min(d_ms):+8.3f}   max  {max(d_ms):+8.3f}")
    print(f"  p10 {pct(d_ms,10):+7.3f}  p50 {pct(d_ms,50):+7.3f}  p90 {pct(d_ms,90):+7.3f}")
    print(f"\ninterpret: median = residual offset error, spread = jitter echo-sync must tame.")
    print(f"min/p10 approach the true one-way floor (what min-RTT filtering chases).")


if __name__ == "__main__":
    main()
