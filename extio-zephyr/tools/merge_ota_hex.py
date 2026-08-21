#!/usr/bin/env python3
"""merge_ota_hex.py -- one hex out of MCUboot + the signed app, for HalfKay.

The FRDM boards flash their two sysbuild images over SWD, and `west flash`
places each one for you. A Teensy has no debug port: HalfKay takes exactly ONE
file, so the bootloader and the application have to arrive already merged.

    python3 tools/merge_ota_hex.py -d build-teensy40-ota -o teensy40-ota.hex
    teensy_loader_cli --mcu=TEENSY40 -s -v teensy40-ota.hex

That is the ONE-TIME migration flash -- it needs the Program button, like every
Teensy reflash of this firmware. Afterwards the box updates over the wire and
this script is only needed again if MCUboot itself changes.

USE THE *SIGNED* APP. zephyr.hex is the unsigned link and MCUboot will refuse
it, leaving a box that enumerates nothing; zephyr.signed.hex carries the image
header and signature MCUboot validates. This script defaults to the signed one
and refuses to merge the unsigned image, because the failure is silent and the
recovery is another button press.

Overlap is checked, not assumed: if the two images collide, the layout is wrong
(most likely MCUboot outgrew boot_partition), and merging them would produce a
file that flashes cleanly and boots into nothing.
"""

import argparse
import pathlib
import sys

from intelhex import IntelHex

# Nothing may extend past here on the teensy40 -- the last sector of the 2 MB
# W25Q16 accepts an erase and silently ignores it. See boards/teensy40.overlay.
# This is a FLASH-RELATIVE offset; the hex files are not.
DEAD_SECTOR = 0x1FF000

# RT1062 XIP window. Zephyr emits hex records at the mapped address
# (0x60000000 + offset) and HalfKay expects exactly that, so every bound in this
# script converts before comparing against the flash-relative partition table.
# Getting this wrong is not academic: the first cut of this script compared a
# mapped address against a flash offset and refused a perfectly good image.
FLASH_BASE = 0x60000000


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-d", "--build-dir", default="build-teensy40-ota",
                    help="sysbuild directory (default: %(default)s)")
    ap.add_argument("-o", "--output", default="teensy40-ota.hex")
    ap.add_argument("--app-image", default="extio-zephyr",
                    help="app image name inside the sysbuild dir")
    ap.add_argument("--allow-unsigned", action="store_true",
                    help="merge zephyr.hex instead of zephyr.signed.hex (MCUboot "
                         "will reject the result -- for layout inspection only)")
    args = ap.parse_args()

    build = pathlib.Path(args.build_dir)
    boot = build / "mcuboot" / "zephyr" / "zephyr.hex"
    name = "zephyr.hex" if args.allow_unsigned else "zephyr.signed.hex"
    app = build / args.app_image / "zephyr" / name

    for p in (boot, app):
        if not p.is_file():
            sys.exit(f"missing {p}\nbuild first:\n"
                     f"  west build -b teensy40 --sysbuild . -d {args.build_dir}")

    ih_boot, ih_app = IntelHex(str(boot)), IntelHex(str(app))
    b0, b1 = ih_boot.minaddr(), ih_boot.maxaddr()
    a0, a1 = ih_app.minaddr(), ih_app.maxaddr()

    if b1 >= a0:
        sys.exit(f"images OVERLAP: mcuboot 0x{b0:x}-0x{b1:x} runs into app "
                 f"0x{a0:x}-0x{a1:x}.\nMCUboot has most likely outgrown "
                 f"boot_partition -- widen it in boards/teensy40_partitions.dtsi "
                 f"(and move slot0/slot1 down to match).")

    merged = IntelHex()
    merged.merge(ih_boot, overlap="error")
    merged.merge(ih_app, overlap="error")

    end_rel = merged.maxaddr() - FLASH_BASE
    if end_rel >= DEAD_SECTOR:
        sys.exit(f"merged image ends at flash offset 0x{end_rel:x}, at or past "
                 f"the dead sector 0x{DEAD_SECTOR:x} -- that region cannot be "
                 f"erased and the flash will not take it.")

    merged.write_hex_file(args.output)
    rel = lambda a: a - FLASH_BASE
    print(f"mcuboot   0x{rel(b0):06x}-0x{rel(b1):06x}  {b1 - b0 + 1:>7} B  ({boot})")
    print(f"app       0x{rel(a0):06x}-0x{rel(a1):06x}  {a1 - a0 + 1:>7} B  ({app})")
    print(f"-> {args.output}   ends 0x{end_rel:06x}, "
          f"{DEAD_SECTOR - end_rel} B clear of the dead sector")
    print("   (offsets are flash-relative; the hex itself is mapped at "
          f"0x{FLASH_BASE:08x}, which is what HalfKay wants)")
    if args.allow_unsigned:
        print("WARNING: unsigned app -- MCUboot will refuse to boot this.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
