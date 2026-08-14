#!/usr/bin/env python3
"""boxname.py -- ask the board on the J-Link VCOM what its persisted name is.

Companion to whichboard.sh: that tells you WHICH physical board (probe serial),
this tells you which BOX it was configured as (box1/box2/box3).

The MCU-Link USB-C gives you the probe plus its VCOM, which carries the board's
hardware UART.  Whether the Zephyr console lands there depends on the box's
`console` setting -- with console=usb the VCOM is silent and you have to plug
the OTG USB-C in as well to reach the box's own CDC console.

Prints the banner/reply verbatim; grep it yourself.  Read-only apart from the
newline + `show` it sends, which the shell answers without changing anything.
"""
import glob
import sys
import time

import serial

ports = sorted(glob.glob("/dev/cu.usbmodem*"))
if not ports:
    sys.exit("no /dev/cu.usbmodem* -- is the MCU-Link cable in?")

port = sys.argv[1] if len(sys.argv) > 1 else ports[-1]
print(f"reading {port}  (candidates: {', '.join(ports)})\n", file=sys.stderr)

with serial.Serial(port, 115200, timeout=0.4) as s:
    s.reset_input_buffer()
    for probe in (b"\r\n", b"show\r\n"):
        s.write(probe)
        time.sleep(0.6)
    deadline = time.time() + 2.5
    out = b""
    while time.time() < deadline:
        chunk = s.read(4096)
        if chunk:
            out += chunk
            deadline = time.time() + 0.8

if not out.strip():
    print("(silent -- console is probably on the box's own USB CDC, not the "
          "J-Link VCOM; plug the OTG USB-C port in too)")
else:
    sys.stdout.write(out.decode("utf-8", "replace"))
