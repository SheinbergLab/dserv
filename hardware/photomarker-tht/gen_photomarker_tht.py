#!/usr/bin/env python3
"""
Stimulus onset photo-marker, rev B -- ALL THROUGH-HOLE.

A deliberately student-buildable version of the rev A SMD board:
  * OPT101 and LM393 both in DIP-8 (socketable)
  * axial resistors, disc caps, 3mm LED
  * RV1 25-turn trimpot sets the onset threshold, measured at TP3

Everything is THT, so U1 mounts from the TOP (its window faces the screen) and
every other body sits on the BOTTOM, away from the screen.

Run with KiCad's bundled python:
  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 gen_photomarker_tht.py
"""
import os
import sys

import pcbnew

FPBASE = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "photomarker-tht.kicad_pcb")

BX, BY = 100.0, 100.0
BW, BH = 75.0, 55.0
DIE_X, DIE_Y = 13.81, 19.81      # OPT101 die = DIP-8 package centre
REV = "B"


def mm(v):
    return pcbnew.FromMM(v)


def V(x, y):
    return pcbnew.VECTOR2I(mm(BX + x), mm(BY + y))


# ---------------------------------------------------------------- components
# Every THT footprint here has pin 1 at its origin, so (x, y) IS pin 1.
# (ref, value, lib, footprint, x, y, rot, bottom?, dnp?)
R_AX = ("Resistor_THT", "R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal")
C_DI = ("Capacitor_THT", "C_Disc_D5.0mm_W2.5mm_P5.00mm")

COMPONENTS = [
    # --- sensor, mounted from the TOP so the window faces the screen
    ("U1", "OPT101", "Package_DIP", "DIP-8_W7.62mm",      10.0, 16.0, 0, False, False),
    ("C1", "100n",   C_DI[0], C_DI[1],                    15.0, 10.0, 0, True,  False),
    ("R6", "DNP",    R_AX[0], R_AX[1],                    17.62, 28.0, 0, True,  True),

    # --- filter
    ("TP1", "OPT_OUT", "TestPoint", "TestPoint_THTPad_D1.5mm_Drill0.7mm", 21.0, 16.0, 0, False, False),
    ("R4", "10k",    R_AX[0], R_AX[1],                    28.62, 22.0, 0, True,  False),
    ("C3", "3n3 C0G", C_DI[0], C_DI[1],                   26.0, 28.0, 0, True,  False),
    ("TP2", "FILT",  "TestPoint", "TestPoint_THTPad_D1.5mm_Drill0.7mm", 21.0, 34.0, 0, False, False),
    ("R11", "10k",   R_AX[0], R_AX[1],                    28.62, 40.0, 0, True,  False),
    ("R12", "DNP",   R_AX[0], R_AX[1],                    21.0, 46.0, 180, True,  True),

    # --- comparator
    ("U2", "LM393",  "Package_DIP", "DIP-8_W7.62mm",      41.62, 16.0, 0, True,  False),
    ("C2", "100n",   C_DI[0], C_DI[1],                    39.0, 10.0, 0, True,  False),
    ("R13", "DNP",   R_AX[0], R_AX[1],                    41.62, 28.0, 0, True,  True),
    ("R14", "DNP",   R_AX[0], R_AX[1],                    41.62, 34.0, 0, True,  True),
    ("R15", "DNP",   R_AX[0], R_AX[1],                    41.62, 40.0, 0, True,  True),

    # --- threshold + hysteresis
    ("R1", "10k",    R_AX[0], R_AX[1],                    53.62, 10.0, 0, True,  False),
    ("R3", "270k",   R_AX[0], R_AX[1],                    53.62, 16.0, 0, True,  False),
    ("R2", "10k",    R_AX[0], R_AX[1],                    53.62, 22.0, 0, True,  False),
    ("R5", "100R",   R_AX[0], R_AX[1],                    53.62, 28.0, 0, True,  False),
    ("R7", "2k2",    R_AX[0], R_AX[1],                    53.62, 34.0, 0, True,  False),
    ("RV1", "50k",   "Potentiometer_THT", "Potentiometer_Bourns_3296W_Vertical",
                                                          62.0, 16.0, 0, True,  False),

    # --- connectors, hand-soldered from the bottom
    ("J1", "JST-PH 5", "Connector_JST",
     "JST_PH_B5B-PH-K_1x05_P2.00mm_Vertical",             50.0, 46.0, 180, True,  False),
    ("J2", "DNP", "Connector_PinHeader_2.54mm",
     "PinHeader_1x05_P2.54mm_Vertical",                    50.0, 40.0, 270, True,  True),

    # --- test points + mounting
    ("TP3", "THRESH",  "TestPoint", "TestPoint_THTPad_D1.5mm_Drill0.7mm", 56.0, 24.0, 0, False, False),
    ("TP4", "CMP_OUT", "TestPoint", "TestPoint_THTPad_D1.5mm_Drill0.7mm", 56.0, 30.0, 0, False, False),
    ("TP5", "GND",     "TestPoint", "TestPoint_THTPad_D1.5mm_Drill0.7mm", 56.0, 36.0, 0, False, False),
    ("H1", "M3", "MountingHole", "MountingHole_3.2mm_M3_Pad",  6.0,  6.0, 0, False, False),
    ("H2", "M3", "MountingHole", "MountingHole_3.2mm_M3_Pad", 69.0,  6.0, 0, False, False),
    ("H3", "M3", "MountingHole", "MountingHole_3.2mm_M3_Pad",  6.0, 49.0, 0, False, False),
    ("H4", "M3", "MountingHole", "MountingHole_3.2mm_M3_Pad", 69.0, 49.0, 0, False, False),
]

# ---------------------------------------------------------------------- nets
# OPT101 : 1=Vs 2=-In 3=-V 4=1M 5=OUT 6=NC 7=NC 8=COM
# LM393  : 1=OUTA 2=INA- 3=INA+ 4=GND 5=INB+ 6=INB- 7=OUTB 8=VCC
# RV1    : 1=end (from R1)  2=wiper (to THRESH_A)  3=other end, LEFT OPEN
#          Wiper+one end is the standard rheostat; tying pin 3 anywhere either
#          halves the range or makes it non-monotonic.
# J1     : 1=+3V3 2=GND 3=AOUT 4=DI-B 5=DI-A
NETS = {
    "GND": [("U1", "3"), ("U1", "8"), ("U2", "4"), ("C1", "2"), ("C2", "2"),
            ("C3", "2"), ("R2", "2"), ("R12", "2"), ("J1", "2"), ("J2", "2"),
            ("TP5", "1"), ("H1", "1"), ("H2", "1"), ("H3", "1"), ("H4", "1")],
    "+3V3": [("U1", "1"), ("U2", "8"), ("C1", "1"), ("C2", "1"), ("R1", "1"),
             ("R7", "1"), ("R11", "1"), ("R14", "1"),
             ("J1", "1"), ("J2", "1")],
    "OPT_OUT":  [("U1", "4"), ("U1", "5"), ("R4", "1"), ("R6", "1"), ("TP1", "1")],
    "OPT_INN":  [("U1", "2"), ("R6", "2")],
    "FILT":     [("R4", "2"), ("C3", "1"), ("U2", "2"), ("U2", "6"),
                 ("J1", "3"), ("J2", "3"), ("TP2", "1")],
    "RV_A":     [("R1", "2"), ("RV1", "1")],
    "THRESH_A": [("RV1", "2"), ("R2", "1"), ("R3", "1"), ("U2", "3"), ("TP3", "1")],
    "THRESH_B": [("R11", "2"), ("R12", "1"), ("R13", "1"), ("U2", "5")],
    "CMPA_OUT": [("U2", "1"), ("R3", "2"), ("R5", "1"), ("R7", "2"),
                 ("TP4", "1")],
    "CMPB_OUT": [("U2", "7"), ("R13", "2"), ("R15", "1"), ("R14", "2")],
    "DI_A":     [("R5", "2"), ("J1", "5"), ("J2", "5")],
    "DI_B":     [("R15", "2"), ("J1", "4"), ("J2", "4")],
}

# ------------------------------------------------------------------- routing
# All parts are THT, so BOTH layers are free apart from pad holes.
# "B" = B.Cu (component side), "F" = F.Cu.  GND is handled by pours.
W_SIG, W_PWR = 0.25, 0.40
PWR_NETS = ("+3V3", "GND")
ROUTES = [
    ("OPT_INN",  "B", [(10,28),(7.5,26),(7.5,17.5),(10,18.54)]),

    ("OPT_OUT",  "B", [(10,23.62),(19,23.62)]),
    ("OPT_OUT",  "B", [(17.62,23.62),(17.62,28)]),
    ("OPT_OUT",  "B", [(19,23.62),(20,20),(27,20),(28.62,22)]),
    ("OPT_OUT",  "B", [(21,16),(19,18),(19,23.62)]),

    ("FILT",     "B", [(21,22),(23.5,24),(23.5,32),(21,34)]),
    ("FILT",     "B", [(23.5,26),(26,28)]),
    ("FILT",     "B", [(23.5,24),(30,24),(32,22.5),(34,21.08)]),
    ("FILT",     "B", [(34,21.08),(36,19.8),(39.6,19.8),(41.62,18.54)]),
    ("FILT",     "B", [(23.5,32),(22,34.5),(19,36),(19,49),(54,49),(54,46)]),
    ("FILT",     "B", [(54,46),(55.08,40)]),

    ("RV_A",     "B", [(46,10),(46,12.5),(60,12.5),(62,16)]),

    ("THRESH_A", "B", [(64.54,16),(64.54,19.5),(55,19.5),(53.62,16)]),
    ("THRESH_A", "B", [(53.62,16),(53.62,22)]),
    ("THRESH_A", "B", [(56,24),(53.62,22)]),
    ("THRESH_A", "F", [(41.62,21.08),(44.5,22.5),(44.5,25),(50,25),(53.62,22)]),

    ("CMPA_OUT", "B", [(41.62,16),(46,16)]),
    ("CMPA_OUT", "B", [(46,16),(43,17.5),(43,33),(46,34)]),
    ("CMPA_OUT", "B", [(53.62,28),(53.62,31),(43,31)]),
    ("CMPA_OUT", "B", [(56,30),(53.62,28)]),

    ("CMPB_OUT", "F", [(34,18.54),(30.5,19.5),(30.5,26.5),(34,28)]),
    ("CMPB_OUT", "B", [(34,28),(34,34)]),
    ("CMPB_OUT", "B", [(34,34),(34,37),(43.5,37),(41.62,40)]),

    ("THRESH_B", "B", [(21,40),(21,46)]),
    ("THRESH_B", "F", [(21,46),(23,48),(32.2,48),(32.2,30),(41.62,26),(41.62,28)]),
    ("THRESH_B", "B", [(34,23.62),(36,25),(40,25),(41.62,28)]),

    ("DI_A",     "F", [(46,28),(48,30),(48,31.8),(62,31.8),(62,44),(58,46)]),
    ("DI_A",     "B", [(58,46),(60.16,40)]),

    ("DI_B",     "F", [(34,40),(35,42),(35,51),(56,51),(56,46)]),
    ("DI_B",     "B", [(56,46),(57.62,40)]),


    ("+3V3",     "B", [(15,10),(15,6),(53.62,6),(53.62,10)]),
    ("+3V3",     "B", [(39,6),(39,10)]),
    ("+3V3",     "B", [(53.62,10),(58,9),(70,13),(70,36),(58,34),(53.62,34)]),
    ("+3V3",     "B", [(10,16),(10,13),(13,10),(15,10)]),
    ("+3V3",     "B", [(34,16),(36,14),(36,10),(39,10)]),
    ("+3V3",     "B", [(53.62,34),(52,36),(52,38),(50,40)]),
    ("+3V3",     "B", [(41.62,34),(44,36),(47,38),(50,40)]),
    ("+3V3",     "B", [(28.62,40),(31,43),(47,43),(50,40)]),
    ("+3V3",     "B", [(50,46),(50,40)]),

    ("GND",      "B", [(52,46),(52.54,40)]),
]


def main():
    board = pcbnew.NewBoard(OUT)

    placed = {}
    for ref, val, lib, fpname, x, y, rot, bottom, dnp in COMPONENTS:
        fp = pcbnew.FootprintLoad(os.path.join(FPBASE, lib + ".pretty"), fpname)
        if fp is None:
            sys.exit("FAILED to load %s:%s" % (lib, fpname))
        fp.SetPosition(V(x, y))
        if rot:
            fp.SetOrientationDegrees(rot)
        board.Add(fp)                 # Add() BEFORE Flip() -- see rev A notes
        if bottom:
            fp.Flip(fp.GetPosition(), False)
        fp.SetReference(ref)
        fp.SetValue(val)
        if dnp and hasattr(fp, "SetDNP"):
            fp.SetDNP(True)
        placed[ref] = fp

    netmap = {}
    for name in NETS:
        n = pcbnew.NETINFO_ITEM(board, name)
        board.Add(n)
        netmap[name] = n
    for name, conns in NETS.items():
        for ref, padnum in conns:
            pad = placed[ref].FindPadByNumber(padnum)
            if pad is None:
                sys.exit("%s has no pad %s (net %s)" % (ref, padnum, name))
            pad.SetNet(netmap[name])

    corners = [(0, 0), (BW, 0), (BW, BH), (0, BH)]
    for i in range(4):
        s = pcbnew.PCB_SHAPE(board)
        s.SetShape(pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(V(*corners[i]))
        s.SetEnd(V(*corners[(i + 1) % 4]))
        s.SetLayer(pcbnew.Edge_Cuts)
        s.SetWidth(mm(0.1))
        board.Add(s)

    for layer in (pcbnew.F_SilkS, pcbnew.B_SilkS):
        for (x1, y1, x2, y2) in ((DIE_X - 2.2, DIE_Y, DIE_X - 1.0, DIE_Y),
                                 (DIE_X + 1.0, DIE_Y, DIE_X + 2.2, DIE_Y),
                                 (DIE_X, DIE_Y - 2.2, DIE_X, DIE_Y - 1.0),
                                 (DIE_X, DIE_Y + 1.0, DIE_X, DIE_Y + 2.2)):
            s = pcbnew.PCB_SHAPE(board)
            s.SetShape(pcbnew.SHAPE_T_SEGMENT)
            s.SetStart(V(x1, y1))
            s.SetEnd(V(x2, y2))
            s.SetLayer(layer)
            s.SetWidth(mm(0.15))
            board.Add(s)

    def text(t, x, y, layer, size=1.0, mirror=False):
        o = pcbnew.PCB_TEXT(board)
        o.SetText(t)
        o.SetPosition(V(x, y))
        o.SetLayer(layer)
        o.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
        o.SetTextThickness(mm(size / 6.0))
        if mirror:
            o.SetMirrored(True)
        board.Add(o)

    text("SHEINBERG LAB  -  PHOTOMARKER", 37.5, 2.2, pcbnew.F_SilkS, 1.2)
    text("stimulus onset    rev %s    all through-hole" % REV, 37.5, 4.8, pcbnew.F_SilkS, 0.9)
    text("THIS SIDE FACES THE SCREEN", 37.5, 53.0, pcbnew.F_SilkS, 1.1)
    text("RV1 sets threshold - measure at TP3", 37.5, 2.2, pcbnew.B_SilkS, 0.9, mirror=True)
    text("J1  1=3V3 2=GND 3=AOUT 4=DIb 5=DIa", 37.5, 53.0, pcbnew.B_SilkS, 0.9, mirror=True)

    # Reference designators live on Fab (they auto-place badly at this density).
    # The silkscreen instead gets a deliberate "R1 10k" label per part -- on a
    # hand-built board you want to know what goes in the hole without a BOM.
    for fp in board.Footprints():
        r = fp.Reference()
        r.SetLayer(pcbnew.B_Fab if fp.GetLayer() == pcbnew.B_Cu else pcbnew.F_Fab)
        r.SetTextSize(pcbnew.VECTOR2I(mm(0.8), mm(0.8)))
        r.SetTextThickness(mm(0.13))
        fp.Value().SetVisible(False)

    LABEL_BELOW = {"J1"}                    # no room above these
    LABEL_AT = {                            # explicit placement where it is tight
        "J2":  (45.5, 37.0),
        "R13": (30.0, 28.0),                # U2's DIP outline fills the gap above
        "TP1": (27.5, 16.3),                # would collide with U1's label
        "TP3": (62.0, 24.3),                # above would land on the R2/R5/R7 pads
        "TP4": (62.0, 30.3),
        "TP5": (62.0, 36.3),
    }
    for fp in board.Footprints():
        ref = fp.GetReference()
        if ref.startswith("H"):
            continue
        back = fp.GetLayer() == pcbnew.B_Cu
        cy = fp.GetCourtyard(pcbnew.B_CrtYd if back else pcbnew.F_CrtYd).BBox()
        if not cy.GetWidth():
            cy = fp.GetCourtyard(pcbnew.F_CrtYd if back else pcbnew.B_CrtYd).BBox()
        cx = pcbnew.ToMM((cy.GetLeft() + cy.GetRight()) / 2) - BX
        if ref in LABEL_AT:
            lx, ly = LABEL_AT[ref]
        elif ref in LABEL_BELOW:
            lx, ly = cx, pcbnew.ToMM(cy.GetBottom()) - BY + 1.45
        else:
            lx, ly = cx, pcbnew.ToMM(cy.GetTop()) - BY - 1.45
        val = fp.GetValue()
        txt = ref if val in ("", ref) else "%s %s" % (ref, val)
        text(txt, lx, ly, pcbnew.B_SilkS if back else pcbnew.F_SilkS, 0.9, mirror=back)


    # -------------------------------------------------------------- routing
    nets_by_name = {n.GetNetname(): n for n in board.GetNetInfo().NetsByName().values()}
    nt = 0
    for net, layer, pts in ROUTES:
        w = W_PWR if net in PWR_NETS else W_SIG
        for i in range(len(pts) - 1):
            t = pcbnew.PCB_TRACK(board)
            t.SetStart(V(*pts[i])); t.SetEnd(V(*pts[i + 1]))
            t.SetWidth(mm(w))
            t.SetLayer(pcbnew.B_Cu if layer == "B" else pcbnew.F_Cu)
            t.SetNet(nets_by_name[net])
            board.Add(t); nt += 1

    gnd = nets_by_name["GND"]
    for lyr in (pcbnew.F_Cu, pcbnew.B_Cu):
        z = pcbnew.ZONE(board)
        z.SetLayer(lyr); z.SetNet(gnd)
        z.SetLocalClearance(mm(0.3)); z.SetMinThickness(mm(0.2))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)
        pts = pcbnew.VECTOR_VECTOR2I()
        for (px, py) in ((0.5, 0.5), (BW - 0.5, 0.5), (BW - 0.5, BH - 0.5), (0.5, BH - 0.5)):
            pts.append(V(px, py))
        z.AddPolygon(pts)
        board.Add(z)
    try:
        pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    except Exception as e:
        print("zone fill skipped: %s" % e)
    print("tracks: %d" % nt)

    board.GetDesignSettings().SetAuxOrigin(V(0, BH))
    board.Save(OUT)
    print("wrote %s" % OUT)

    print("\n--- pad positions (board-local mm) ---")
    for ref in sorted(placed):
        fp = placed[ref]
        ps = sorted(fp.Pads(), key=lambda p: p.GetNumber())
        print("  %-4s %s" % (ref, "  ".join(
            "%s:(%.2f,%.2f)%s" % (p.GetNumber(),
                                  pcbnew.ToMM(p.GetPosition().x) - BX,
                                  pcbnew.ToMM(p.GetPosition().y) - BY,
                                  "=" + p.GetNetname() if p.GetNetname() else "")
            for p in ps)))


if __name__ == "__main__":
    main()
