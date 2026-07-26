#!/usr/bin/env python3
"""Read the box's 128-byte dserv frames straight off a CDC tty, no dserv.

Pure stdlib (os + termios) so it runs on a bare Pi. Reports per-frame ARRIVAL
spacing, which is the quantity in question: if the host drains continuously,
frames the box emits microseconds apart arrive together.
"""
import os, sys, termios, time, struct

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM1"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd)
a[0] = a[1] = a[3] = 0                      # iflag oflag lflag: raw
a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
a[6][termios.VMIN] = 0
a[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, a)

buf = b""
frames = []          # (arrival, name)
junk = 0
t0 = time.time()
while time.time() - t0 < secs:
    try:
        d = os.read(fd, 4096)
    except BlockingIOError:
        time.sleep(0.0002)
        continue
    if not d:
        time.sleep(0.0002)
        continue
    now = time.time()
    buf += d
    while len(buf) >= 128:
        if buf[0] not in (0x3E, 0x44):
            buf = buf[1:]; junk += 1; continue
        fr, buf = buf[:128], buf[128:]
        nm = ""
        if fr[0] == 0x3E:
            vl = struct.unpack_from("<H", fr, 1)[0]
            if vl <= 109:
                nm = fr[3:3+vl].decode("ascii", "replace")
        frames.append((now, nm))
os.close(fd)

el = time.time() - t0
print(f"{len(frames)} frames in {el:.1f}s ({len(frames)/el:.1f}/s), junk={junk}")
if len(frames) > 2:
    gaps = [(frames[i][0]-frames[i-1][0])*1e6 for i in range(1, len(frames))]
    g = sorted(gaps)
    n = len(g)
    print(f"inter-frame arrival gap us:  min {g[0]:.0f}  med {g[n//2]:.0f} "
          f" p90 {g[int(n*0.9)]:.0f}  max {g[-1]:.0f}")
    tight = sum(1 for x in gaps if x < 1000)
    print(f"gaps < 1 ms: {tight}/{len(gaps)}  ({100*tight/len(gaps):.0f}%)"
          "   <- high % means the host drains continuously")
    print("\nfirst 12 frames (gap us, name):")
    for i in range(1, min(13, len(frames))):
        print(f"  {(frames[i][0]-frames[i-1][0])*1e6:9.0f}  {frames[i][1]}")
