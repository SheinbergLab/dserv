# mdio_health — is the KSZ8081 actually on the bus?

A standalone Zephyr app that resets the PHY itself and then hammers its ID
registers, reporting a **success rate** rather than a pass/fail.

    cd ~/zephyrproject && west build -b frdm_rw612 \
        /Users/sheinb/src/dserv/extio-zephyr/tools/mdio_health -d /tmp/build-mdio
    west flash -d /tmp/build-mdio --dev-id <probe-serial>
    # watch the board UART (J-Link VCOM) at 115200

    addr  2: PHYID1==0x0022 200/200 | PHYID2==0x1561 200/200 | 0xffff 0 ...

## Why a rate and not a check

The ksz8081 driver reports `PHY is still in factory mode!` / `powered down!` for
any FAILED READ — an all-ones read sets the bits it tests. So the driver's
message says nothing about the device, and a single read says nothing either:
this PHY answers intermittently when power is marginal. A rate is something you
can improve against while reseating cables.

KSZ8081 signature: PHYID1 (reg 2) = `0x0022`, PHYID2 (reg 3) = `0x1561`.
`0xffff` = nobody drove the bus. `0x0000` = bus held low.

## Known signatures (2026-07-27)

| state | reads |
|---|---|
| **healthy (MEASURED, board 4, brand new)** | **`0x0022` 200/200 AND `0x1561` 200/200 at BOTH addr 0 and addr 2; zero `0xffff`, zero `0x0000`, zero other** |
| marginal power (recoverable, PORTING.md) | mix of `0x0`, `0xffff`, `0x5`, occasional valid — and it goes to a STABLE 200/200 the instant power is fixed |
| board 3 (faulty, RMA) | `0xffff` 200/200 at addr 2, no variation across 94+ passes |

**Recoverable and dead look similar for one pass and diverge over several.** The
discriminator is not the rate at any instant, it is which way it MOVES and
whether PHYID1 ever reads at all:

- box1/box2 (2026-07-28, both fine): valid reads present in the mix, and
  reseating took them to a stable 200/200 **immediately, and it held**.
- board 3, re-measured on a direct connection before RMA: **PHYID1 0/200 at BOTH
  addresses, ever**; addr 2 uniformly `0xffff` (undriven); addr 0 uniformly
  `0x0000` (bus HELD LOW) with `0x1561` decaying 110 → 2 → 0 across four passes.
  Same "answers a little, then goes silent" as the original condemnation.

The healthy case is **perfect, not merely good** — so this is a binary test with no
judgement call in the middle. A board sitting at, say, 150/200 is marginal and
should be treated as a power/seating problem, not accepted.

Note a healthy KSZ8081 answers at **both address 0 and address 2** (address 0 is
the broadcast address), which matches PORTING.md's earlier `mdio scan` finding.
Only addr 2 is the board's configured address.

Board 3 showed two genuine `0x1561` reads early on, then went uniformly silent
and never recovered across both cable configurations and a full ordered
discharge — so "it answered once" is not sufficient evidence of a usable PHY.

## The ID rate says NOTHING about the media side (2026-07-28)

Learned the hard way on box2. MDIO is the MANAGEMENT interface; it is on the
board and does not traverse the RJ45, and it works regardless of what the media
side is doing. So `200/200` next to `autonegotiation timed out` is not a
contradiction, and neither reading alone tells you where to look:

| signature | where the fault is |
|---|---|
| garbage IDs (`0x0000`/`0xffff`/`0x0005` mixed) | **POWER PATH** -- USB-C seating, hubs. The Ethernet cable is IRRELEVANT: MDIO never touches it. |
| IDs perfect + autoneg timeout | **the wire** -- cable, or the switch port |
| IDs perfect + `BMSR [LINK aneg-done]` | PHY and wire are both FINE; look at the host software |

So the meter now also dumps the media side (`status()`): BMCR (power-down /
isolate / aneg-enable), BMSR read TWICE because link status is latching-low, and
KSZ8081 PHY Control 1/2. A healthy linked board reads

    BMCR 3100 [aneg-en ] BMSR 786d/786d [LINK aneg-done ] CTL1 0136 CTL2 8180

`CTL1` bits [2:0] are the operation-mode indication: `6` = 100BASE-TX full duplex.

**This is what settled box2**: it read 200/200 AND `LINK aneg-done` at 100 Mb
full duplex, while the extio firmware on the same board had just reported
`autonegotiation timed out`. That ruled out both the board and the cable in one
reading, and pointed at power -- correctly.

## Do not decode a failed read (this tool got it wrong too)

`0xffff` = nobody drove the bus, `0x0000` = bus held low. NEITHER is a register
value. The first version of `status()` decoded them anyway and printed

    BMCR ffff [POWER-DOWN ISOLATE aneg-en aneg-restart ] BMSR ffff [LINK aneg-done ...]

— a confident-sounding diagnosis assembled entirely out of an absent device, and
the exact mistake this tool exists to correct in the ksz8081 driver, whose
"PHY is still in factory mode!" is that same all-ones read. Caught on the RMA
candidate hours after the README warning above was written. `status()` now
refuses to decode unless the read is live.

## Do not read the LEDs

The green LED by the debug header is the PROBE's USB status, not the PHY. And
"PHY lights solid, no blinking" was observed on a board whose PHY was, per its
own BMSR, fully linked. The LEDs have now sent this hunt the wrong way twice.
