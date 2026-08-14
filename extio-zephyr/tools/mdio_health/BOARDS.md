# FRDM-RW612 board inventory

Inventoried 2026-08-14 by plugging each board's MCU-Link USB-C into the Mac one
at a time and reading the onboard probe's USB serial (`whichboard.sh`).

Every one of these boards carries an MCU-Link reflashed with **SEGGER J-Link OEM
firmware**, so the probe enumerates as vendor `SEGGER`, product `J_Link`.

| probe serial | board | PHY | notes |
|---|---|---|---|
| `1069813330` | 1 | good | the first RW612. PTP bring-up 2026-07-25; PORTING.md quotes its VCOM as `/dev/cu.usbmodem0010698133301` |
| `1063771898` | 2 | good | the 2026-07-28 near-RMA. Fault was a **partly-seated USB-C**, not the board |
| `1067513969` | 3 | **DEAD — RMA** | `PHYID1 0/200` at both MDIO addresses, ever; addr 2 uniformly `0xffff`, addr 0 held low |
| `1060261978` | 4 | good | == box3. The measured 200/200 healthy baseline in README.md |

box1/box2/box3 *names* are not yet pinned to serials -- see below.

## Reading a serial

    sh whichboard.sh

Uses `ioreg`, deliberately. `system_profiler SPUSBDataType` returns a
**completely empty tree** under some sandboxes, which is indistinguishable from
"no board plugged in" -- it cost a wrong answer during this very inventory.
macOS also zero-pads the serial in the USB descriptor (`001067513969`), so a
literal compare against the recorded 10-digit number fails; the script strips it.

## The J-Link VCOM does NOT carry the box CLI

On the RW612 the MCU-Link VCOM (`/dev/cu.usbmodem00<serial>1`) is the **board
UART**, flexcomm3 -- Zephyr's own shell and `printk`. The box CLI (`show`,
`name`, `pin`) lives on the **RW612's own USB-C CDC**, a second cable to the OTG
port. So `show: command not found` on the VCOM says nothing about which firmware
is loaded, and must not be read as "this board isn't running extio" (it was,
once, during this inventory).

What the VCOM *does* give you for free: a board running the extio firmware with
no link chatters `reg: config link down -> re-registering (xNN)` from the
connect-back watchdog. Boards 1 and 2 both did; board 4 was quiet, so its
firmware is simply unknown.

`boxname.py` reads the VCOM and prints whatever answers. To map serial -> box
name you need the OTG cable in as well.

## Do not conclude from the LEDs, or from one MDIO pass

Both traps are written up in `README.md`. The short version: the green LED by
the debug header is the **probe's** USB status, and recoverable-vs-dead look
alike for a single pass -- they diverge over several, and the discriminators are
whether PHYID1 ever reads at all and which way the rate moves.

And these boards go **straight into the machine, never a hub**. An unpowered hub
splits 500 mA across its ports; the RW612 wants real current with the PHY up, and
the result looks like dead silicon rather than like a power problem. That is what
nearly cost board 2 an RMA.
