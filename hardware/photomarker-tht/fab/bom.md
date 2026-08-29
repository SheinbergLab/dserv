# Mouser BOM - photomarker rev B (through-hole)

For **5 boards**, hand-assembled. Quantities include spares.

`OK` = verified listing. `CHECK` = derived from the maker's scheme, confirm in the cart. `PICK` = you must choose one.

| Qty | Value | Designators | Mouser # | Mfr part | Ext. | Note |
|----:|-------|-------------|----------|----------|-----:|------|
| 10 | 100R | R5 | `603-MFR-25FBF52-100R` | `MFR-25FBF52-100R` | $1.00 | 1% |
| 20 | 100n | C1,C2 | `594-K104K15X7RF5UH5` | `K104K15X7RF5UH5` | $5.60 | 0.1uF 50V X7R disc, 5mm pitch. Decoupling -- X7R is fine here. |
| 26 | 10k | R1,R11,R2,R4 | `603-MFR-25FBF52-10K` | `MFR-25FBF52-10K` | $2.60 | 1% |
| 10 | 270k | R3 | `603-MFR-25FBF52-270K` | `MFR-25FBF52-270K` | $1.00 | 1% |
| 10 | 2k2 | R7 | `603-MFR-25FBF52-2K2` | `MFR-25FBF52-2K2` | $1.00 | 1% |
| 10 | 3n3 C0G | C3 | `505-FKP2D013301G00JS` | `FKP2D013301G00JSSD` | $9.70 | 3.3nF 100V 5% polypropylene film, 5mm pitch. The board silk reads 'C3 3n3 C0G' -- read that as 'the stable dielectric', which this film part satisfies. NEVER X7R here: it drifts with temperature and DC bias and would turn the calibrated filter lag into a drifting one. |
| 6 | 50k | RV1 | `652-3296W-1-503LF` | `3296W-1-503LF` | $13.68 | 25-turn cermet trimmer, top adjust. Sets the threshold. |
| 10 | JST-PH 5 | J1 | `306-B5BPHKSLFSNP` | `B5B-PH-K-S(LF)(SN)` | $2.40 | 5-pin PH vertical header, board side. |
| 10 | LM393 | U2 | `595-LM393P` | `LM393P` | $5.30 | DIP-8. |
| 7 | OPT101 | U1 | `595-OPT101P` | `OPT101P` | $71.82 | 8-DIP clear. ~68% of the order cost -- 5 builds + 2 spares. Stock is healthy (2k+ at Mouser, 3k+ at DigiKey) and TI lists it Active, so the deep-buy hedge is not worth $10/unit. |
| 10 | DIP-8 socket | - | `571-1-2199298-2` | `1-2199298-2` | $2.70 | One each for U1 and U2. Keeps soldering heat off the OPT101 entirely. |
| 5 | PH housing | - | `306-PHR-5PP` | `PHR-5` | $0.50 | Cable-side mating shell for J1. |
| 25 | PH crimps | - | `306-SPH-002T-P0.5S` | `SPH-002T-P0.5S` | $2.75 | 22-28 AWG. Needs the right crimp tool -- else buy ready-made pigtails. |

**Estimated total $120.05** at single-unit pricing -- the real figure is lower, since several lines cross a price break.

## Not populated (DNP)

Buy only if you want the option.

- **J2, R12, R13, R14, R15, R6** - DNP

R6 is the OPT101 gain resistor: **pick its value after seeing TP1 on a real screen**, not now.
