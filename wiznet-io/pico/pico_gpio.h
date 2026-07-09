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
#include "hardware/sync.h"      /* save_and_disable_interrupts: torn-read-safe sync latch */
#include "pico/time.h"

/* Alarm pool for pulse/sched timing, created by the RT core (core 1) so those
 * IRQs fire there -- the DEFAULT pool interrupts core 0 (whichever core ran
 * runtime init), which would put pulse edges on the UI core. Defined in the
 * main .c; NULL until core 1 creates it (fallbacks below cover that window). */
extern alarm_pool_t *box_alarm_pool;

/* GP28: Eth/USB mode strap, read at every boot (dual build) to pick the transport --
 * open/high = USB (safe default, never touches the W6300), tied to GND = Ethernet.
 * Free on the W6300-EVB -- the analog-in is the external ADS1115 over I2C, so the
 * RP2350's own ADC pins are unused. Overridable at build for a different board. */
#ifndef BOX_MODE_STRAP_PIN
#define BOX_MODE_STRAP_PIN 28
#endif

/* SSD1306 128x32 SPI status display (Adafruit 661-style), runtime-enabled via
 * `oled enable 1` + save/reboot. SPI0 on GP2/3 + three control pins -- see
 * wiznet-io/PINMAP.md for the whole box pin budget. Overridable per board.
 * The five pins are reserved (below) ONLY while the display is enabled, so a
 * display-less box keeps them all for user I/O. pico_oled.h drives them,
 * always from core 0. */
#ifndef OLED_PIN_SCK
#define OLED_PIN_SCK  2      /* SPI0 SCK  -> display CLK          */
#define OLED_PIN_MOSI 3      /* SPI0 TX   -> display DATA         */
#define OLED_PIN_CS   6      /* chip select                       */
#define OLED_PIN_DC   7      /* data/command (display SA0 pin)    */
#define OLED_PIN_RST  8      /* reset                             */
#endif
static uint8_t pico_gpio_oled_claim;   /* set at boot (main, core 0) iff cfg->oled_en */

/* Pins the box firmware must never drive/route:
 *  - W6300 wired build: the W6300 QSPI/INT/CS/RST block (GPIO15-22 on EVB-Pico2)
 *  - dual build:        also the Eth/USB mode strap (BOX_MODE_STRAP_PIN)
 *  - oled enabled:      the 5 display pins (boot-latched claim, like ain_en)
 *  - pico2w WiFi build:  the CYW43 wireless pins (board-specific; taken from the
 *    SDK board header, e.g. 23/24/25/29 on the Pimoroni Pico Plus 2 W) */
static inline int pico_gpio_reserved(int n)
{
    if (pico_gpio_oled_claim &&
        (n == OLED_PIN_SCK || n == OLED_PIN_MOSI || n == OLED_PIN_CS ||
         n == OLED_PIN_DC  || n == OLED_PIN_RST)) return 1;
#ifdef BOX_NET_LWIP
#if defined(CYW43_DEFAULT_PIN_WL_REG_ON)
    if (n == CYW43_DEFAULT_PIN_WL_REG_ON   || n == CYW43_DEFAULT_PIN_WL_CLOCK    ||
        n == CYW43_DEFAULT_PIN_WL_CS        || n == CYW43_DEFAULT_PIN_WL_DATA_OUT ||
        n == CYW43_DEFAULT_PIN_WL_DATA_IN) return 1;
#endif
    return 0;
#else
    if (n >= 15 && n <= 22) return 1;              /* W6300 QSPI block */
#ifdef BOX_NET_DUAL
    if (n == BOX_MODE_STRAP_PIN) return 1;         /* Eth/USB mode strap */
#endif
    return 0;
#endif
}

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

/* ---- hardware obs-sync input: raw edge latch ----
 * The rig host's TTL obs line (ess-2.0.tm begin_obs: rpioPinOn, high = in obs)
 * wired to a box pin. The IRQ latches each edge's box-clock time, unfiltered
 * and unpublished -- deliberately NOT the debounced DI path: this timestamp
 * becomes the clock anchor, paired with the dserv timestamp the ess/in_obs
 * frame delivers later (see on_frame), so transport delay drops out of the
 * sync error budget entirely. */
static volatile int      pico_gpio_sync_pin = -1;       /* -1 = no sync input configured */
static volatile uint64_t pico_gpio_sync_edge[2];        /* [0]=falling(end), [1]=rising(begin) */

/* torn-read-safe snapshot: the IRQ writes the 64-bit latch on this same core */
static inline uint64_t pico_gpio_sync_edge_get(int rising)
{
    uint32_t s = save_and_disable_interrupts();
    uint64_t t = pico_gpio_sync_edge[rising ? 1 : 0];
    restore_interrupts(s);
    return t;
}

static void pico_gpio_irq_cb(uint gpio, uint32_t events)
{
    if ((int) gpio == pico_gpio_sync_pin) {             /* TTL obs-sync: latch, don't report */
        uint64_t t = time_us_64();
        if (events & GPIO_IRQ_EDGE_RISE) pico_gpio_sync_edge[1] = t;
        if (events & GPIO_IRQ_EDGE_FALL) pico_gpio_sync_edge[0] = t;
        return;
    }
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

    /* obs-mirror pin: always an OUTPUT (driven from ess/in_obs), overriding any
     * pin_mode. Enabled via obs_en (any GPIO incl GP0). Refuses reserved pins. */
    if (obs_mirror_enabled(c)) {
        int p = obs_mirror_pin(c);
        if (p >= 0 && p < PICO_NPINS && !pico_gpio_reserved(p)) {
            gpio_init(p); gpio_set_dir(p, GPIO_OUT); gpio_put(p, 0);
            gpio_set_irq_enabled(p, EDGES, false);
        }
    }

    /* hardware obs-sync input: always an INPUT with the raw edge-latch IRQ,
     * overriding any pin_mode (the redirect in pico_gpio_irq_cb keeps its
     * edges out of the DI report path). TTL is actively driven -> no pulls.
     * Claim LAST so the pin loop can't undo the IRQ enable; stale latches are
     * cleared so a pin move can't pair an old edge. */
    pico_gpio_sync_pin = -1;
    pico_gpio_sync_edge[0] = pico_gpio_sync_edge[1] = 0;
    if (sync_input_enabled(c)) {
        int p = sync_input_pin(c);
        if (p >= 0 && p < PICO_NPINS && !pico_gpio_reserved(p)) {
            gpio_init(p); gpio_set_dir(p, GPIO_IN); gpio_disable_pulls(p);
            di_unsettled[p] = 0;                     /* never reports via poll_di */
            gpio_set_irq_enabled_with_callback(p, EDGES, true, pico_gpio_irq_cb);
            pico_gpio_sync_pin = p;
        }
    }
}

/* Drive the obs-mirror pin to the box's live copy of ess/in_obs (no-op if off). */
static inline void pico_gpio_obs_mirror(const pico_config_t *c, int obs)
{
    if (!obs_mirror_enabled(c)) return;
    int p = obs_mirror_pin(c);
    if (p >= 0 && p < PICO_NPINS && !pico_gpio_reserved(p)) gpio_put(p, obs ? 1 : 0);
}

/* Falling edge of a non-blocking DO pulse, fired from a hardware timer alarm
 * (runs in the timer IRQ). user data carries the pin number. */
static int64_t pico_gpio_pulse_end(alarm_id_t id, void *pin)
{ (void) id; gpio_put((uint)(uintptr_t) pin, 0); return 0; }   /* return 0 -> one-shot */

/* Execute a gpio command. SET drives a level; PULSE drives high now and schedules
 * the falling edge on a HARDWARE ALARM (non-blocking -- the superloop is free for
 * the whole pulse; box-timed width so it's immune to host/dserv jitter). Concurrent
 * pulses on different pins are fine (one alarm each). us precision; if you ever
 * need sub-us stim-grade edges, this is the drop-in point for a PIO pulse. */
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
    } else if (cmd->value) { /* GPIO_OP_PULSE: raise now, drop via alarm (non-blocking) */
        gpio_put(cmd->pin, 1);
        alarm_id_t a = box_alarm_pool
            ? alarm_pool_add_alarm_in_us(box_alarm_pool, cmd->value, pico_gpio_pulse_end,
                                         (void *)(uintptr_t) cmd->pin, true)
            : add_alarm_in_us(cmd->value, pico_gpio_pulse_end,
                              (void *)(uintptr_t) cmd->pin, true);
        if (a <= 0) { busy_wait_us(cmd->value); gpio_put(cmd->pin, 0); }   /* pool full -> fallback */
    } else {
        gpio_put(cmd->pin, 0);                                            /* zero width */
    }
}

#endif /* PICO_GPIO_H_DEV */
