/*
 * box_gpio.c -- RW612/Zephyr implementation of the box_gpio.h seam.
 * See the header for the design rationale (devicetree pins, CTIMER counter
 * pulse, gpio_callback DI capture) and how it differs from pico_gpio.h.
 */
#include "box_gpio.h"
#include "box_event.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/counter.h>

/* ---- devices (devicetree aliases, resolved per board) ----
 * box pin n -> <box-gpio-port>.n, so the flat wire index is preserved on every
 * board. Each board's overlay supplies two aliases:
 *   box-gpio-port      GPIO controller carrying the box's DO/DI pins
 *   box-pulse-counter  hardware counter for the non-blocking box-timed pulse
 * (RW612: hsgpio0 + ctimer1.  Teensy 4.x: gpio2 + gpt2.) */
#if !DT_NODE_EXISTS(DT_ALIAS(box_gpio_port))
#error "board overlay must define the 'box-gpio-port' alias"
#endif
#if !DT_NODE_EXISTS(DT_ALIAS(box_pulse_counter))
#error "board overlay must define the 'box-pulse-counter' alias"
#endif

static const struct device *port;          /* box-gpio-port     */
static const struct device *pulse_ctr;     /* box-pulse-counter */

#define BOX_PULSE_NCH 4                    /* CTIMER match channels for concurrent pulses */
static uint8_t  pulse_nch;                 /* min(BOX_PULSE_NCH, counter channels) */
static int8_t   pulse_chan_pin[BOX_PULSE_NCH];  /* pin owning channel c, -1 = free */

static inline gpio_pin_t off_of(int n) { return (gpio_pin_t) n; }

/* No board pins are hard-reserved on the RW612 the way GPIO15-22 were the W6300
 * QSPI block on the EVB-Pico2. Peripheral claims (SPI ADC, etc.) will register
 * here as those blocks land; for now nothing is refused. */
static inline int box_gpio_reserved(int n) { (void) n; return 0; }

/* ---- high-resolution timestamp (box clock) ---- */
static inline uint64_t now_us(void)
{
	return k_cyc_to_us_floor64(k_cycle_get_64());
}

/* ---- DI edge capture + debounce ---- */
static volatile uint64_t di_first_edge_us[BOX_NPINS];  /* press/release moment  */
static volatile uint64_t di_last_edge_us[BOX_NPINS];   /* moving quiet-since     */
static volatile uint8_t  di_unsettled[BOX_NPINS];
static uint8_t           di_pub_level[BOX_NPINS];      /* poller-only            */

/* ---- hardware obs-sync input: raw edge latch (clock anchor, unpublished) ---- */
static volatile int      sync_pin = -1;
static volatile uint64_t sync_edge[2];                 /* [0]=fall(end) [1]=rise(begin) */

static struct gpio_callback di_cb;
static bool                 cb_added;

/* single ISR for every configured input + the sync pin. 64-bit fields are
 * written here and read under irq_lock in the poller (single-core CM33). */
static void di_isr(const struct device *dev, struct gpio_callback *cb,
		   gpio_port_pins_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	uint64_t t = now_us();
	bool woke = false;

	for (int i = 0; i < BOX_NPINS; i++) {
		if (!(pins & BIT(i))) {
			continue;
		}
		if (i == sync_pin) {                         /* latch, do not report */
			int lvl = gpio_pin_get(port, off_of(i));
			sync_edge[lvl ? 1 : 0] = t;
			continue;
		}
		if (!di_unsettled[i]) {
			di_first_edge_us[i] = t;
			di_unsettled[i] = 1;
		}
		di_last_edge_us[i] = t;
		woke = true;
	}
	if (woke) {
		box_event_signal();          /* wake the service loop now, don't wait for a poll */
	}
}

uint64_t box_gpio_sync_edge_us(int rising)
{
	unsigned int k = irq_lock();
	uint64_t t = sync_edge[rising ? 1 : 0];
	irq_unlock(k);
	return t;
}

/* ---- non-blocking pulse: CTIMER match channel drops the edge ---- */
static void pulse_expired(const struct device *dev, uint8_t chan,
			  uint32_t ticks, void *user)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ticks);
	int pin = (int)(intptr_t) user;
	gpio_pin_set(port, off_of(pin), 0);
	if (chan < pulse_nch) {
		pulse_chan_pin[chan] = -1;               /* free the channel */
	}
}

int box_gpio_init(void)
{
	port = DEVICE_DT_GET(DT_ALIAS(box_gpio_port));
	if (!device_is_ready(port)) {
		return -1;
	}

	pulse_ctr = DEVICE_DT_GET(DT_ALIAS(box_pulse_counter));
	if (device_is_ready(pulse_ctr)) {
		uint8_t n = counter_get_num_of_channels(pulse_ctr);
		pulse_nch = n < BOX_PULSE_NCH ? n : BOX_PULSE_NCH;
		for (int c = 0; c < BOX_PULSE_NCH; c++) {
			pulse_chan_pin[c] = -1;
		}
		counter_start(pulse_ctr);
	} else {
		pulse_ctr = NULL;                        /* busy-wait fallback */
		pulse_nch = 0;
	}
	return 0;
}

void box_gpio_apply_config(const box_config_t *c)
{
	if (cb_added) {                                  /* rebuild the input set */
		gpio_remove_callback(port, &di_cb);
		cb_added = false;
	}
	gpio_port_pins_t input_mask = 0;

	for (int i = 0; i < BOX_NPINS; i++) {
		if (box_gpio_reserved(i)) {
			continue;
		}
		switch (c->pin_mode[i]) {
		case 1:                                  /* output */
			gpio_pin_configure(port, off_of(i), GPIO_OUTPUT_INACTIVE);
			break;
		case 2:                                  /* input */
		case 3:                                  /* input + pull-up */
			gpio_pin_configure(port, off_of(i),
				GPIO_INPUT | (c->pin_mode[i] == 3 ? GPIO_PULL_UP : 0));
			gpio_pin_interrupt_configure(port, off_of(i), GPIO_INT_EDGE_BOTH);
			di_unsettled[i] = 0;
			di_pub_level[i] = (uint8_t) gpio_pin_get(port, off_of(i));
			input_mask |= BIT(i);
			break;
		default:                                 /* 0 = leave untouched */
			break;
		}
	}

	/* obs-mirror output (always output, any pin, overrides pin_mode) */
	if (obs_mirror_enabled(c)) {
		int p = obs_mirror_pin(c);
		if (p >= 0 && p < BOX_NPINS && !box_gpio_reserved(p)) {
			gpio_pin_configure(port, off_of(p), GPIO_OUTPUT_INACTIVE);
		}
	}

	/* hardware obs-sync input (raw edge latch, kept out of the DI report path) */
	sync_pin = -1;
	sync_edge[0] = sync_edge[1] = 0;
	if (sync_input_enabled(c)) {
		int p = sync_input_pin(c);
		if (p >= 0 && p < BOX_NPINS && !box_gpio_reserved(p)) {
			gpio_pin_configure(port, off_of(p), GPIO_INPUT);
			gpio_pin_interrupt_configure(port, off_of(p), GPIO_INT_EDGE_BOTH);
			input_mask |= BIT(p);
			sync_pin = p;
		}
	}

	if (input_mask) {
		gpio_init_callback(&di_cb, di_isr, input_mask);
		gpio_add_callback(port, &di_cb);
		cb_added = true;
	}
}

void box_gpio_exec(const box_config_t *c, const gpio_cmd_t *cmd)
{
	ARG_UNUSED(c);
	if (cmd->op == GPIO_OP_NONE) {
		return;
	}
	if (cmd->pin >= BOX_NPINS || box_gpio_reserved(cmd->pin)) {
		return;
	}

	/* ensure output (a bare cmd may precede a mode set) */
	gpio_pin_configure(port, off_of(cmd->pin), GPIO_OUTPUT_INACTIVE);

	if (cmd->op == GPIO_OP_SET) {
		gpio_pin_set(port, off_of(cmd->pin), cmd->value ? 1 : 0);
		return;
	}
	if (cmd->op != GPIO_OP_PULSE || cmd->value == 0) {
		gpio_pin_set(port, off_of(cmd->pin), 0);     /* zero-width no-op */
		return;
	}

	/* PULSE: raise now, drop via a hardware CTIMER match channel */
	gpio_pin_set(port, off_of(cmd->pin), 1);

	int chan = -1;
	for (int ch = 0; ch < pulse_nch; ch++) {
		if (pulse_chan_pin[ch] < 0) { chan = ch; break; }
	}
	if (pulse_ctr && chan >= 0) {
		struct counter_alarm_cfg acfg = {
			.callback  = pulse_expired,
			.ticks     = counter_us_to_ticks(pulse_ctr, cmd->value),
			.user_data = (void *)(intptr_t) cmd->pin,
			.flags     = 0,                       /* relative to now */
		};
		pulse_chan_pin[chan] = (int8_t) cmd->pin;
		if (counter_set_channel_alarm(pulse_ctr, chan, &acfg) != 0) {
			pulse_chan_pin[chan] = -1;
			k_busy_wait(cmd->value);
			gpio_pin_set(port, off_of(cmd->pin), 0);
		}
	} else {                                          /* no channel -> blocking fallback */
		k_busy_wait(cmd->value);
		gpio_pin_set(port, off_of(cmd->pin), 0);
	}
}

int box_gpio_poll_di(const box_config_t *c, box_di_event_t *out)
{
	uint64_t now = now_us();
	for (int i = 0; i < BOX_NPINS; i++) {
		unsigned int k = irq_lock();
		uint8_t  uns   = di_unsettled[i];
		uint64_t last  = di_last_edge_us[i];
		uint64_t first = di_first_edge_us[i];
		irq_unlock(k);

		if (!uns) {
			continue;
		}
		uint64_t win = (uint64_t) c->debounce_ms[i] * 1000u;
		if (now - last < win) {
			continue;                        /* still bouncing */
		}
		uint8_t lvl = (uint8_t) gpio_pin_get(port, off_of(i));
		k = irq_lock();
		di_unsettled[i] = 0;
		irq_unlock(k);
		if (lvl != di_pub_level[i]) {
			di_pub_level[i] = lvl;
			out->pin = (uint8_t) i;
			out->level = lvl;
			out->t_us = first;
			return 1;
		}
	}
	return 0;
}

uint64_t box_gpio_now_us(void)
{
	return now_us();
}

void box_gpio_read_di_levels(const box_config_t *c, uint8_t levels[BOX_NPINS])
{
	for (int i = 0; i < BOX_NPINS; i++) {
		levels[i] = 0;
		/* DI modes only (in / in_pullup); outputs and off read 0. */
		if (c->pin_mode[i] == 2 || c->pin_mode[i] == 3) {
			int raw = gpio_pin_get(port, off_of(i));

			if (raw >= 0) {
				levels[i] = (uint8_t) di_logical(c, i, raw);
			}
		}
	}
}

void box_gpio_obs_mirror(const box_config_t *c, int obs)
{
	if (!obs_mirror_enabled(c)) {
		return;
	}
	int p = obs_mirror_pin(c);
	if (p >= 0 && p < BOX_NPINS && !box_gpio_reserved(p)) {
		gpio_pin_set(port, off_of(p), obs ? 1 : 0);
	}
}
