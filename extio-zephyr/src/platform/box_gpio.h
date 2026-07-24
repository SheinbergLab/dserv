/*
 * box_gpio.h -- RW612/Zephyr GPIO platform layer.
 *
 * Turns box_config_t + gpio_cmd_t into real pin operations, the same seam the
 * bare-metal wiznet-io/pico/pico_gpio.h fills on the RP2350 -- but implemented
 * with Zephyr/NXP idioms rather than transliterated:
 *   - pins are resolved through devicetree (hsgpio0), not flat SDK numbers;
 *   - the non-blocking box-timed pulse drops its falling edge from a per-pin
 *     k_timer (100 us kernel tick), the portable analog of the Pico alarm
 *     pool. NOT the Zephyr `counter` API: the mcux GPT driver arms alarms
 *     without clearing a stale compare flag and defaults to restart mode, so
 *     alarms fire instantly -- see PORTING.md;
 *   - DI edges are captured with a gpio_callback and timestamped from the
 *     high-resolution cycle counter.
 *
 * The wire contract is unchanged: pins are still addressed by the flat index the
 * host sets via config/pin/<n>/mode and cmd/do/<n>. box pin n maps to hsgpio0.n
 * (so pin 12 = the FRDM user LED, pin 11 = User SW2); a devicetree overlay can
 * remap or extend onto hsgpio1 without touching the protocol.
 */
#ifndef BOX_GPIO_H
#define BOX_GPIO_H

#include "dserv_config.h"
#include <stdint.h>

/* Resolve the GPIO port device and init the per-pin pulse timers.
 * Returns 0 on success, negative on a missing/!ready device. Call once at boot. */
int box_gpio_init(void);

/* (Re)configure every pin from cfg: output / input / input+pullup, plus the DI
 * edge interrupts, the obs-mirror output, and the hardware obs-sync input.
 * Idempotent -- call at boot and after any pin/<n>/mode change. Pins the host
 * has not configured (mode 0) are left untouched, so board console/peripheral
 * pins are never disturbed unless explicitly claimed. */
void box_gpio_apply_config(const box_config_t *c);

/* Execute one gpio command. SET drives a level now (and cancels the pin's
 * pending pulse falling edge, so a stale timer can never clobber it); PULSE
 * drives high now and schedules the falling edge on the pin's own k_timer
 * (non-blocking, box-timed width immune to host/dserv jitter, 100 us tick
 * resolution). Every pin can pulse concurrently; nothing ever blocks.
 * ISR-safe for configured output pins (box_sched fires pulses from a timer). */
void box_gpio_exec(const box_config_t *c, const gpio_cmd_t *cmd);

/* One settled (debounced) DI transition, timestamped at the first edge (the
 * press/release moment). Returns 1 and fills *out, or 0 when none pending.
 * Call repeatedly each service pass until it returns 0. */
typedef struct { uint8_t pin; uint8_t level; uint64_t t_us; } box_di_event_t;
int box_gpio_poll_di(const box_config_t *c, box_di_event_t *out);

/* Drive the obs-mirror output to the box's live copy of ess/in_obs (no-op off). */
void box_gpio_obs_mirror(const box_config_t *c, int obs);

/* The box's monotonic microsecond clock -- the SAME source that stamps
 * box_di_event_t.t_us, so callers can compare against DI event times (the DI
 * chord-settle windows in box_group.h need exactly that). */
uint64_t box_gpio_now_us(void);

/* Current LOGICAL level of each configured DI pin, indexed by box pin number
 * (active_low already applied); non-DI pins read 0. Used to seed the DI group
 * state machines so a switch already held at boot is not reported as an edge. */
void box_gpio_read_di_levels(const box_config_t *c, uint8_t levels[BOX_NPINS]);

/* Latched hardware obs-sync edge time in microseconds (box clock).
 * rising=1 -> begin_obs edge, rising=0 -> end_obs edge. 0 if none seen. */
uint64_t box_gpio_sync_edge_us(int rising);

#endif /* BOX_GPIO_H */
