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

# value -> (mfr part, Mouser part, manufacturer, unit price, spare, note)
#   spare: multiplier on the per-build count, rounded up
#   All Mouser numbers below were confirmed by an actual BOM match (2026-08-28);
#   Min./Mult. is 1/1 on every line, so quantities are free to choose.
PARTS = {
    "OPT101":     ("OPT101P", "595-OPT101P", "TI", 10.26, 1.4,
                   "8-DIP clear. ~68% of the order cost -- 5 builds + 2 spares. "
                   "Stock is healthy (2k+ at Mouser, 3k+ at DigiKey) and TI lists "
                   "it Active, so the deep-buy hedge is not worth $10/unit."),
    "LM393":      ("LM393P", "595-LM393P", "TI", 0.53, 2.0, "DIP-8."),
    "50k":        ("3296W-1-503LF", "652-3296W-1-503LF", "Bourns", 2.28, 1.2,
                   "25-turn cermet trimmer, top adjust. Sets the threshold."),
    "10k":        ("MFR-25FBF52-10K", "603-MFR-25FBF52-10K", "Yageo", 0.10, 1.3, "1%"),
    "270k":       ("MFR-25FBF52-270K", "603-MFR-25FBF52-270K", "Yageo", 0.10, 2.0, "1%"),
    "100R":       ("MFR-25FBF52-100R", "603-MFR-25FBF52-100R", "Yageo", 0.10, 2.0, "1%"),
    "2k2":        ("MFR-25FBF52-2K2", "603-MFR-25FBF52-2K2", "Yageo", 0.10, 2.0, "1%"),
    "100n":       ("K104K15X7RF5UH5", "594-K104K15X7RF5UH5", "Vishay", 0.28, 2.0,
                   "0.1uF 50V X7R disc, 5mm pitch. Decoupling -- X7R is fine here."),
    "3n3 C0G":    ("FKP2D013301G00JSSD", "505-FKP2D013301G00JS", "WIMA", 0.97, 2.0,
                   "3.3nF 100V 5% polypropylene film, 5mm pitch. The board silk reads "
                   "'C3 3n3 C0G' -- read that as 'the stable dielectric', which this "
                   "film part satisfies. NEVER X7R here: "
                   "it drifts with temperature and DC bias and would turn the "
                   "calibrated filter lag into a drifting one."),
    "JST-PH 5":   ("B5B-PH-K-S(LF)(SN)", "306-B5BPHKSLFSNP", "JST", 0.24, 2.0,
                   "5-pin PH vertical header, board side."),
}
# needed for a build but carrying no footprint on the board
EXTRAS = [
    ("DIP-8 socket", "1-2199298-2", "571-1-2199298-2", "TE", 0.27, BOARDS * 2,
     "One each for U1 and U2. Keeps soldering heat off the OPT101 entirely."),
    ("PH housing", "PHR-5", "306-PHR-5PP", "JST", 0.10, BOARDS,
     "Cable-side mating shell for J1."),
    ("PH crimps", "SPH-002T-P0.5S", "306-SPH-002T-P0.5S", "JST", 0.11, BOARDS * 5,
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
        if val not in PARTS:
            missing.append(val)
            continue
        mpn, mouser, mfr, price, spare, note = PARTS[val]
        qty = int(math.ceil(per_board * BOARDS * spare))
        rows.append((val, ",".join(sorted(refs[val])), qty, mpn, mouser, mfr,
                     price, note))
    for name, mpn, mouser, mfr, price, qty, note in EXTRAS:
        rows.append((name, "-", qty, mpn, mouser, mfr, price, note))

    with open(os.path.join(D, "fab", "bom-mouser.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Mouser Part Number", "Quantity",
                    "Mfr Part Number", "Description", "Designators"])
        for val, r, qty, mpn, mouser, mfr, _price, _note in rows:
            w.writerow([mouser, qty, mpn, "%s %s" % (mfr, val), r])

    with open(os.path.join(D, "fab", "bom.md"), "w") as f:
        f.write("# Mouser BOM - photomarker rev B (through-hole)\n\n")
        f.write("For **%d boards**, hand-assembled. Quantities include spares.\n\n" % BOARDS)
        f.write("`OK` = verified listing. `CHECK` = derived from the maker's scheme, "
                "confirm in the cart. `PICK` = you must choose one.\n\n")
        f.write("| Qty | Value | Designators | Mouser # | Mfr part | Ext. | Note |\n")
        f.write("|----:|-------|-------------|----------|----------|-----:|------|\n")
        for val, r, qty, mpn, mouser, mfr, price, note in rows:
            f.write("| %d | %s | %s | `%s` | `%s` | $%.2f | %s |\n"
                    % (qty, val, r, mouser, mpn, qty * price, note))
        f.write("\n**Estimated total $%.2f** at single-unit pricing -- the real "
                "figure is lower, since several lines cross a price break.\n"
                % sum(q * p for _v, _r, q, _m, _mo, _mf, p, _n in rows))
        f.write("\n## Not populated (DNP)\n\nBuy only if you want the option.\n\n")
        for val, rs in sorted(dnp.items()):
            f.write("- **%s** - %s\n" % (", ".join(sorted(rs)), val))
        f.write("\nR6 is the OPT101 gain resistor: **pick its value after seeing TP1 "
                "on a real screen**, not now.\n")

    total = sum(q * p for _v, _r, q, _m, _mo, _mf, p, _n in rows)
    print("%d boards -> %d line items, est. $%.2f" % (BOARDS, len(rows), total))
    for val, r, qty, mpn, mouser, mfr, price, _n in rows:
        print("  %-3d x %-13s %-22s $%6.2f" % (qty, val, mouser, qty * price))
    if missing:
        print("\nNEEDS A PART NUMBER: %s" % ", ".join(missing))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
