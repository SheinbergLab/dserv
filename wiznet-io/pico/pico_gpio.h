/*
 * pico_gpio.h -- device GPIO layer: turn pico_config_t + gpio_cmd_t into real
 * RP2350 pin operations. The simulator has print-only stubs of the same two
 * calls, so the dispatch logic is identical on both.
 *
 * W6300-EVB-Pico2: GPIO15..22 belong to the W6300 -- refused here as a guard.
 */
#ifndef PICO_GPIO_H_DEV
#define PICO_GPIO_H_DEV

#include "dserv_config.h"
#include "hardware/gpio.h"
#include "pico/time.h"

/* W6300 QSPI/INT/CS/RST pins on the EVB-Pico2 */
static inline int pico_gpio_reserved(int n)
{ return n >= 15 && n <= 22; }

/* ---- DI edge capture with per-pin debounce ----
 * The IRQ just records edge times per pin (cheap). The main loop reports a
 * transition only after the line has been QUIET for debounce_ms (like the
 * Linux GPIO debounce), reads the settled level, and timestamps it at the
 * FIRST edge (the press/release moment). debounce_ms=0 -> report on next poll
 * (still collapses same-loop glitches, since we read the settled level once).
 */
typedef struct { uint8_t pin; uint8_t level; uint64_t t_us; } di_event_t;

static volatile uint32_t di_last_edge_us[PICO_NPINS];   /* 32-bit -> atomic read for settle math */
static volatile uint64_t di_first_edge_us[PICO_NPINS];  /* press moment -> event timestamp        */
static volatile uint8_t  di_unsettled[PICO_NPINS];
static uint8_t           di_pub_level[PICO_NPINS];       /* last published (main loop only)        */

static void pico_gpio_irq_cb(uint gpio, uint32_t events)
{
    (void) events;
    if (gpio >= PICO_NPINS) return;
    if (!di_unsettled[gpio]) { di_first_edge_us[gpio] = time_us_64(); di_unsettled[gpio] = 1; }
    di_last_edge_us[gpio] = time_us_32();               /* moving quiet-since marker */
}

/* Report one settled (debounced) input transition. Returns 1 if *out filled;
 * call repeatedly each loop until it returns 0. */
static inline int pico_gpio_poll_di(const pico_config_t *c, di_event_t *out)
{
    uint32_t now = time_us_32();
    for (int i = 0; i < PICO_NPINS; i++) {
        if (!di_unsettled[i]) continue;
        uint32_t win = (uint32_t) c->debounce_ms[i] * 1000u;      /* ms -> us */
        if ((uint32_t)(now - di_last_edge_us[i]) < win) continue; /* still bouncing */
        uint64_t fe  = di_first_edge_us[i];                       /* snapshot before clearing */
        uint8_t  lvl = (uint8_t) gpio_get(i);                     /* the settled level */
        di_unsettled[i] = 0;
        if (lvl != di_pub_level[i]) {
            di_pub_level[i] = lvl;
            out->pin = (uint8_t) i; out->level = lvl; out->t_us = fe;
            return 1;
        }
    }
    return 0;
}

/* (Re)initialize all pins to their configured modes. Idempotent; call at boot
 * and after any pin/<n>/mode change. */
static inline void pico_gpio_apply_config(const pico_config_t *c)
{
    const uint32_t EDGES = GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL;
    for (int i = 0; i < PICO_NPINS; i++) {
        if (pico_gpio_reserved(i)) continue;
        switch (c->pin_mode[i]) {
        case 1: gpio_init(i); gpio_set_dir(i, GPIO_OUT); gpio_put(i, 0);
                gpio_set_irq_enabled(i, EDGES, false); break;
        case 2: gpio_init(i); gpio_set_dir(i, GPIO_IN); gpio_disable_pulls(i);
                busy_wait_us(20); di_unsettled[i] = 0; di_pub_level[i] = (uint8_t) gpio_get(i);
                gpio_set_irq_enabled_with_callback(i, EDGES, true, pico_gpio_irq_cb); break;
        case 3: gpio_init(i); gpio_set_dir(i, GPIO_IN); gpio_pull_up(i);
                busy_wait_us(20); di_unsettled[i] = 0; di_pub_level[i] = (uint8_t) gpio_get(i);
                gpio_set_irq_enabled_with_callback(i, EDGES, true, pico_gpio_irq_cb); break;
        default: gpio_set_irq_enabled(i, EDGES, false); break;   /* 0 = off */
        }
    }
}

/* Execute a gpio command. SET drives a level; PULSE drives high for value us
 * then low (box-timed via busy_wait -- deterministic width regardless of host/
 * dserv jitter). PULSE blocks the loop for its duration: fine for short DO
 * pulses; use a timer/alarm for long ones. */
static inline void pico_gpio_exec(const pico_config_t *c, const gpio_cmd_t *cmd)
{
    (void) c;
    if (cmd->op == GPIO_OP_NONE) return;
    if (pico_gpio_reserved(cmd->pin) || cmd->pin >= PICO_NPINS) return;

    /* ensure the pin is an output (a bare gpio command may precede a mode set) */
    gpio_init(cmd->pin);
    gpio_set_dir(cmd->pin, GPIO_OUT);

    if (cmd->op == GPIO_OP_SET) {
        gpio_put(cmd->pin, cmd->value ? 1 : 0);
    } else { /* GPIO_OP_PULSE */
        gpio_put(cmd->pin, 1);
        busy_wait_us(cmd->value);
        gpio_put(cmd->pin, 0);
    }
}

#endif /* PICO_GPIO_H_DEV */
