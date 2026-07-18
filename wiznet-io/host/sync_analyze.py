#!/usr/bin/env python3
"""
sync_analyze.py -- first-pass analysis of the extio hardware obs-sync anchors.

Feed it the output of a dservctl listen capture taken while obs periods cycle:

    dservctl --host rpi500.local --json listen "extio/*/state/sync/*" "ess/in_obs" \
        > sync_run.jsonl        # Ctrl-C after ~5-10 min of obs cycling
    python3 sync_analyze.py sync_run.jsonl

Reads JSON-lines with {name, timestamp, dtype, data}; data may be a JSON
number, an ASCII number, base64 of ASCII, or base64 of raw little-endian
int32/int64 -- all are handled, so both today's --json passthrough and any
future decoded --jsonl format parse identically.

Reports:
  1. anchor inventory: hw vs sw counts, by edge polarity (begin/end via the
     ess/in_obs datapoint that shares the anchor's dserv timestamp)
  2. transport_us distribution (frame-behind-edge delay), split by polarity
  3. crystal drift in ppm from consecutive same-polarity anchor pairs
     (constant host pin->stamp skew cancels in the difference)
  4. begin/end differential skew estimate from the polarity alternation
"""
import base64
import json
import statistics
import struct
import sys


def decode_value(dtype, raw):
    """Best-effort decode of a dserv JSON data field to int/float/str."""
    if isinstance(raw, (int, float)):
        return raw
    if not isinstance(raw, str):
        return raw
    s = raw
    try:  # base64 layer, if present
        b = base64.b64decode(s, validate=True)
        try:
            s = b.decode("ascii")
        except UnicodeDecodeError:
            if len(b) == 8:
                return struct.unpack("<q", b)[0]
            if len(b) == 4:
                return struct.unpack("<i", b)[0]
            if len(b) == 2:
                return struct.unpack("<h", b)[0]
            return b.hex()
    except Exception:
        pass
    s = s.strip().rstrip("\x00")
    for conv in (int, float):
        try:
            return conv(s)
        except ValueError:
            continue
    return s


def load(path):
    """Parse capture -> (anchors keyed by dserv_us, in_obs values by ts)."""
    anchors, in_obs = {}, {}
    with open(path) if path != "-" else sys.stdin as f:
        for line in f:
            line = line.strip()
            if not line or not line.startswith("{"):
                continue
            try:
                dp = json.loads(line)
            except json.JSONDecodeError:
                continue
            name = dp.get("name", "")
            ts = int(dp.get("timestamp", dp.get("t", 0)))   # raw --json or --jsonl
            raw = dp["data"] if "data" in dp else dp.get("value")
            val = decode_value(dp.get("dtype"), raw)
            if name == "ess/in_obs":
                in_obs[ts] = int(val)
            elif "/state/sync/" in name:
                leaf = name.rsplit("/", 1)[-1]
                anchors.setdefault(ts, {})[leaf] = val
    return anchors, in_obs


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p / 100.0 * len(xs)))]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "-"
    anchors, in_obs = load(path)

    rows = []  # (dserv_us, box_us, offset, source, transport, polarity)
    for ts in sorted(anchors):
        a = anchors[ts]
        if "dserv_us" not in a or "box_us" not in a:
            continue
        rows.append((int(a["dserv_us"]), int(a["box_us"]),
                     int(a.get("offset_us", 0)), str(a.get("source", "?")),
                     int(a["transport_us"]) if "transport_us" in a else None,
                     in_obs.get(ts, None)))
    if len(rows) < 3:
        print(f"only {len(rows)} complete anchors found -- need obs cycling "
              f"during capture (and both sync/* and ess/in_obs patterns)")
        return 1

    n = len(rows)
    hw = [r for r in rows if r[3] == "hw"]
    span_s = (rows[-1][0] - rows[0][0]) / 1e6
    print(f"anchors: {n} over {span_s:.0f}s   hw={len(hw)} sw={n - len(hw)}"
          + ("   <-- sw anchors present: check TTL/window" if n - len(hw) else ""))
    for pol, tag in ((1, "begin"), (0, "end"), (None, "unpaired")):
        sub = [r for r in rows if r[5] == pol]
        if sub:
            print(f"  {tag:9s} {len(sub):4d}  "
                  f"hw={sum(1 for r in sub if r[3] == 'hw')}")

    tr = [r[4] for r in hw if r[4] is not None]
    if tr:
        print(f"\ntransport_us (hw anchors, n={len(tr)}):")
        print(f"  median={pct(tr, 50)}  p90={pct(tr, 90)}  "
              f"max={max(tr)}  min={min(tr)}")
        for pol, tag in ((1, "begin"), (0, "end")):
            sub = [r[4] for r in hw if r[5] == pol and r[4] is not None]
            if sub:
                print(f"  {tag}: median={pct(sub, 50)}  max={max(sub)}")

    def drift_pairs(sub):
        """ppm from consecutive anchor pairs within one list."""
        out = []
        for (d0, b0, *_), (d1, b1, *_) in zip(sub, sub[1:]):
            dd = d1 - d0
            if dd > 1_000_000:                      # >1s apart: usable pair
                out.append(((b1 - b0) - dd) / dd * 1e6)
        return out

    print("\ncrystal drift, box vs dserv (hw anchors only):")
    for pol, tag in ((1, "begin->begin"), (0, "end->end"), (None, None)):
        sub = ([r for r in hw if r[5] == pol] if pol is not None else hw)
        tag = tag or "all adjacent"
        ppm = drift_pairs(sub)
        if len(ppm) >= 2:
            print(f"  {tag:12s} n={len(ppm):3d}  mean={statistics.mean(ppm):+7.1f} ppm"
                  f"  sd={statistics.stdev(ppm):5.1f}"
                  f"  median={statistics.median(ppm):+7.1f}")
    print("  (same-polarity rows cancel host stamping skew exactly; if the "
          "all-adjacent\n   mean differs, that difference ~= begin-vs-end "
          "stamp-skew asymmetry)")

    offs = [r[2] for r in hw]
    print(f"\noffset_us trajectory: first={offs[0]}  last={offs[-1]}  "
          f"net {offs[-1] - offs[0]:+d} over {span_s:.0f}s "
          f"({(offs[-1] - offs[0]) / span_s if span_s else 0:+.1f} us/s)")

    # firmware-learned rate (state/sync/rate_ppb, present once the box runs
    # the rate-corrected clock) -- should converge to the measured drift
    rates = [int(anchors[ts]["rate_ppb"]) for ts in sorted(anchors)
             if "rate_ppb" in anchors[ts]]
    if rates:
        tail = rates[len(rates) // 2:]
        print(f"\nfirmware rate_ppb: first={rates[0]}  last={rates[-1]}  "
              f"settled mean={statistics.mean(tail):+.0f} ppb "
              f"(= {statistics.mean(tail) / 1000:+.1f} ppm; box drift is the "
              f"NEGATIVE of the measured pair ppm above)")
    else:
        print("\nfirmware rate_ppb: absent (box firmware predates rate "
              "correction, or no hw anchor pairs yet)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
