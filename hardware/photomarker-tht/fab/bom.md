# Mouser BOM - photomarker rev B (through-hole)

For **5 boards**, hand-assembled. Quantities include spares.

`OK` = verified listing. `CHECK` = derived from the maker's scheme, confirm in the cart. `PICK` = you must choose one.

| Qty | Value | Designators | Mfr part | Mfr | ? | Note |
|----:|-------|-------------|----------|-----|---|------|
| 10 | 100R | R5 | `MFR-25FBF52-100R` | Yageo | CHECK | 1% |
| 20 | 100n | C1,C2 | `K104K15X7RF5UH5` | Vishay | CHECK | 100nF 50V X7R disc, 5mm pitch. Any 0.1uF THT is fine. |
| 26 | 10k | R1,R11,R2,R4 | `MFR-25FBF52-10K` | Yageo | CHECK | 1% |
| 10 | 270k | R3 | `MFR-25FBF52-270K` | Yageo | CHECK | 1% |
| 10 | 2k2 | R7 | `MFR-25FBF52-2K2` | Yageo | CHECK | 1% |
| 10 | 3n3 C0G | C3 | `FKP2D013301G00JSSD` | WIMA | OK | 3.3nF 100V +-2% polypropylene film, 5mm pitch. Film is the easy THT answer here; a C0G/NP0 ceramic is equally fine. NEVER X7R -- it drifts with temperature and DC bias and would turn the calibrated lag into a drifting one. |
| 6 | 50k | RV1 | `3296W-1-503LF` | Bourns | OK | 25-turn cermet trimmer, top adjust. |
| 10 | JST-PH 5 | J1 | `B5B-PH-K-S(LF)(SN)` | JST | CHECK | 5-pin PH vertical header, board side. |
| 10 | LM393 | U2 | `LM393P` | TI | CHECK | DIP-8. LM393N is the onsemi equivalent. |
| 10 | OPT101 | U1 | `OPT101P` | TI | OK | 8-DIP clear. Legacy THT analog part -- buy deep. |
| 10 | DIP-8 socket | - | `1-2199298-2` | TE | CHECK | One each for U1 and U2. Keeps soldering heat off the OPT101 entirely. |
| 5 | PH housing | - | `PHR-5` | JST | CHECK | Cable-side mating shell for J1. |
| 25 | PH crimps | - | `SPH-002T-P0.5S` | JST | CHECK | 22-28 AWG. Needs the right crimp tool -- else buy ready-made pigtails. |

## Not populated (DNP)

Buy only if you want the option.

- **J2, R12, R13, R14, R15, R6** - DNP

R6 is the OPT101 gain resistor: **pick its value after seeing TP1 on a real screen**, not now.
