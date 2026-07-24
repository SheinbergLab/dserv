/*
 * box_sched.h -- box-scheduled events: fire at an absolute box time.
 *
 * The Zephyr counterpart of the Pico's g_sched[] table (wizchip_dserv_config.c),
 * same wire contract:
 *   cmd/do/<n>/at <us>     pulse pin n (width = config/pin/<n>/pulse_us, default
 *                          1000) at beginobs + <us>, then post state/timer/<n>
 *   cmd/timer/<t>/at <us>  post state/timer/<t> at beginobs + <us> (notify-only)
 *
 * Each slot rides its own k_timer (100 us kernel tick -- the accepted
 * resolution), so slots never contend and nothing blocks. The expiry runs in
 * ISR context: it drives the GPIO pulse at the intended instant and hands the
 * slot to box_sched_poll(), which the service loop drains to publish
 * state/timer/<tid> stamped at the INTENDED fire time (not drain time).
 */
#ifndef BOX_SCHED_H
#define BOX_SCHED_H

#include "dserv_config.h"
#include <stdint.h>

#define BOX_SCHED_MAX          8      /* in-flight events (same as the Pico) */
#define BOX_SCHED_NOTIFY_ONLY  0xFF   /* pin value: no GPIO, just the publish */

/* Init the slot timers. Call once at boot, after box_gpio_init(). */
void box_sched_init(void);

/* Arm one event: at fire_us (absolute box time, box_gpio_now_us clock) drive a
 * pulse of width_us on pin -- or nothing when pin == BOX_SCHED_NOTIFY_ONLY --
 * and queue the state/timer/<tid> publish. A fire_us already in the past fires
 * immediately (still stamped at fire_us: late, and honestly so).
 * Returns 0, or -1 when all slots are armed. */
int box_sched_arm(const box_config_t *c, uint8_t pin, uint8_t tid,
		  uint32_t width_us, uint64_t fire_us);

/* One fired event to publish: state/timer/<tid>, stamped at fire_us. Returns 1
 * and fills *out (freeing the slot), or 0 when none pending. Call repeatedly
 * each service pass until it returns 0. */
typedef struct { uint8_t tid; uint64_t fire_us; } box_sched_fired_t;
int box_sched_poll(box_sched_fired_t *out);

#endif /* BOX_SCHED_H */
