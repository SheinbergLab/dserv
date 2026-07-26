#!/usr/bin/env python3
"""boxcli.py -- run commands on the extio box's CDC console.

The box exposes two CDCs: console (if00) and data (if02). dserv's extio
subprocess owns the DATA port; the console is normally free, which is what
makes this safe to run against a live, registered box. It is still worth
checking -- a console that "misbehaves" is usually a second opener, not
broken firmware.

    boxcli.py show
    boxcli.py -p /dev/ttyACM0 'name box1' 'save'
    boxcli.py --raw 'net ptp'          # no reply-shaping, print bytes as they land

Exclusive open, so a clash fails loudly instead of interleaving two readers.
CDC ignores the baud rate; it is passed only because the API wants a number.
"""
import argparse
import sys
import time

import serial


def run(ser, cmd, idle, hard):
    """Send one line, then read until the box goes quiet for `idle` seconds."""
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode())
    ser.flush()
    out = bytearray()
    last = time.monotonic()
    t0 = last
    while True:
        chunk = ser.read(4096)
        now = time.monotonic()
        if chunk:
            out += chunk
            last = now
        elif now - last >= idle and out:
            break
        elif now - t0 >= hard:
            break
    return out.decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", default="/dev/ttyACM0")
    ap.add_argument("-i", "--idle", type=float, default=0.35,
                    help="quiet seconds that end a reply (default 0.35)")
    ap.add_argument("-t", "--timeout", type=float, default=4.0,
                    help="hard cap per command (default 4)")
    ap.add_argument("--echo", action="store_true", help="print each command before its reply")
    ap.add_argument("cmds", nargs="+")
    a = ap.parse_args()

    try:
        ser = serial.Serial(a.port, 115200, timeout=0.05, exclusive=True)
    except serial.SerialException as e:
        sys.exit("cannot open %s: %s" % (a.port, e))

    # A freshly-opened CDC often has a banner or partial line queued; drop it so
    # the first command's reply is not prefixed with someone else's output.
    time.sleep(0.15)
    ser.reset_input_buffer()

    for cmd in a.cmds:
        if a.echo or len(a.cmds) > 1:
            print("--- %s" % cmd)
        sys.stdout.write(run(ser, cmd, a.idle, a.timeout))
        sys.stdout.flush()
    ser.close()


if __name__ == "__main__":
    main()
