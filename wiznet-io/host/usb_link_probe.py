#!/usr/bin/env python3
"""usb_link_probe.py -- extio USB data-CDC health check, no wiring needed.
Reads the data interface for N seconds; the box publishes 3 frames/sec
(watchdog+uptime+link), so watchdog-counter gaps directly count lost frames.
Born 2026-07-11 diagnosing a degraded macOS USB stack (30-70% bulk-IN loss,
cured by host reboot). Note: skips can false-positive from the naive '>'
resync walking the DTR-connect seed burst; counter gaps are the truth.
Usage: usb_link_probe.py /dev/cu.usbmodemXXX3 [seconds]"""
import os, sys, termios, select, struct, time, fcntl

DATA = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem103"
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0

fd = os.open(DATA, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd)
a[0] = 0; a[1] = 0
a[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
a[3] = 0
a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, a)
fcntl.ioctl(fd, 0x8004746C, struct.pack('I', termios.TIOCM_DTR | termios.TIOCM_RTS))
termios.tcflush(fd, termios.TCIFLUSH)

raw = bytearray()
t0 = time.time()
while time.time() - t0 < SECS:
    r, _, _ = select.select([fd], [], [], 0.02)
    if fd in r:
        try:
            chunk = os.read(fd, 4096)
            if chunk:
                raw += chunk
        except BlockingIOError:
            pass
os.close(fd)

frames, skips, beats = 0, 0, []
last_skip = -1
i = 0
while i + 128 <= len(raw):
    if raw[i] != ord('>'):
        i += 1; skips += 1; last_skip = i; continue
    f = raw[i:i+128]
    namelen = struct.unpack_from('<H', f, 1)[0]
    if namelen > 109:
        i += 1; skips += 1; continue
    name = f[3:3+namelen].decode('ascii', 'replace')
    off = 3 + namelen
    frames += 1
    if name.endswith('state/watchdog'):
        beats.append(struct.unpack_from('<i', f, off+16)[0])
    i += 128

expect = int(SECS * 3)
gaps = sum(b2 - b1 - 1 for b1, b2 in zip(beats, beats[1:]) if b2 > b1 + 1)
print(f"{DATA}  {SECS:.0f}s")
print(f"raw bytes: {len(raw)} ({len(raw)/128:.2f} frames worth)  parsed: {frames} (~expect {expect})  skips: {skips}")
print(f"watchdog beats seen: {len(beats)}  missing beats (counter gaps): {gaps}")
print(f"skips: {skips} (last at byte offset {last_skip} of {len(raw)})")
steady_clean = gaps == 0 and (last_skip < 2500)  # skips confined to the DTR seed burst
print("VERDICT:", "CLEAN steady-state" if steady_clean else f"LOSSY ({gaps} beats lost, skips through offset {last_skip})")
