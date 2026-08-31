/* MDIO health meter for the KSZ8081.
 *
 * The PHY on this board answers INTERMITTENTLY -- PORTING.md traced that to
 * marginal power (a partially-seated USB-C), not silicon. A single read tells
 * you nothing: 0xffff is an unanswered read, and the ksz8081 driver misreports
 * it as "factory mode" / "powered down". So hammer the ID registers and report
 * a SUCCESS RATE, which is something you can improve against while reseating
 * cables -- rather than a pass/fail that changes every boot.
 *
 * KSZ8081: PHYID1 (reg 2) = 0x0022, PHYID2 (reg 3) = 0x1561.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mdio.h>
#include <zephyr/drivers/gpio.h>

static const struct device *mdio = DEVICE_DT_GET(DT_NODELABEL(enet_mdio));

#define TRIES 200

static void hammer(uint8_t addr)
{
	int ok1 = 0, ok2 = 0, ones = 0, zeros = 0, other = 0;
	uint16_t last_other = 0;

	for (int i = 0; i < TRIES; i++) {
		uint16_t v = 0;

		if (mdio_read(mdio, addr, 2, &v) == 0) {
			if (v == 0x0022)      ok1++;
			else if (v == 0xffff) ones++;
			else if (v == 0x0000) zeros++;
			else { other++; last_other = v; }
		}
		if (mdio_read(mdio, addr, 3, &v) == 0 && v == 0x1561) {
			ok2++;
		}
		k_msleep(2);
	}
	printk("addr %2d: PHYID1==0x0022 %3d/%d | PHYID2==0x1561 %3d/%d | "
	       "0xffff %3d  0x0000 %3d  other %3d (last %04x)\n",
	       addr, ok1, TRIES, ok2, TRIES, ones, zeros, other, last_other);
}

/* The MEDIA side, which a perfect ID-register rate says nothing about.
 *
 * MDIO is the management interface and works regardless of what the media side
 * is doing -- so 200/200 on PHYID1/2 next to an autonegotiation timeout is not a
 * contradiction, it is the signature of a PHY that is reachable and deliberately
 * not linking. The two bits that do that are in BMCR: POWER-DOWN and ISOLATE,
 * either of which can be latched from the strap pins at reset (PORTING.md: "the
 * PHY latches its straps when reset deasserts"). Neither is visible in an ID
 * read, and neither makes the driver say anything useful.
 *
 * BMSR bit 2 is the honest answer to "is there a link": it is LATCHING-LOW, so
 * read it twice -- the first read can report a link that has since dropped. */
/* A register read is only meaningful if the bus was actually driven. */
static inline bool live(uint16_t v) { return v != 0xffff && v != 0x0000; }

static void status(uint8_t addr)
{
	uint16_t bmcr = 0xffff, bmsr = 0xffff, bmsr2 = 0xffff;
	uint16_t ctl1 = 0xffff, ctl2 = 0xffff;

	mdio_read(mdio, addr, 0, &bmcr);
	mdio_read(mdio, addr, 1, &bmsr);
	mdio_read(mdio, addr, 1, &bmsr2);
	mdio_read(mdio, addr, 0x1e, &ctl1);   /* KSZ8081 PHY Control 1 */
	mdio_read(mdio, addr, 0x1f, &ctl2);   /* KSZ8081 PHY Control 2 */

	/* DECODE NOTHING FROM A FAILED READ.
	 *
	 * 0xffff means nobody drove the bus and 0x0000 means it was held low --
	 * neither is a register value. Decoding them anyway produces
	 * "BMCR ffff [POWER-DOWN ISOLATE ...]", which is a confident-sounding lie
	 * assembled out of an absent device, and is EXACTLY the mistake this tool
	 * exists to correct in the ksz8081 driver ("PHY is still in factory mode!"
	 * is that same all-ones read). Caught on the RMA candidate 2026-07-28, in
	 * this tool's own output, hours after writing the README warning about it. */
	if (!live(bmcr) || !live(bmsr2)) {
		printk("  BMCR %04x BMSR %04x/%04x CTL1 %04x CTL2 %04x "
		       "-- NO VALID READ, nothing decodable (%s)\n",
		       bmcr, bmsr, bmsr2, ctl1, ctl2,
		       (bmcr == 0xffff) ? "bus not driven" : "bus held low");
		return;
	}

	printk("  BMCR %04x [%s%s%s%s] BMSR %04x/%04x [%s%s%s] CTL1 %04x CTL2 %04x\n",
	       bmcr,
	       (bmcr & (1u << 11)) ? "POWER-DOWN "  : "",
	       (bmcr & (1u << 10)) ? "ISOLATE "     : "",
	       (bmcr & (1u << 12)) ? "aneg-en "     : "aneg-OFF ",
	       (bmcr & (1u <<  9)) ? "aneg-restart " : "",
	       bmsr, bmsr2,
	       (bmsr2 & (1u << 2)) ? "LINK "        : "no-link ",
	       (bmsr2 & (1u << 5)) ? "aneg-done "   : "aneg-incomplete ",
	       (bmsr2 & (1u << 4)) ? "remote-fault " : "",
	       ctl1, ctl2);
}

int main(void)
{
	printk("\n=== KSZ8081 MDIO health ===\n");
	if (!device_is_ready(mdio)) {
		printk("MDIO device NOT READY\n");
		return 0;
	}

#if DT_NODE_HAS_PROP(DT_NODELABEL(phy), reset_gpios)
	{
		const struct gpio_dt_spec rst =
			GPIO_DT_SPEC_GET(DT_NODELABEL(phy), reset_gpios);

		if (gpio_is_ready_dt(&rst)) {
			gpio_pin_configure_dt(&rst, GPIO_OUTPUT_ACTIVE);
			k_msleep(10);
			gpio_pin_set_dt(&rst, 0);
			k_msleep(100);
			printk("PHY reset pulsed, 100 ms settle\n");
		}
	}
#endif

	/* SWEEP ALL 32 ADDRESSES ONCE, BEFORE HAMMERING TWO OF THEM.
	 *
	 * Added 2026-08-31. prj.conf has claimed since day one that this tool would
	 * "scan the MDIO bus ourselves instead of trusting one hardcoded address",
	 * and the loop below then hardcoded 0 and 2 -- so the tool inherited exactly
	 * the assumption it was written to remove.
	 *
	 * That gap has a specific cost. "PHY is dead" and "this board revision put
	 * the PHY somewhere else" produce IDENTICAL output when you only ever look
	 * at two addresses, and the second is far likelier than a second RMA in a
	 * row. A replacement board is precisely the case where the address may have
	 * moved: the strap pins that set the PHYAD are ordinary board resistors.
	 *
	 * A silent sweep is the answer to "is ANYTHING on this bus", which is a
	 * different question from "is the KSZ8081 healthy at its expected address" --
	 * and it has to be asked first, because a no to the second means nothing
	 * until the first is answered. */
	{
		int found = 0;

		printk("MDIO address sweep (0-31), reg2/reg3:\n");
		for (uint8_t a = 0; a < 32; a++) {
			uint16_t id1 = 0, id2 = 0;

			(void) mdio_read(mdio, a, 2, &id1);
			(void) mdio_read(mdio, a, 3, &id2);
			if (live(id1) || live(id2)) {
				printk("  addr %2d: %04x %04x%s\n", a, id1, id2,
				       (id1 == 0x0022 && id2 == 0x1561)
				       ? "   <-- KSZ8081" : "   <-- SOMETHING (not a KSZ8081)");
				found++;
			}
		}
		if (!found) {
			printk("  nothing answered at ANY of the 32 addresses --\n"
			       "  the bus is undriven, so this is not a wrong-address\n"
			       "  problem. Look at PHY power, the reset line, and MDC.\n");
		}
	}

	/* 0 and 2: PORTING.md's mdio scan found the PHY answering at both. */
	while (1) {
		hammer(0);
		hammer(2);
		status(2);
		printk("--- (reseat/repower and watch these climb; 200/200 = healthy) ---\n");
		k_msleep(1000);
	}
	return 0;
}
