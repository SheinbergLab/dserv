#!/usr/bin/env python3
"""
Emit the JLCPCB CPL + BOM from the board, with LCSC part numbers filled in.

DNP parts and hand-soldered THT parts are excluded, so JLCPCB places only what
it should. Any part still missing an LCSC number is written as PENDING and
reported loudly -- never guess a C-number, it orders the wrong component.

  .../Python.framework/Versions/3.9/bin/python3 make_jlc.py
"""
import csv
import os
import sys

import pcbnew

D = os.path.dirname(os.path.abspath(__file__))

# value -> (LCSC, MPN, Basic/Extended).  Verified on JLCPCB 2026-08-28.
LCSC = {
    "100n":   ("C49678",  "CC0805KRX7R9BB104",  "Basic"),
    "10k":    ("C17414",  "0805W8F1002T5E",     "Basic"),
    "100R":   ("C17408",  "0805W8F1000T5E",     "Basic"),
    "2k2":    ("C17520",  "0805W8F2201T5E",     "Basic"),
    "39k":    ("C25826",  "0805W8F3902T5E",     "Extended"),
    "270k":   ("C17589",  "0805W8F2703T5E",     "Extended"),
    # --- still needed -------------------------------------------------------
    # C7467177 (FOJAN FCC0805B332K500DT) is X7R despite the "C0G" in our comment
    # field -- rejected. Need a listing whose description says C0G or NP0.
    "3n3 C0G": ("PENDING", "must be C0G/NP0; C7467177 is X7R - rejected", "?"),
    # C51912398 (GOODWORK) matches but is Extended with only 5 in stock for a
    # 5-board order, and an unknown maker for a part whose Vos sets our timing
    # spread. Prefer a Basic, well-stocked TI/onsemi LM393.
    "LM393":  ("PENDING", "want Basic + stock >> 5, TI or onsemi", "?"),
}


def main():
    board = pcbnew.LoadBoard(os.path.join(D, "photomarker.kicad_pcb"))
    dnp, tht = set(), set()
    for fp in board.Footprints():
        r = fp.GetReference()
        if fp.IsDNP():
            dnp.add(r)
        if fp.Pads() and all(p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH for p in fp.Pads()):
            tht.add(r)

    rows = list(csv.DictReader(open(os.path.join(D, "fab", "pos-raw.csv"))))
    place = [r for r in rows if r["Ref"] not in dnp and r["Ref"] not in tht
             and not r["Ref"].startswith(("TP", "H"))]

    with open(os.path.join(D, "fab", "photomarker-cpl.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for r in sorted(place, key=lambda r: r["Ref"]):
            w.writerow([r["Ref"], "%.4f" % float(r["PosX"]), "%.4f" % float(r["PosY"]),
                        r["Side"].capitalize(), "%.1f" % float(r["Rot"])])

    grp = {}
    for r in place:
        grp.setdefault(r["Val"], []).append(r["Ref"])

    missing = []
    with open(os.path.join(D, "fab", "photomarker-bom.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
        for val, refs in sorted(grp.items()):
            lcsc, mpn, tier = LCSC.get(val, ("PENDING", "not searched", "?"))
            if lcsc == "PENDING":
                missing.append((val, sorted(refs), mpn))
            pkg = next(r["Package"] for r in place if r["Val"] == val)
            w.writerow([val, ",".join(sorted(refs)), pkg, lcsc])

    print("placed %d parts in %d line items" % (len(place), len(grp)))
    for val, refs, mpn in missing:
        print("  PENDING  %-9s %-14s %s" % (val, ",".join(refs), mpn))
    if missing:
        print("\nBOM is NOT ready to upload -- %d line item(s) need an LCSC number."
              % len(missing))
        return 1
    print("\nBOM complete.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
