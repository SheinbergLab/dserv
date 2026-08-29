#!/usr/bin/env python3
"""
Verify photomarker.kicad_sch and photomarker.kicad_pcb agree.

The schematic is generated FROM the board, but that generation can still be
stale -- this is the check that catches it. Compares nets, refs, values,
footprints and DNP flags. Run before ordering, after any regeneration.

  .../Python.framework/Versions/3.9/bin/python3 check_netlist.py
Exit status is 0 on agreement, 1 on any mismatch.
"""
import collections
import os
import re
import subprocess
import sys

import pcbnew

D = os.path.dirname(os.path.abspath(__file__))
CLI = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
NET = os.path.join(D, "sch.net")


def blk(t, i):
    d = 0
    for j in range(i, len(t)):
        if t[j] == '(':
            d += 1
        elif t[j] == ')':
            d -= 1
            if d == 0:
                return t[i:j + 1]
    raise ValueError("unbalanced s-expression")


def main():
    subprocess.run([CLI, "sch", "export", "netlist", "--format", "kicadsexpr",
                    "-o", NET, os.path.join(D, "photomarker.kicad_sch")],
                   check=True, capture_output=True)
    s = open(NET).read()

    sch = collections.defaultdict(set)
    nb = blk(s, s.index("(nets"))
    for m in re.finditer(r'\(net\b', nb):
        b = blk(nb, m.start())
        nm = re.search(r'\(name "([^"]+)"\)', b)
        if not nm:
            continue
        for ref, pn in re.findall(r'\(ref "([^"]+)"\)\s*\(pin "([^"]+)"\)', b):
            if not ref.startswith(("#PWR", "#FLG")):
                sch[nm.group(1).lstrip("/")].add((ref, pn))

    scomp, sdnp = {}, set()
    cb = blk(s, s.index("(components"))
    for m in re.finditer(r'\(comp\b', cb):
        b = blk(cb, m.start())
        ref = re.search(r'\(ref "([^"]+)"\)', b).group(1)
        if ref.startswith(("#PWR", "#FLG")):
            continue
        v = re.search(r'\(value "([^"]*)"\)', b)
        f = re.search(r'\(footprint "([^"]*)"\)', b)
        scomp[ref] = (v.group(1) if v else "", (f.group(1) if f else "").split(":")[-1])
        if re.search(r'\(property\s*\(name "dnp"\)\s*\)', b):
            sdnp.add(ref)

    board = pcbnew.LoadBoard(os.path.join(D, "photomarker.kicad_pcb"))
    pcb, pcomp, pdnp = collections.defaultdict(set), {}, set()
    for fp in board.Footprints():
        r = fp.GetReference()
        pcomp[r] = (fp.GetValue(),
                    str(fp.GetFPID().GetUniStringLibId()).split(":")[-1])
        if fp.IsDNP():
            pdnp.add(r)
        for p in fp.Pads():
            if p.GetNetname():
                pcb[p.GetNetname()].add((r, p.GetNumber()))

    real = {k: v for k, v in sch.items() if not k.startswith("unconnected-")}
    bad = []
    for n in sorted(set(real) | set(pcb)):
        if real.get(n, set()) != pcb.get(n, set()):
            bad.append("net %s: sch-only=%s pcb-only=%s"
                       % (n, sorted(real.get(n, set()) - pcb.get(n, set())),
                          sorted(pcb.get(n, set()) - real.get(n, set()))))
    if set(scomp) != set(pcomp):
        bad.append("refs differ: %s" % (set(scomp) ^ set(pcomp)))
    for r in sorted(set(scomp) & set(pcomp)):
        if scomp[r] != pcomp[r]:
            bad.append("%s: sch=%r pcb=%r" % (r, scomp[r], pcomp[r]))
    if sdnp != pdnp:
        bad.append("dnp differs: sch-only=%s pcb-only=%s"
                   % (sorted(sdnp - pdnp), sorted(pdnp - sdnp)))

    os.remove(NET)
    print("nets %d  refs %d  dnp %d" % (len(pcb), len(pcomp), len(pdnp)))
    if bad:
        print("\n".join("  MISMATCH " + b for b in bad))
        print("\n*** MISMATCH ***")
        return 1
    print("*** schematic and board agree: nets, refs, values, footprints, DNP ***")
    return 0


if __name__ == "__main__":
    sys.exit(main())
