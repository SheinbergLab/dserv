#!/usr/bin/env python3
"""
Generate photomarker.kicad_sym (custom OPT101 + TLV3202 symbols) and
photomarker.kicad_sch, driven by the *actual netlist in the PCB* so the two
can never drift apart.

KiCad ships no OPT101 and no TLV3202 symbol, so both are authored here.
TLV3202 reuses KiCad's LM2903 dual-comparator geometry (same pinout) with
push-pull outputs instead of open-collector.

Run with KiCad's bundled python:
  .../Python.framework/Versions/3.9/bin/python3 gen_schematic.py
"""
import hashlib
import os
import re
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
PCB = os.path.join(HERE, "photomarker.kicad_pcb")
SYMLIB = os.path.join(HERE, "photomarker.kicad_sym")
SCH = os.path.join(HERE, "photomarker.kicad_sch")
STOCK = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols"

_uid = [0]


def uuid(tag=""):
    """Deterministic UUIDs so regenerating doesn't churn the file."""
    _uid[0] += 1
    h = hashlib.md5(("photomarker-%s-%d" % (tag, _uid[0])).encode()).hexdigest()
    return "%s-%s-%s-%s-%s" % (h[0:8], h[8:12], h[12:16], h[16:20], h[20:32])


# --------------------------------------------------------------- custom symbols
def pin(num, name, etype, x, y, ang, length=2.54):
    return '''\t\t\t(pin %s line
\t\t\t\t(at %s %s %d)
\t\t\t\t(length %s)
\t\t\t\t(name "%s" (effects (font (size 1.27 1.27))))
\t\t\t\t(number "%s" (effects (font (size 1.27 1.27))))
\t\t\t)''' % (etype, x, y, ang, length, name, num)


def props(ref, value, footprint, desc, datasheet):
    out = []
    for n, v, hide, px, py in (("Reference", ref, "no", -10.16, 12.7),
                               ("Value", value, "no", -10.16, -12.7),
                               ("Footprint", footprint, "yes", 0, 0),
                               ("Datasheet", datasheet, "yes", 0, 0),
                               ("Description", desc, "yes", 0, 0)):
        out.append('''\t\t(property "%s" "%s"
\t\t\t(at %s %s 0)
\t\t\t(show_name no) (do_not_autoplace no)%s
\t\t\t(effects (font (size 1.27 1.27)) (justify left))
\t\t)''' % (n, v, px, py, "\n\t\t\t(hide yes)" if hide == "yes" else ""))
    return "\n".join(out)


OPT101_SYM = '''\t(symbol "OPT101"
\t\t(pin_names (offset 0.508))
\t\t(exclude_from_sim no) (in_bom yes) (on_board yes)
%s
\t\t(symbol "OPT101_0_1"
\t\t\t(rectangle
\t\t\t\t(start -7.62 7.62) (end 7.62 -7.62)
\t\t\t\t(stroke (width 0.254) (type default))
\t\t\t\t(fill (type background))
\t\t\t)
\t\t\t(polyline
\t\t\t\t(pts (xy -3.81 1.27) (xy -3.81 -1.27) (xy -1.27 0) (xy -3.81 1.27))
\t\t\t\t(stroke (width 0.2) (type default)) (fill (type none))
\t\t\t)
\t\t\t(polyline
\t\t\t\t(pts (xy -1.27 1.27) (xy -1.27 -1.27))
\t\t\t\t(stroke (width 0.2) (type default)) (fill (type none))
\t\t\t)
\t\t\t(polyline
\t\t\t\t(pts (xy -5.72 3.3) (xy -4.45 2.03))
\t\t\t\t(stroke (width 0.15) (type default)) (fill (type none))
\t\t\t)
\t\t\t(polyline
\t\t\t\t(pts (xy -4.45 3.3) (xy -3.18 2.03))
\t\t\t\t(stroke (width 0.15) (type default)) (fill (type none))
\t\t\t)
\t\t\t(polyline
\t\t\t\t(pts (xy 1.27 3.81) (xy 5.08 0) (xy 1.27 -3.81) (xy 1.27 3.81))
\t\t\t\t(stroke (width 0.2) (type default)) (fill (type none))
\t\t\t)
\t\t)
\t\t(symbol "OPT101_1_1"
%s
\t\t)
\t\t(embedded_fonts no)
\t)'''

TLV_UNIT = '''\t\t(symbol "TLV3202_%d_1"
\t\t\t(polyline
\t\t\t\t(pts (xy -5.08 5.08) (xy 5.08 0) (xy -5.08 -5.08) (xy -5.08 5.08))
\t\t\t\t(stroke (width 0.254) (type default))
\t\t\t\t(fill (type background))
\t\t\t)
%s
\t\t)'''


def build_symlib():
    o_pins = "\n".join([
        pin("1", "Vs", "power_in", 0, 10.16, 270),
        pin("3", "-V", "power_in", 0, -10.16, 90),
        pin("2", "-IN", "input", -10.16, 5.08, 0),
        pin("8", "COM", "passive", -10.16, 0, 0),
        pin("7", "NC", "no_connect", -10.16, -5.08, 0),
        pin("5", "OUT", "output", 10.16, 5.08, 180),
        pin("4", "1M", "passive", 10.16, 0, 180),
        pin("6", "NC", "no_connect", 10.16, -5.08, 180),
    ])
    opt = OPT101_SYM % (
        props("U", "OPT101", "Package_DIP:DIP-8_W7.62mm",
              "Monolithic photodiode + transimpedance amp, DIP-8",
              "https://www.ti.com/lit/ds/symlink/opt101.pdf"),
        o_pins)

    u1 = "\n".join([pin("1", "", "output", 7.62, 0, 180),
                    pin("2", "-", "input", -7.62, -2.54, 0),
                    pin("3", "+", "input", -7.62, 2.54, 0)])
    u2 = "\n".join([pin("5", "+", "input", -7.62, 2.54, 0),
                    pin("6", "-", "input", -7.62, -2.54, 0),
                    pin("7", "", "output", 7.62, 0, 180)])
    u3 = "\n".join([pin("4", "V-", "power_in", -2.54, -7.62, 90, 3.81),
                    pin("8", "V+", "power_in", -2.54, 7.62, 270, 3.81)])
    tlv = '''\t(symbol "TLV3202"
\t\t(pin_names (offset 0.127))
\t\t(exclude_from_sim no) (in_bom yes) (on_board yes)
%s
%s
%s
\t\t(symbol "TLV3202_3_1"
%s
\t\t)
\t\t(embedded_fonts no)
\t)''' % (props("U", "TLV3202", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm",
                "Dual push-pull comparator, SOIC-8. Pin-identical to LM393.",
                "https://www.ti.com/lit/ds/symlink/tlv3201.pdf"),
         TLV_UNIT % (1, u1), TLV_UNIT % (2, u2), u3)

    with open(SYMLIB, "w") as f:
        f.write('(kicad_symbol_lib\n\t(version 20241209)\n\t(generator "photomarker")\n'
                '\t(generator_version "9.0")\n%s\n%s\n)\n' % (opt, tlv))
    print("wrote %s" % SYMLIB)


# ------------------------------------------------- stock symbol block extraction
def sexp_block(text, start):
    d = 0
    for j in range(start, len(text)):
        if text[j] == '(':
            d += 1
        elif text[j] == ')':
            d -= 1
            if d == 0:
                return text[start:j + 1]
    raise ValueError("unbalanced")


def get_stock(lib, name):
    src = open(os.path.join(STOCK, lib + ".kicad_sym")).read()
    blk = sexp_block(src, src.index('(symbol "%s"' % name))
    ext = re.search(r'\(extends "([^"]+)"\)', blk)
    if ext:
        parent = sexp_block(src, src.index('(symbol "%s"' % ext.group(1)))
        # splice the parent's drawing units in, drop the extends line
        units = re.findall(r'\t\t\(symbol "%s_\d+_\d+"' % re.escape(ext.group(1)), parent)
        bodies = []
        for u in units:
            bodies.append(sexp_block(parent, parent.index(u.strip()))
                          .replace('"%s_' % ext.group(1), '"%s_' % name))
        blk = blk.replace(ext.group(0), "").rstrip()[:-1] + "\n" + \
            "\n".join("\t\t" + b for b in bodies) + "\n\t)"
    return blk.replace('(symbol "%s"' % name, '(symbol "%s:%s"' % (lib, name), 1)




# ---------------------------------------------------------------------- layout
# All symbols are placed at angle 0 so pin math stays trivial:
#   library Y is up, schematic Y is down  ->  pin(px,py) lands at (X+px, Y-py)
PLACE = {
    "U1": (71.12, 88.9), "R6": (45.72, 88.9), "TP1": (99.06, 66.04),
    "R4": (111.76, 78.74), "C3": (127.0, 96.52), "TP2": (127.0, 68.58),
    "U2A": (187.96, 88.9), "R1": (157.48, 53.34), "R8": (144.78, 53.34), "R2": (157.48, 127.0),
    "R3": (215.9, 53.34), "R7": (231.14, 53.34), "R5": (243.84, 88.9),
    "TP3": (149.86, 78.74), "TP4": (261.62, 71.12),
    "U2B": (187.96, 185.42), "R11": (157.48, 149.86), "R12": (157.48, 220.98),
    "R13": (215.9, 149.86), "R14": (231.14, 149.86), "R15": (243.84, 185.42),
    "U2C": (78.74, 205.74), "C1": (104.14, 210.82), "C2": (127.0, 210.82), "TP5": (147.32, 203.2),
    "R9": (299.72, 58.42), "D1": (299.72, 83.82),
    "J1": (353.06, 104.14), "J2": (353.06, 165.1),
    "H1": (330.2, 243.84), "H2": (350.52, 243.84), "H3": (370.84, 243.84), "H4": (391.16, 243.84),
}
NOTES = [
    (40.64, 45.72, "SENSOR  -  OPT101 (top side, faces the screen)"),
    (111.76, 45.72, "FILTER  4.8 kHz"),
    (149.86, 40.64, "COMPARATOR A  -  onset edge      threshold 0.749 V, hysteresis 94 mV"),
    (149.86, 137.16, "COMPARATOR B  -  optional 2nd threshold. R11 fitted parks IN+ high so it cannot float."),
    (60.96, 190.5, "POWER  -  U2 is SOIC-8: TLV3202 (push-pull) or LM393 (open-collector, fit R7/R14)"),
    (287.02, 45.72, "BRING-UP LED (DNP)"),
    (330.2, 88.9, "J1 / J2  1=3V3 2=GND 3=AOUT 4=DI-B 5=DI-A"),
    (40.64, 238.76, "Hysteresis sits entirely on the RELEASE side: it costs nothing on the onset edge."),
]
# bare footprint name -> schematic symbol lib_id.  Footprints loaded via
# FootprintLoad() carry no library nickname, so match on the bare name and
# map back to the full lib:name for the Footprint property.
FP_LIB = {
    "DIP-8_W7.62mm": "Package_DIP",
    "SOIC-8_3.9x4.9mm_P1.27mm": "Package_SO",
    "R_0805_2012Metric": "Resistor_SMD",
    "C_0805_2012Metric": "Capacitor_SMD",
    "LED_0805_2012Metric": "LED_SMD",
    "JST_PH_B5B-PH-K_1x05_P2.00mm_Vertical": "Connector_JST",
    "PinHeader_1x05_P2.54mm_Vertical": "Connector_PinHeader_2.54mm",
    "TestPoint_THTPad_D1.5mm_Drill0.7mm": "TestPoint",
    "MountingHole_3.2mm_M3_Pad": "MountingHole",
}
SYM_FOR = {
    "DIP-8_W7.62mm": "photomarker:OPT101",
    "SOIC-8_3.9x4.9mm_P1.27mm": "photomarker:TLV3202",
    "R_0805_2012Metric": "Device:R",
    "C_0805_2012Metric": "Device:C",
    "LED_0805_2012Metric": "Device:LED",
    "JST_PH_B5B-PH-K_1x05_P2.00mm_Vertical": "Connector_Generic:Conn_01x05",
    "PinHeader_1x05_P2.54mm_Vertical": "Connector_Generic:Conn_01x05",
    "TestPoint_THTPad_D1.5mm_Drill0.7mm": "Connector:TestPoint",
    "MountingHole_3.2mm_M3_Pad": "Mechanical:MountingHole_Pad",
}
# lib_id -> {pin number: (px, py) in library coords}
PINPOS = {
    "Device:R": {"1": (0, 3.81), "2": (0, -3.81)},
    "Device:C": {"1": (0, 3.81), "2": (0, -3.81)},
    "Device:LED": {"1": (-3.81, 0), "2": (3.81, 0)},
    "Connector:TestPoint": {"1": (0, 0)},
    "Mechanical:MountingHole_Pad": {"1": (0, -2.54)},
    "Connector_Generic:Conn_01x05": {str(n): (-5.08, 5.08 - 2.54 * (n - 1)) for n in range(1, 6)},
    "photomarker:OPT101": {"1": (0, 10.16), "3": (0, -10.16), "2": (-10.16, 5.08),
                           "8": (-10.16, 0), "7": (-10.16, -5.08), "5": (10.16, 5.08),
                           "4": (10.16, 0), "6": (10.16, -5.08)},
    "photomarker:TLV3202": {"1": (7.62, 0), "2": (-7.62, -2.54), "3": (-7.62, 2.54),
                            "5": (-7.62, 2.54), "6": (-7.62, -2.54), "7": (7.62, 0),
                            "4": (-2.54, -7.62), "8": (-2.54, 7.62)},
}
UNIT_OF = {"1": 1, "2": 1, "3": 1, "5": 2, "6": 2, "7": 2, "4": 3, "8": 3}
POWER = {"GND": "power:GND", "+3V3": "power:+3V3"}


def build_sch():
    board = pcbnew.LoadBoard(PCB)
    root = uuid("root")
    parts = []          # (ref, lib_id, unit, x, y, value, footprint, dnp, pins[(num,net)])
    for fp in board.Footprints():
        ref = fp.GetReference()
        bare = str(fp.GetFPID().GetUniStringLibId()).split(":")[-1]
        lib = SYM_FOR.get(bare)
        if lib is None:
            sys.exit("no symbol mapped for %s (%s)" % (ref, bare))
        fpid = "%s:%s" % (FP_LIB[bare], bare)
        pins = [(p.GetNumber(), p.GetNetname()) for p in fp.Pads()]
        pins = sorted(set(pins))
        if lib == "photomarker:TLV3202":
            for u, suffix in ((1, "A"), (2, "B"), (3, "C")):
                sel = [(n, net) for n, net in pins if UNIT_OF[n] == u]
                x, y = PLACE[ref + suffix]
                parts.append((ref, lib, u, x, y, fp.GetValue(), fpid, fp.IsDNP(), sel))
        else:
            x, y = PLACE[ref]
            parts.append((ref, lib, 1, x, y, fp.GetValue(), fpid, fp.IsDNP(), pins))

    body, used = [], set()
    for ref, lib, unit, X, Y, val, fpid, dnp, pins in parts:
        used.add(lib)
        rdx, rdy = {"photomarker:OPT101": (-10.16, -13.97),
                    "photomarker:TLV3202": (-5.08, -11.43),
                    "Connector_Generic:Conn_01x05": (2.54, -10.16)}.get(lib, (3.81, -2.54))
        pin_uuids = "\n".join('\t\t(pin "%s" (uuid "%s"))' % (n, uuid("p")) for n, _ in pins)
        body.append('''\t(symbol
\t\t(lib_id "%s") (at %s %s 0) (unit %d)
\t\t(exclude_from_sim no) (in_bom yes) (on_board yes) (dnp %s)
\t\t(uuid "%s")
\t\t(property "Reference" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left)))
\t\t(property "Value" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left)))
\t\t(property "Footprint" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (hide yes)))
%s
\t\t(instances (project "photomarker" (path "/%s" (reference "%s") (unit %d))))
\t)''' % (lib, X, Y, unit, "yes" if dnp else "no", uuid("sym"),
         ref + ("ABC"[unit - 1] if lib.endswith("TLV3202") else ""), X + rdx, Y + rdy,
         val, X + rdx, Y + rdy + 2.54, fpid, X, Y, pin_uuids, root, ref, unit))

        for num, net in pins:
            if not net:
                continue
            px, py = PINPOS[lib][num]
            ax, ay = X + px, Y - py            # library Y-up -> schematic Y-down
            dx = (px > 0) - (px < 0)
            dy = -((py > 0) - (py < 0))
            if dx == 0 and dy == 0:
                dy = -1                        # TestPoint: stub upward
            ex, ey = ax + dx * 3.81, ay + dy * 3.81
            body.append('\t(wire (pts (xy %s %s) (xy %s %s)) '
                        '(stroke (width 0) (type default)) (uuid "%s"))'
                        % (ax, ay, ex, ey, uuid("w")))
            if net in POWER:
                body.append('''\t(symbol
\t\t(lib_id "%s") (at %s %s 0) (unit 1)
\t\t(exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no)
\t\t(uuid "%s")
\t\t(property "Reference" "#PWR%03d" (at %s %s 0) (effects (font (size 1.27 1.27)) (hide yes)))
\t\t(property "Value" "%s" (at %s %s 0) (effects (font (size 1.27 1.27))))
\t\t(pin "1" (uuid "%s"))
\t\t(instances (project "photomarker" (path "/%s" (reference "#PWR%03d") (unit 1))))
\t)''' % (POWER[net], ex, ey, uuid("pwr"), _uid[0], ex, ey,
                 net, ex, ey + (3 if net == "GND" else -3), uuid("pp"), root, _uid[0]))
                used.add(POWER[net])
            else:
                ang = 0 if dx > 0 else 180 if dx < 0 else 90 if dy < 0 else 270
                body.append('\t(label "%s" (at %s %s %d) '
                            '(effects (font (size 1.27 1.27)) (justify left bottom)) (uuid "%s"))'
                            % (net, ex, ey, ang, uuid("l")))
        # OPT101 has two genuine NC pins
        if lib == "photomarker:OPT101":
            for num in ("6", "7"):
                px, py = PINPOS[lib][num]
                body.append('\t(no_connect (at %s %s) (uuid "%s"))'
                            % (X + px, Y - py, uuid("nc")))

    # PWR_FLAG on each rail: the rails are fed from the connector, so nothing
    # on this sheet is a power *output* without them.
    for net, fx, fy in (("+3V3", 25.4, 259.08), ("GND", 60.96, 259.08)):
        used.add(POWER[net]); used.add("power:PWR_FLAG")
        py_ = fy - 5.08 if net == "+3V3" else fy + 5.08
        body.append('\t(wire (pts (xy %s %s) (xy %s %s)) '
                    '(stroke (width 0) (type default)) (uuid "%s"))' % (fx, fy, fx, py_, uuid("w")))
        for lid, sx, sy in ((POWER[net], fx, py_), ("power:PWR_FLAG", fx, fy)):
            body.append('''\t(symbol
\t\t(lib_id "%s") (at %s %s 0) (unit 1)
\t\t(exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no)
\t\t(uuid "%s")
\t\t(property "Reference" "#FLG%03d" (at %s %s 0) (effects (font (size 1.27 1.27)) (hide yes)))
\t\t(property "Value" "%s" (at %s %s 0) (effects (font (size 1.27 1.27))))
\t\t(pin "1" (uuid "%s"))
\t\t(instances (project "photomarker" (path "/%s" (reference "#FLG%03d") (unit 1))))
\t)''' % (lid, sx, sy, uuid("f"), _uid[0], sx, sy,
           net if lid != "power:PWR_FLAG" else "PWR_FLAG", sx, sy - 2.54,
           uuid("fp"), root, _uid[0]))

    for x, y, txt in NOTES:
        body.append('\t(text "%s" (at %s %s 0) (effects (font (size 1.6 1.6) (bold yes)) '
                    '(justify left)) (uuid "%s"))' % (txt, x, y, uuid("t")))

    libs = []
    for lid in sorted(used):
        libname, sname = lid.split(":", 1)
        if libname == "photomarker":
            src = open(SYMLIB).read()
            blk = sexp_block(src, src.index('(symbol "%s"' % sname))
            libs.append(blk.replace('(symbol "%s"' % sname, '(symbol "%s"' % lid, 1))
        else:
            libs.append(get_stock(libname, sname))

    with open(SCH, "w") as f:
        f.write('''(kicad_sch
\t(version 20250114)
\t(generator "photomarker")
\t(generator_version "9.0")
\t(uuid "%s")
\t(paper "A3")
\t(title_block
\t\t(title "Stimulus onset photo-marker")\n\t\t(company "Sheinberg Lab")\n\t\t(rev "A")
\t\t(rev "A")
\t\t(comment 1 "Netlist generated from photomarker.kicad_pcb - the two cannot drift")
\t\t(comment 2 "U2: TLV3202 or LM393 (pin-identical). Fit R7/R14 pull-ups for LM393 only.")
\t)
\t(lib_symbols
%s
\t)
%s
\t(sheet_instances (path "/" (page "1")))
\t(embedded_fonts no)
)
''' % (root, "\n".join(libs), "\n".join(body)))
    print("wrote %s  (%d symbols)" % (SCH, len(parts)))


if __name__ == "__main__":
    build_symlib()
    build_sch()
