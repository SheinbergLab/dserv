# Mouser parts list — stimulus onset photo-marker rev A

For **5 boards, fully hand-assembled**. Quantities include spares; everything
here is cheap enough that ordering doubles is the right call.

`✓` = part number verified. `?` = follows the manufacturer's documented
numbering scheme but **confirm in the cart before ordering** — a mis-derived
suffix silently gets you the wrong tolerance or dielectric.

## Semiconductors

| Qty | Ref | Part | Note |
|-----|-----|------|------|
| 10 | U1 | **OPT101P** — Texas Instruments ✓ | 8-DIP clear. Legacy through-hole analog part; buy a lifetime supply. |
| 10 | U2 | **LM393DR** — Texas Instruments ✓ | SOIC-8. TI, not a no-name. |

**TLV3202AIDR** is the drop-in upgrade for U2 if you ever want the better offset
(5 mV vs the LM393's 5–9 mV over temp) and a push-pull output. Same SOIC-8
footprint. If you fit it, **remove R7 and R14** — they're open-collector
pull-ups and pointless on a push-pull part.

## Capacitors — 0805

| Qty | Ref | Value | Spec | Part |
|-----|-----|-------|------|------|
| 20 | C1, C2 | 100 nF | 50 V X7R, ±10 % fine | any 0805 X7R |
| 10 | C3 | **3.3 nF** | **50 V C0G/NP0, ±5 %** | **GRM2165C1H332JA01D** — Murata ✓ |

**C3 is the one part where the dielectric is not negotiable.** X7R drifts with
temperature and DC bias, which moves the filter corner and turns your calibrated
constant lag into a drifting one. The listing must say **C0G** or **NP0**, and
tolerance ±5 % (J) — ±10 % (K) is the tell for X7R. Kemet's equivalent is
`C0805C332J5GAC` ?

## Resistors — 0805, thick film, **±1 %**, ≥100 mW

Yageo `RC0805FR-07…L` series numbers below follow Yageo's documented scheme, but
verify each in the cart.

| Qty | Ref | Value | Yageo ? |
|-----|-----|-------|---------|
| 10 | R1 | 39 kΩ | RC0805FR-0739KL |
| 25 | R2, R4, R11 | 10 kΩ | RC0805FR-0710KL |
| 10 | R3 | 270 kΩ | RC0805FR-07270KL |
| 10 | R5 | 100 Ω | RC0805FR-07100RL |
| 20 | R7, R14 | 2.2 kΩ | RC0805FR-072K2L |

**1 % matters on R1, R2, R3.** Their tolerance sets the threshold spread across
rigs — at 5 % the boards would not agree with each other. R5, R7, R14 are
non-critical; 5 % would do, but buy 1 % and keep one bin.

## Connector

| Qty | Part | Note |
|-----|------|------|
| 10 | **B5B-PH-K-S(LF)(SN)** — JST ? | 5-pin PH vertical header, board side |
| 10 | **PHR-5** — JST ? | mating housing, cable side |
| 50 | **SPH-002T-P0.5S** — JST ? | crimp contacts, 22–28 AWG |

Crimping PH contacts needs the right tool. If you don't have one, buy pre-made
5-pin JST-PH pigtails and cut to length — for a handful of rigs that's the
saner path.

## Optional / DNP positions

Only if you want them; the board works without every one of these.

| Ref | Value | What it buys |
|-----|-------|--------------|
| R6 | ~1 MΩ or lower, 0805 | OPT101 gain reduction. **Fit this if you saturate** — likely at 0.45 V/µW. Pick after seeing TP1 on a real screen. |
| R8 | 0805 | Parallel with R1 for fine threshold trim |
| R9 | 1 kΩ 0805 | Bring-up LED series resistor |
| D1 | 0805 LED | Bring-up indicator. **Pad 1 = cathode.** |
| R12, R13, R15 | 0805 | Comparator B's second threshold → DI-B |

## Not on this list

- **PCBs** — 5 bare boards from JLCPCB, `fab/photomarker-gerbers.zip`. No
  assembly, no BOM, no CPL.
- **Stencil** — optional, ~$10 from JLCPCB. All 12 SMD parts are on the bottom
  and the gerbers already carry `B_Paste`, so one reflow replaces 24
  iron-touches. Worth it if you have a hotplate or hot air.
- **Cable** — 5-conductor to the extio box. Twisted pair or shielded; keep it
  away from the monitor's backlight driver.
- **Bracket / hood** — M3 on a 40 × 24 mm pattern. The die centre is at
  board-local (14, 17) mm, marked by a crosshair on both silkscreens.

## Assembly order

1. **SMD first, bottom side** — U2, then the passives. Doing the fine-pitch part
   while the board is flat and uncluttered is much easier.
2. **U1 (OPT101) from the top.** Bake at 85 °C/24 h first if the parts have sat
   in humidity — TI warns the clear plastic is moisture-sensitive and that rapid
   heating can stress the wire bonds. Solder gently regardless.
3. **J1 from the bottom**, so the cable exits away from the screen.
4. Bring-up per the README: cover the sensor (TP1 ≈ 7.5 mV), check TP3 = 0.749 V,
   then point it at a white screen and look for saturation at TP1.
