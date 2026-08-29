#!/usr/bin/env python3
"""
Generate the LCD onset photo-marker PCB with the KiCad 10 pcbnew API.

Sensor:      OPT101 (DIP-8, THT, mounted from the TOP so its window faces the screen)
Comparator:  SOIC-8 -- accepts TLV3202 (recommended) or LM393 (pin-identical fallback)
Assembly:    all SMD on the BOTTOM layer -> single-sided SMT at JLCPCB, and the board
             itself shields the indicator LED from the photodiode.

Run with KiCad's bundled python:
  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 gen_photomarker.py
"""
import os
import sys
import pcbnew

FPBASE = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "photomarker.kicad_pcb")

# Board origin on the sheet, and board size (mm)
BX, BY = 100.0, 100.0
BW, BH = 50.0, 34.0
DIE_X, DIE_Y = 14.0, 17.0          # OPT101 photodiode centre, board-local
REV = "A"                          # bump on any board change

def mm(v):
    return pcbnew.FromMM(v)

def V(x, y):
    """Board-local mm -> absolute VECTOR2I."""
    return pcbnew.VECTOR2I(mm(BX + x), mm(BY + y))

# ---------------------------------------------------------------- components
# (ref, value, lib, footprint, x, y, rot, bottom?, dnp?)
COMPONENTS = [
    # --- sensor: THT, body on TOP facing the screen. Origin is pin 1; the die
    #     sits at the package centre, +3.81/+3.81 from it -> (DIE_X, DIE_Y).
    ("U1", "OPT101",  "Package_DIP",   "DIP-8_W7.62mm",
     DIE_X - 3.81, DIE_Y - 3.81,   0, False, False),

    # --- comparator: SOIC-8, bottom. TLV3202 (recommended) or LM393.
    #     Flipped, pins 1-4 (A side) land at x+2.475, pins 5-8 (B side) at x-2.475.
    ("U2", "LM393",   "Package_SO",    "SOIC-8_3.9x4.9mm_P1.27mm",   34.00, 17.00,   0, True,  False),

    # --- signal chain: U1 OUT -> R4 -> C3 -> comparator inputs
    ("R4", "10k",     "Resistor_SMD",  "R_0805_2012Metric",          24.00, 21.00, 180, True,  False),
    ("C3", "3n3 C0G", "Capacitor_SMD", "C_0805_2012Metric",          27.50, 21.00, 180, True,  False),

    # --- threshold + hysteresis, comparator A: RIGHT of U2, by its pins 1-3
    ("R1", "39k",     "Resistor_SMD",  "R_0805_2012Metric",          40.00, 10.00, 180, True,  False),
    ("R8", "DNP",     "Resistor_SMD",  "R_0805_2012Metric",          44.00, 10.00,   0, True,  True),
    ("R3", "270k",    "Resistor_SMD",  "R_0805_2012Metric",          40.00, 12.50,   0, True,  False),
    ("R7", "2k2",     "Resistor_SMD",  "R_0805_2012Metric",          44.00, 12.50,   0, True,  False),
    ("R2", "10k",     "Resistor_SMD",  "R_0805_2012Metric",          40.00, 22.00,   0, True,  False),
    ("R5", "100R",    "Resistor_SMD",  "R_0805_2012Metric",          40.00, 25.00,   0, True,  False),

    # --- threshold network, comparator B: LEFT of U2, by its pins 5-7 (all DNP)
    ("R11", "10k",    "Resistor_SMD",  "R_0805_2012Metric",          25.00,  9.00,   0, True,  False),
    ("R13", "DNP",    "Resistor_SMD",  "R_0805_2012Metric",          28.50, 12.00, 180, True,  True),
    ("R12", "DNP",    "Resistor_SMD",  "R_0805_2012Metric",          25.00, 12.00,   0, True,  True),
    ("R14", "2k2",    "Resistor_SMD",  "R_0805_2012Metric",          28.50,  9.00,   0, True,  False),
    ("R15", "DNP",    "Resistor_SMD",  "R_0805_2012Metric",          34.50, 22.00, 180, True,  True),

    # --- OPT101 external gain resistor (DNP): drop gain if you saturate
    ("R6", "DNP",     "Resistor_SMD",  "R_0805_2012Metric",           7.50, 17.00,  90, True,  True),

    # --- decoupling, each at its own chip's supply pin
    ("C1", "100n",    "Capacitor_SMD", "C_0805_2012Metric",          10.00, 10.00,   0, True,  False),
    ("C2", "100n",    "Capacitor_SMD", "C_0805_2012Metric",          33.00, 11.00, 180, True,  False),

    # --- bring-up LED, on the bottom so the board blocks its light (DNP)
    ("R9", "DNP 1k",  "Resistor_SMD",  "R_0805_2012Metric",          34.00,  4.00, 180, True,  True),
    ("D1", "DNP LED", "LED_SMD",       "LED_0805_2012Metric",        37.50,  4.00,   0, True,  True),

    # --- connectors (THT, hand-soldered alongside the OPT101)
    ("J1", "JST-PH 5", "Connector_JST",
     "JST_PH_B5B-PH-K_1x05_P2.00mm_Vertical",                        12.00, 31.00, 180, True,  False),
    ("J2", "DNP 1x05", "Connector_PinHeader_2.54mm",
     "PinHeader_1x05_P2.54mm_Vertical",                              12.00, 25.50, 270, True,  True),

    # --- test points
    ("TP1", "OPT_OUT", "TestPoint", "TestPoint_THTPad_D1.5mm_Drill0.7mm", 21.50, 14.50, 0, False, False),
    ("TP2", "FILT",    "TestPoint", "TestPoint_THTPad_D1.5mm_Drill0.7mm", 22.50, 18.50, 0, False, False),
    ("TP3", "THRESH",  "TestPoint", "TestPoint_THTPad_D1.5mm_Drill0.7mm", 46.50, 22.50, 0, False, False),
    ("TP4", "CMP_OUT", "TestPoint", "TestPoint_THTPad_D1.5mm_Drill0.7mm", 46.50, 20.00, 0, False, False),
    ("TP5", "GND",     "TestPoint", "TestPoint_THTPad_D1.5mm_Drill0.7mm", 21.50, 11.00, 0, False, False),

    # --- mounting: M3 on a 40 x 24 mm pattern
    ("H1", "M3", "MountingHole", "MountingHole_3.2mm_M3_Pad",  5.00,  5.00, 0, False, False),
    ("H2", "M3", "MountingHole", "MountingHole_3.2mm_M3_Pad", 45.00,  5.00, 0, False, False),
    ("H3", "M3", "MountingHole", "MountingHole_3.2mm_M3_Pad",  5.00, 29.00, 0, False, False),
    ("H4", "M3", "MountingHole", "MountingHole_3.2mm_M3_Pad", 45.00, 29.00, 0, False, False),
]

# ---------------------------------------------------------------------- nets
# OPT101 : 1=Vs 2=-In 3=-V 4=1M 5=OUT 6=NC 7=NC 8=COM
# SOIC-8 : 1=OUTA 2=INA- 3=INA+ 4=GND 5=INB+ 6=INB- 7=OUTB 8=VCC  (TLV3202 == LM393)
# J1     : 1=+3V3 2=GND 3=AOUT 4=DI1 5=DI2
NETS = {
    "GND": [("U1", "3"), ("U1", "8"), ("U2", "4"), ("C1", "2"), ("C2", "2"),
            ("C3", "2"), ("R2", "2"), ("R12", "2"), ("J1", "2"), ("J2", "2"),
            ("TP5", "1"), ("H1", "1"), ("H2", "1"), ("H3", "1"), ("H4", "1")],
    "+3V3": [("U1", "1"), ("U2", "8"), ("C1", "1"), ("C2", "1"), ("R1", "1"),
             ("R8", "1"), ("R7", "1"), ("R9", "1"), ("R11", "1"), ("R14", "1"),
             ("J1", "1"), ("J2", "1")],
    "OPT_OUT":  [("U1", "4"), ("U1", "5"), ("R4", "1"), ("R6", "1"), ("TP1", "1")],
    "OPT_INN":  [("U1", "2"), ("R6", "2")],
    "FILT":     [("R4", "2"), ("C3", "1"), ("U2", "2"), ("U2", "6"),
                 ("J1", "3"), ("J2", "3"), ("TP2", "1")],
    "THRESH_A": [("R1", "2"), ("R8", "2"), ("R2", "1"), ("R3", "1"),
                 ("U2", "3"), ("TP3", "1")],
    "THRESH_B": [("R11", "2"), ("R12", "1"), ("R13", "1"), ("U2", "5")],
    "CMPA_OUT": [("U2", "1"), ("R3", "2"), ("R5", "1"), ("R7", "2"),
                 ("D1", "1"), ("TP4", "1")],
    "CMPB_OUT": [("U2", "7"), ("R13", "2"), ("R15", "1"), ("R14", "2")],
    "DI1":      [("R5", "2"), ("J1", "5"), ("J2", "5")],
    "DI2":      [("R15", "2"), ("J1", "4"), ("J2", "4")],
    "LED_A":    [("R9", "2"), ("D1", "2")],
}


# ------------------------------------------------------------------- routing
# (net, layer, [(x,y), ...])   layer: "B" = B.Cu (all SMD), "F" = F.Cu
W_SIG, W_PWR = 0.25, 0.40
ROUTES = [
    ("OPT_INN",  "B", [(7.5,16.088),(7.5,15.73),(10.19,15.73)]),
    ("OPT_OUT",  "B", [(7.5,17.912),(7.5,20.81),(10.19,20.81)]),
    ("OPT_OUT",  "B", [(10.19,20.81),(17.81,20.81)]),
    ("OPT_OUT",  "B", [(17.81,20.81),(23.088,21.0)]),
    ("OPT_OUT",  "B", [(21.5,14.5),(20.5,15.5),(20.5,19.8),(23.088,21.0)]),

    ("FILT",     "B", [(24.912,21.0),(26.55,21.0)]),
    ("FILT",     "B", [(26.55,21.0),(26.55,23.6),(29.3,23.6),(29.3,17.635),(31.525,17.635)]),
    ("FILT",     "B", [(31.525,17.635),(33.2,17.635),(35.0,16.365),(36.475,16.365)]),
    ("FILT",     "B", [(22.5,18.5),(24.912,19.6),(24.912,21.0)]),
    ("FILT",     "B", [(16.0,31.0),(17.08,25.5)]),
    ("FILT",     "B", [(24.912,21.0),(25.8,22.0)]),
    ("FILT",     "F", [(25.8,22.0),(24.0,22.9),(17.08,22.9),(17.08,25.5)]),

    ("THRESH_A", "B", [(40.912,10.0),(40.912,22.0)]),
    ("THRESH_A", "B", [(40.912,17.635),(36.475,17.635)]),
    ("THRESH_A", "B", [(43.088,10.0),(43.088,11.3),(40.912,11.3)]),
    ("THRESH_A", "B", [(46.5,22.5),(42.5,22.5),(40.912,22.0)]),

    ("CMPA_OUT", "B", [(36.475,15.095),(39.088,12.5)]),
    ("CMPA_OUT", "B", [(38.438,4.0),(38.438,5.6)]),
    ("CMPA_OUT", "F", [(38.438,5.6),(38.0,6.5),(38.0,13.6),(37.8,13.6)]),
    ("CMPA_OUT", "B", [(37.8,13.6),(39.088,12.5)]),
    ("CMPA_OUT", "B", [(43.088,12.5),(43.088,14.3)]),
    ("CMPA_OUT", "F", [(43.088,14.3),(39.088,14.3)]),
    ("CMPA_OUT", "B", [(39.088,14.3),(39.088,12.5)]),
    ("CMPA_OUT", "B", [(40.912,25.0),(42.8,23.9)]),
    ("CMPA_OUT", "F", [(42.8,23.9),(42.8,14.3),(43.088,14.3)]),
    ("CMPA_OUT", "F", [(46.5,20.0),(44.2,20.0),(42.8,19.0)]),

    ("THRESH_B", "B", [(24.088,9.0),(25.912,12.0)]),
    ("THRESH_B", "B", [(25.912,12.0),(27.588,12.0)]),
    ("THRESH_B", "B", [(27.588,12.0),(27.2,14.4)]),
    ("THRESH_B", "F", [(27.2,14.4),(27.2,23.0),(31.5,23.0),(31.5,22.2)]),
    ("THRESH_B", "B", [(31.5,22.2),(31.525,18.905)]),

    ("CMPB_OUT", "B", [(27.588,9.0),(28.4,10.4),(29.412,12.0)]),
    ("CMPB_OUT", "B", [(29.412,12.0),(30.2,13.2),(30.2,15.8),(29.9,16.365)]),
    ("CMPB_OUT", "B", [(31.525,16.365),(29.9,16.365)]),
    ("CMPB_OUT", "F", [(29.9,16.365),(29.9,20.5),(33.0,19.6)]),
    ("CMPB_OUT", "B", [(33.0,19.6),(33.588,22.0)]),

    ("DI1",      "B", [(20.0,31.0),(22.16,25.5)]),
    ("DI1",      "B", [(39.088,25.0),(37.6,26.8)]),
    ("DI1",      "F", [(37.6,26.8),(37.0,24.6),(22.16,24.6),(22.16,25.5)]),

    ("DI2",      "B", [(18.0,31.0),(19.62,25.5)]),
    ("DI2",      "B", [(35.412,22.0),(36.9,23.2)]),
    ("DI2",      "F", [(36.9,23.2),(35.0,23.6),(19.62,23.6),(19.62,25.5)]),

    ("LED_A",    "B", [(34.912,4.0),(36.562,4.0)]),

    ("+3V3",     "B", [(10.95,10.0),(10.19,13.19)]),
    ("+3V3",     "B", [(10.95,10.0),(10.95,7.5),(33.0,7.5)]),
    ("+3V3",     "B", [(25.912,7.5),(25.912,9.0)]),
    ("+3V3",     "B", [(29.412,7.5),(29.412,9.0)]),
    ("+3V3",     "B", [(32.05,7.5),(32.05,11.0)]),
    ("+3V3",     "B", [(33.0,7.5),(33.088,4.0)]),
    ("+3V3",     "B", [(32.05,11.0),(32.05,13.5),(31.525,15.095)]),
    ("+3V3",     "B", [(33.0,7.5),(39.088,7.5),(39.088,10.0)]),
    ("+3V3",     "B", [(39.088,7.5),(41.5,8.8),(44.912,8.8),(44.912,10.0)]),
    ("+3V3",     "B", [(44.912,10.0),(44.912,12.5)]),
    ("+3V3",     "B", [(12.0,31.0),(12.0,25.5)]),
    ("+3V3",     "F", [(10.19,13.19),(8.7,14.5),(8.7,29.0),(12.0,31.0)]),

    ("GND",      "B", [(14.0,31.0),(14.54,25.5)]),
    ("GND",      "B", [(33.95,11.0),(35.6,11.5)]),
]
VIAS = [
    ("GND", 35.6, 11.5), ("FILT", 25.8, 22.0), ("DI1", 37.6, 26.8), ("DI2", 36.9, 23.2),
    ("CMPA_OUT", 43.088, 14.3), ("CMPA_OUT", 39.088, 14.3), ("CMPA_OUT", 42.8, 23.9),
    ("CMPA_OUT", 38.438, 5.6), ("CMPA_OUT", 37.8, 13.6),
    ("THRESH_B", 27.2, 14.4), ("THRESH_B", 31.5, 22.2),
    ("CMPB_OUT", 29.9, 16.365), ("CMPB_OUT", 33.0, 19.6),
]
PWR_NETS = ("+3V3", "GND")


def main():
    board = pcbnew.NewBoard(OUT)

    # ------------------------------------------------------------ footprints
    placed = {}
    for ref, val, lib, fpname, x, y, rot, bottom, dnp in COMPONENTS:
        fp = pcbnew.FootprintLoad(os.path.join(FPBASE, lib + ".pretty"), fpname)
        if fp is None:
            sys.exit("FAILED to load footprint %s:%s" % (lib, fpname))
        fp.SetPosition(V(x, y))
        if rot:
            fp.SetOrientationDegrees(rot)
        # NOTE: Add() must precede Flip() -- Flip on an unparented footprint
        # segfaults (it reaches for board/GUI context that isn't there yet).
        board.Add(fp)
        if bottom:
            fp.Flip(fp.GetPosition(), False)
        fp.SetReference(ref)
        fp.SetValue(val)
        if dnp and hasattr(fp, "SetDNP"):
            fp.SetDNP(True)
        placed[ref] = fp

    # ------------------------------------------------------------------ nets
    netmap = {}
    for name in NETS:
        n = pcbnew.NETINFO_ITEM(board, name)
        board.Add(n)
        netmap[name] = n

    for name, conns in NETS.items():
        for ref, padnum in conns:
            fp = placed.get(ref)
            if fp is None:
                sys.exit("net %s references missing component %s" % (name, ref))
            pad = fp.FindPadByNumber(padnum)
            if pad is None:
                sys.exit("net %s: %s has no pad %s" % (name, ref, padnum))
            pad.SetNet(netmap[name])

    # ------------------------------------------------------------ board edge
    corners = [(0, 0), (BW, 0), (BW, BH), (0, BH)]
    for i in range(4):
        seg = pcbnew.PCB_SHAPE(board)
        seg.SetShape(pcbnew.SHAPE_T_SEGMENT)
        seg.SetStart(V(*corners[i]))
        seg.SetEnd(V(*corners[(i + 1) % 4]))
        seg.SetLayer(pcbnew.Edge_Cuts)
        seg.SetWidth(mm(0.1))
        board.Add(seg)

    # -------------------------------------------------- die-centre crosshair
    # OPT101's photodiode sits at the centre of the package: board-local (15,15).
    # Mark it on both silkscreens so an aperture/hood can be aligned to it.
    cx, cy = DIE_X, DIE_Y
    for layer in (pcbnew.F_SilkS, pcbnew.B_SilkS):
        for (x1, y1, x2, y2) in ((cx - 2.2, cy, cx - 1.0, cy), (cx + 1.0, cy, cx + 2.2, cy),
                                 (cx, cy - 2.2, cx, cy - 1.0), (cx, cy + 1.0, cx, cy + 2.2)):
            s = pcbnew.PCB_SHAPE(board)
            s.SetShape(pcbnew.SHAPE_T_SEGMENT)
            s.SetStart(V(x1, y1))
            s.SetEnd(V(x2, y2))
            s.SetLayer(layer)
            s.SetWidth(mm(0.15))
            board.Add(s)

    # ------------------------------------------------------------ silkscreen
    def text(s, x, y, layer, size=0.9, mirror=False):
        t = pcbnew.PCB_TEXT(board)
        t.SetText(s)
        t.SetPosition(V(x, y))
        t.SetLayer(layer)
        t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
        t.SetTextThickness(mm(size / 6.0))
        if mirror:
            t.SetMirrored(True)
        board.Add(t)

    text("SHEINBERG LAB  -  PHOTOMARKER", 25.0, 2.0, pcbnew.F_SilkS, 1.0)
    text("stimulus onset    rev %s" % REV, 25.0, 4.4, pcbnew.F_SilkS, 0.9)
    text("THIS SIDE FACES THE SCREEN", 30.0, 28.5, pcbnew.F_SilkS, 1.0)
    text("die", DIE_X, DIE_Y + 3.2, pcbnew.F_SilkS, 0.8)
    text("U2: LM393   rev %s" % REV, 32.0, 2.0, pcbnew.B_SilkS, 0.8, mirror=True)
    text("3V3 GND AO DIb DIa", 32.0, 32.0, pcbnew.B_SilkS, 0.8, mirror=True)



    # --------------------------------------------------- silkscreen cleanup
    # Board is dense: auto-placed refdes pile onto pads and each other. Move
    # reference text to the Fab layers (visible in KiCad + assembly drawing)
    # and keep silkscreen for the markings you actually need at the bench.
    for fp in board.Footprints():
        ref = fp.Reference()
        ref.SetLayer(pcbnew.B_Fab if fp.GetLayer() == pcbnew.B_Cu else pcbnew.F_Fab)
        ref.SetTextSize(pcbnew.VECTOR2I(mm(0.7), mm(0.7)))
        ref.SetTextThickness(mm(0.12))
        fp.Value().SetVisible(False)

    # -------------------------------------------------------------- routing
    nets_by_name = {n.GetNetname(): n for n in board.GetNetInfo().NetsByName().values()}

    def track(net, layer, a, b, width):
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(V(*a)); t.SetEnd(V(*b))
        t.SetWidth(mm(width))
        t.SetLayer(pcbnew.B_Cu if layer == "B" else pcbnew.F_Cu)
        t.SetNet(nets_by_name[net])
        board.Add(t)

    ntracks = 0
    for net, layer, pts in ROUTES:
        w = W_PWR if net in PWR_NETS else W_SIG
        for i in range(len(pts) - 1):
            track(net, layer, pts[i], pts[i + 1], w)
            ntracks += 1

    for net, x, y in VIAS:
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(V(x, y))
        v.SetWidth(mm(0.8)); v.SetDrill(mm(0.4))
        v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
        v.SetNet(nets_by_name[net])
        board.Add(v)

    # ---------------------------------------------------------- GND pours
    gnd = nets_by_name["GND"]
    for lyr in (pcbnew.F_Cu, pcbnew.B_Cu):
        z = pcbnew.ZONE(board)
        z.SetLayer(lyr)
        z.SetNet(gnd)
        z.SetLocalClearance(mm(0.3))
        z.SetMinThickness(mm(0.2))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)
        pts = pcbnew.VECTOR_VECTOR2I()
        for (px, py) in ((0.5, 0.5), (BW - 0.5, 0.5), (BW - 0.5, BH - 0.5), (0.5, BH - 0.5)):
            pts.append(V(px, py))
        z.AddPolygon(pts)
        board.Add(z)

    try:
        pcbnew.ZONE_FILLER(board).Fill(board.Zones())
        print("zones filled")
    except Exception as e:
        print("zone fill skipped: %s" % e)

    print("tracks: %d, vias: %d" % (ntracks, len(VIAS)))

    # Place/drill origin at the board's bottom-left corner so the CPL comes out
    # as "origin bottom-left, Y up" -- what JLCPCB expects.
    board.GetDesignSettings().SetAuxOrigin(V(0, BH))

    board.Save(OUT)
    print("wrote %s" % OUT)

    # ------------------------------- report actual pad coords (post-flip!) --
    print("\n--- pad positions (mm, board-local) ---")
    for ref in ("J1", "J2"):
        fp = placed[ref]
        pads = sorted(fp.Pads(), key=lambda p: p.GetNumber())
        for p in pads:
            pos = p.GetPosition()
            print("  %-4s pad %-2s  (%7.3f, %7.3f)  net=%s" % (
                ref, p.GetNumber(),
                pcbnew.ToMM(pos.x) - BX, pcbnew.ToMM(pos.y) - BY,
                p.GetNetname() or "-"))
    print("\nlayer check: U2 on %s" % board.GetLayerName(placed["U2"].GetLayer()))
    print("layer check: U1 on %s" % board.GetLayerName(placed["U1"].GetLayer()))


if __name__ == "__main__":
    main()
