#!/usr/bin/env python3
"""Decode the box's 128-byte dserv frames off the USB-HS data pipe and measure
the PTP counter's rate against the host clock.

No dserv involved -- this reads the raw CDC data endpoint and parses the wire
format in src/core/dserv_msg.h directly, so it also serves as a first exercise
of the frame pipe on this board.
"""
import struct
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1103"
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 90.0

LEN = 128
INT, INT64, STRING, FLOAT, DOUBLE = 5, 16, 1, 2, 3


def parse(fr):
    if fr[0] != ord(">"):
        return None
    varlen = struct.unpack_from("<H", fr, 1)[0]
    if varlen > 109:
        return None
    off = 3 + varlen
    name = fr[3:off].decode("ascii", "replace")
    ts, dtype, dlen = struct.unpack_from("<QII", fr, off)
    off += 16
    data = fr[off:off + dlen]
    if dtype == INT and dlen >= 4:
        val = struct.unpack_from("<i", data)[0]
    elif dtype == INT64 and dlen >= 8:
        val = struct.unpack_from("<q", data)[0]
    elif dtype == STRING:
        val = data.decode("ascii", "replace").rstrip("\x00")
    elif dtype == FLOAT and dlen >= 4:
        val = struct.unpack_from("<f", data)[0]
    elif dtype == DOUBLE and dlen >= 8:
        val = struct.unpack_from("<d", data)[0]
    else:
        val = data.hex()
    return name, ts, val


ser = serial.Serial(PORT, 115200, timeout=0.2)
ser.dtr = True

buf = b""
samples, seen, frames, junk = [], {}, 0, 0
t0 = time.time()
reopens = 0
while time.time() - t0 < SECS:
    try:
        chunk = ser.read(4096)
    except serial.SerialException as e:
        # macOS cu.* devices raise this transiently; reopen and carry on.
        reopens += 1
        print(f"[reopen {reopens}: {e}]", file=sys.stderr)
        try:
            ser.close()
        except Exception:
            pass
        time.sleep(0.5)
        ser = serial.Serial(PORT, 115200, timeout=0.2)
        ser.dtr = True
        continue
    if not chunk:
        continue
    buf += chunk
    while len(buf) >= LEN:
        if buf[0] not in (ord(">"), ord("D")):
            buf = buf[1:]
            junk += 1
            continue
        fr, buf = buf[:LEN], buf[LEN:]
        frames += 1
        p = parse(fr)
        if not p:
            continue
        name, ts, val = p
        seen[name] = val
        if name.endswith("/state/ptp/ns"):
            samples.append((time.time(), val))
ser.close()

print(f"frames={frames}  resync-discarded bytes={junk}  distinct keys={len(seen)}")
print(f"ptp/ns samples={len(samples)}\n")
for k in sorted(seen):
    print(f"  {k:34s} = {seen[k]}")

def rate(sl, label):
    if len(sl) < 3:
        print(f"{label}: too few samples")
        return
    h0, p0 = sl[0]
    xs = [h - h0 for h, _ in sl]
    ys = [(p - p0) / 1e9 for _, p in sl]
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    slope = (sum((x - mx) * (y - my) for x, y in zip(xs, ys))
             / sum((x - mx) ** 2 for x in xs))
    ep = ys[-1] / xs[-1]
    resid = [(y - my) - slope * (x - mx) for x, y in zip(xs, ys)]
    worst = max(abs(r) for r in resid)
    print(f"{label}  n={n}  span={xs[-1]:.1f}s")
    print(f"   endpoints  {ep:.7f} x  ({(ep - 1) * 1e6:+8.1f} ppm)")
    print(f"   lsq slope  {slope:.7f} x  ({(slope - 1) * 1e6:+8.1f} ppm)"
          f"   max resid {worst * 1e3:.1f} ms")


print()
rate(samples, "all samples          ")
rate(samples[10:], "skipping first 10    ")
rate(samples[10:-1], "skip first 10 + last ")
