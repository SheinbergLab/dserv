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

	/* 0 and 2: PORTING.md's mdio scan found the PHY answering at both. */
	while (1) {
		hammer(0);
		hammer(2);
		printk("--- (reseat/repower and watch these climb; 200/200 = healthy) ---\n");
		k_msleep(1000);
	}
	return 0;
}
