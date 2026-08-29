#!/usr/bin/env python3
"""
Build the Mouser order BOM for photomarker rev B.

Reference designators and per-board quantities are read from the .kicad_pcb, so
they cannot drift from the board. Manufacturer part numbers come from the table
below and are tagged:

    OK      verified against a distributor listing
    CHECK   follows the manufacturer's documented numbering scheme -- confirm in
            the cart. A mis-derived suffix silently gets you the wrong tolerance
            or dielectric.

Writes:
    fab/bom-mouser.csv   Mfr Part Number + Quantity, for Mouser's BOM upload
    fab/bom.md           human-readable, with the specs that actually matter

  .../Python.framework/Versions/3.9/bin/python3 make_bom.py [boards]
"""
import collections
import csv
import math
import os
import sys

import pcbnew

D = os.path.dirname(os.path.abspath(__file__))
BOARDS = int(sys.argv[1]) if len(sys.argv) > 1 else 5

# value -> (mfr part, manufacturer, status, spare policy, note)
#   spare: multiplier applied to the per-build count, then rounded up
PARTS = {
    "OPT101":     ("OPT101P",            "TI",      "OK",
                   2.0, "8-DIP clear. Legacy THT analog part -- buy deep."),
    "LM393":      ("LM393P",             "TI",      "CHECK",
                   2.0, "DIP-8. LM393N is the onsemi equivalent."),
    "50k":        ("3296W-1-503LF",      "Bourns",  "OK",
                   1.2, "25-turn cermet trimmer, top adjust."),
    "10k":        ("MFR-25FBF52-10K",    "Yageo",   "CHECK", 1.3, "1%"),
    "270k":       ("MFR-25FBF52-270K",   "Yageo",   "CHECK", 2.0, "1%"),
    "100R":       ("MFR-25FBF52-100R",   "Yageo",   "CHECK", 2.0, "1%"),
    "2k2":        ("MFR-25FBF52-2K2",    "Yageo",   "CHECK", 2.0, "1%"),
    "100n":       ("K104K15X7RF5UH5",    "Vishay",  "CHECK",
                   2.0, "100nF 50V X7R disc, 5mm pitch. Any 0.1uF THT is fine."),
    "3n3 C0G":    ("FKP2D013301G00JSSD", "WIMA",   "OK",
                   2.0, "3.3nF 100V +-2% polypropylene film, 5mm pitch. Film is "
                        "the easy THT answer here; a C0G/NP0 ceramic is equally "
                        "fine. NEVER X7R -- it drifts with temperature and DC "
                        "bias and would turn the calibrated lag into a drifting one."),
    "JST-PH 5":   ("B5B-PH-K-S(LF)(SN)", "JST",     "CHECK",
                   2.0, "5-pin PH vertical header, board side."),
}
# things the board does not carry a footprint for but you still need
EXTRAS = [
    ("DIP-8 socket",   "1-2199298-2",       "TE",   "CHECK", 2, BOARDS * 2,
     "One each for U1 and U2. Keeps soldering heat off the OPT101 entirely."),
    ("PH housing",     "PHR-5",             "JST",  "CHECK", 2, BOARDS,
     "Cable-side mating shell for J1."),
    ("PH crimps",      "SPH-002T-P0.5S",    "JST",  "CHECK", 2, BOARDS * 5,
     "22-28 AWG. Needs the right crimp tool -- else buy ready-made pigtails."),
]


def main():
    board = pcbnew.LoadBoard(os.path.join(D, "photomarker-tht.kicad_pcb"))
    fitted, dnp = collections.Counter(), collections.defaultdict(list)
    refs = collections.defaultdict(list)
    for fp in board.Footprints():
        ref, val = fp.GetReference(), fp.GetValue()
        if ref.startswith(("H", "TP")) or val == "DNP" and fp.IsDNP():
            if fp.IsDNP():
                dnp[val].append(ref)
            continue
        if fp.IsDNP():
            dnp[val].append(ref)
            continue
        fitted[val] += 1
        refs[val].append(ref)

    rows, missing = [], []
    for val, per_board in sorted(fitted.items()):
        mpn, mfr, status, spare, note = PARTS.get(val, ("", "", "PICK", 2.0, ""))
        qty = int(math.ceil(per_board * BOARDS * spare))
        if not mpn:
            missing.append(val)
        rows.append((val, ",".join(sorted(refs[val])), per_board, qty,
                     mpn, mfr, status, note))
    for name, mpn, mfr, status, _s, qty, note in EXTRAS:
        rows.append((name, "-", "-", qty, mpn, mfr, status, note))

    with open(os.path.join(D, "fab", "bom-mouser.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Mfr Part Number", "Quantity", "Description", "Designators"])
        for val, r, _pb, qty, mpn, mfr, status, note in rows:
            w.writerow([mpn or "PICK-ONE", qty, "%s %s" % (mfr, val), r])

    with open(os.path.join(D, "fab", "bom.md"), "w") as f:
        f.write("# Mouser BOM - photomarker rev B (through-hole)\n\n")
        f.write("For **%d boards**, hand-assembled. Quantities include spares.\n\n" % BOARDS)
        f.write("`OK` = verified listing. `CHECK` = derived from the maker's scheme, "
                "confirm in the cart. `PICK` = you must choose one.\n\n")
        f.write("| Qty | Value | Designators | Mfr part | Mfr | ? | Note |\n")
        f.write("|----:|-------|-------------|----------|-----|---|------|\n")
        for val, r, _pb, qty, mpn, mfr, status, note in rows:
            f.write("| %d | %s | %s | `%s` | %s | %s | %s |\n"
                    % (qty, val, r, mpn or "-", mfr or "-", status, note))
        f.write("\n## Not populated (DNP)\n\nBuy only if you want the option.\n\n")
        for val, rs in sorted(dnp.items()):
            f.write("- **%s** - %s\n" % (", ".join(sorted(rs)), val))
        f.write("\nR6 is the OPT101 gain resistor: **pick its value after seeing TP1 "
                "on a real screen**, not now.\n")

    print("%d boards -> %d line items" % (BOARDS, len(rows)))
    for val, r, pb, qty, mpn, mfr, status, _n in rows:
        print("  %-4s x %-14s %-22s %s" % (qty, val, mpn or "PICK-ONE", status))
    if missing:
        print("\nNEEDS A PART NUMBER: %s" % ", ".join(missing))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
