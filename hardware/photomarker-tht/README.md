# Stimulus onset photo-marker — rev B, all through-hole

Sheinberg Lab

A per-rig photodiode board that timestamps stimulus light onset two ways at
once: an **analog waveform** into an MCP3204 channel, and a **comparator edge**
into a dserv DI pin (IRQ-latched with a box-clock stamp, so it beats the ~100 µs
ADC scan quantization).

Wire both. The digital edge is the routine per-trial timestamp; the analog trace
is how you verify the edge means what you think, because a fixed voltage
threshold lands at a different fraction of the transition for black→white than
for a gray-to-gray change.

**This is the buildable version.** Everything is through-hole, both ICs are
socketable DIP-8, and the threshold is a trimpot you set with a screwdriver
while watching a meter. Rev A (SMD, in `../photomarker/`) is smaller and stays
valid, but this one is the one to hand a student.

- **75 × 55 mm**, 2-layer, M3 mounts on a **63 × 43 mm** pattern
- **DRC clean**: 0 violations, 0 unconnected · **ERC clean** apart from 3
  cosmetic `lib_symbol_mismatch` (see *Schematic* below)
- Schematic netlist **diff-verified against the board** — nets, refs, values,
  footprints and DNP flags all agree
- OPT101 die centre at board-local **(13.81, 19.81) mm**, crosshaired on both
  silkscreens so an aperture or hood can be aligned to it
- **Every part is labelled on the silkscreen with its reference *and* value** —
  `R1 10k`, `C3 3n3 C0G`, `R6 DNP` — on the side you insert it from, so you can
  populate the board without a BOM in hand. `assembly-top.pdf` and
  `assembly-bottom.pdf` are the printable versions.

## Board sides

**Top faces the screen.** Only U1 (the OPT101) is mounted from the top, so its
window looks at the display and nothing shadows it.

**Everything else mounts from the bottom**, away from the screen — including the
trimpot, so you can adjust it with the board in place.

Component leads protrude slightly on the screen side. **Trim them flush** and
use standoffs on the M3 holes; don't let the board touch the panel.

## Building it

Order **5 bare boards** — `fab/photomarker-tht-gerbers.zip`. No assembly, no BOM
upload, no CPL, no part-matching, no Extended-part fees. Get the parts from
Mouser (see `../photomarker/fab/mouser-parts.md`, minus the SMD packages, plus
the items below).

### Parts that differ from rev A

| Ref | Part |
|-----|------|
| U2 | **LM393N / LM393P** — DIP-8, *not* the SOIC |
| RV1 | **Bourns 3296W-1-503** — 50 kΩ, 25-turn trimmer |
| R1–R7, R11–R15 | ¼ W axial, **1 %**, 7.62 mm (0.3") lead pitch |
| C1, C2 | 100 nF ceramic disc, 5 mm lead pitch |
| C3 | **3.3 nF C0G/NP0**, 5 mm lead pitch |
| — | 2× **DIP-8 sockets** (optional but recommended) |

Sockets are worth it: they keep soldering heat off the OPT101 entirely, and let
a student swap a part they've cooked without touching the board again.

C3 must still be **C0G/NP0, not X7R** — X7R drifts with temperature and DC bias,
which moves the filter corner and turns your calibrated constant lag into a
drifting one.

### Suggested order

1. **Resistors first** (lowest parts), then the caps, then the trimpot.
2. **Sockets** for U1 and U2 — don't insert the ICs yet.
3. **J1 from the bottom.**
4. Insert U1 **from the top**, U2 from the bottom, watching pin 1.
5. Bring-up below.

## Setting the threshold — the part that's different

RV1 (50 k) is in series with R1 (10 k), so the effective R1 sweeps **10–60 kΩ**:

| R1 effective | Onset threshold | Hysteresis |
|---|---|---|
| 10 k (pot at min) | 1.68 V | 60 mV |
| **39 k (design point)** | **0.749 V** | **94 mV** |
| 60 k (pot at max) | 0.56 V | 102 mV |

The design point sits at ~58 % of pot travel, comfortably mid-range.

**Set it by measuring TP3, not by eye.** That is what makes a trimpot
reproducible here rather than a "turn it till it works" knob — you are setting a
*measured* voltage, and TP3 sits directly on the threshold node.

Aim for **~50 % of your lit-state level at TP1**, not 20 %. Mid-transition is
where the panel's rise is steepest, so timing there is least sensitive to
stimulus amplitude drift. That target is monitor-dependent, which is exactly why
this is adjustable.

RV1 is wired as a rheostat: **wiper + one end, pin 3 deliberately open.** Tying
pin 3 to the wiper halves the range; tying it to pin 1 makes the adjustment
non-monotonic. Leave it open.

## Connector J1

Pin 1 is leftmost. Silkscreen reads `J1  1=3V3 2=GND 3=AOUT 4=DIb 5=DIa`.

| Pin | Net | To the extio box |
|-----|-----|------------------|
| 1 | +3V3 | 3V3 |
| 2 | GND | GND |
| 3 | AOUT | MCP3204 **CH2** (CH0/CH1 are the joystick) |
| 4 | DI-B | free GPIO — optional second threshold |
| 5 | DI-A | free GPIO — **main onset edge** |

Free GPIOs per `wiznet-io/PINMAP.md`: 9–14, 26, 27.

**Output goes LOW on light onset** (signal on IN−), so configure that DI for the
falling edge. With the LM393's open-collector output that is also the *fast*
active edge; the rising edge is an RC through R7.

On the `ain` group carrying CH2: set `deadband 0` and `average 1`. Both smear the
edge, which is the one thing you are trying to measure.

## Bring-up

Test points: **TP1** OPT_OUT, **TP2** FILT, **TP3** THRESH, **TP4** CMP_OUT,
**TP5** GND.

1. **Power only, sensor covered.** TP1 ≈ 7.5 mV. Mid-scale or railed means a
   part is in backwards or the OPT101 is damaged.
2. **Set TP3** with RV1 to your chosen threshold.
3. **Point at a white screen.** TP1 should rise well clear of TP3. If TP1 pins
   at ~2.0 V you are saturated — fit R6, mask down to a smaller aperture, or
   back off. **The LM393's common-mode ceiling is ~1.8 V at 3.3 V**, so a
   saturated OPT101 also pushes IN− out of range. Keep the lit level below that.
4. **Watch TP4** — it should swing cleanly with the stimulus.
5. **Check for backlight PWM on the analog channel before trusting anything.** A
   few-hundred-Hz chop cannot be filtered out without wrecking a 2 ms edge. Run
   the panel at 100 % brightness, where most panels stop PWM-ing. Only if you're
   stuck with it, raise C3 to 22 nF and accept the larger constant lag.

## DNP options

| Ref | Purpose |
|-----|---------|
| R6 | OPT101 gain reduction — **fit if you saturate**, likely at 0.45 V/µW. Pick the value after seeing TP1 on a real screen. |
| R12, R13, R14, R15 | Comparator B's second threshold → DI-B. Populate to time the 25 %→75 % crossing and measure the panel's transition slope in hardware, per trial. |

**R11 (10 k) is fitted, not DNP.** With the B network unpopulated it pulls
comparator B's IN+ to 3.3 V — above IN−'s ceiling — so that comparator parks
instead of oscillating on a floating input on the same die as the one you care
about.

Rev A's bring-up LED (R9/D1) is **not** on this board. TP4 exposes the same node
for a scope or meter, and dropping it removed several routing conflicts.

## Regenerating

Both files are generated, not hand-drawn:

```bash
/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 gen_photomarker_tht.py
/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 gen_schematic_tht.py
/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 check_netlist.py
```

`gen_schematic_tht.py` **reads the netlist out of the .kicad_pcb**, so the
schematic cannot silently drift. `check_netlist.py` proves it and exits nonzero
on any mismatch — run it before ordering.

Three pcbnew gotchas are baked into the scripts: `board.Add(fp)` must precede
`fp.Flip()` (flipping an unparented footprint segfaults); flipping **mirrors in
X**, so a part's body extends the opposite way from its pin 1; and schematic pins
land at `(X+px, Y−py)` because library Y is up while sheet Y is down.

## Schematic

`photomarker-tht.kicad_sch` + `schematic.pdf`. Only the **OPT101** needs a custom
symbol (`photomarker-tht.kicad_sym`) — unlike rev A, U2 uses KiCad's stock
`Comparator:LM393`.

Connectivity is by net label rather than drawn wire, with components grouped by
function. That is deliberate for a generated schematic: label connectivity is
verifiable by ERC *and* by netlist diff, where hand-computed wire geometry can
look connected while being fractionally short.

ERC reports **3 `lib_symbol_mismatch`** on LM393. That is cosmetic: KiCad's stock
LM393 uses `extends "LM2903"`, and the generator inlines the parent's geometry
because emitting the `extends` form leaves KiCad unable to resolve the parent —
the symbol then comes out with *no pins at all*, which is far worse. Pins and
netlist are correct; `check_netlist.py` is the authority.
