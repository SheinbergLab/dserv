/*
 * box_sched.c -- box-scheduled events on per-slot k_timers (see box_sched.h).
 */
#include "box_sched.h"
#include "box_gpio.h"
#include "box_event.h"

#include <zephyr/kernel.h>

typedef enum { SCH_FREE = 0, SCH_ARMED, SCH_FIRED } sch_state_t;

/* Slot lifecycle: FREE -> ARMED (thread, box_sched_arm) -> FIRED (timer ISR)
 * -> FREE (thread, box_sched_poll). One writer per transition, single-core,
 * word-sized state -- the same lock-free shape as box_gpio's di_unsettled. */
typedef struct {
	struct k_timer       timer;
	const box_config_t  *cfg;         /* for box_gpio_exec at fire time */
	volatile sch_state_t st;
	uint8_t              pin;         /* BOX_SCHED_NOTIFY_ONLY = no GPIO */
	uint8_t              tid;
	uint32_t             width_us;
	uint64_t             fire_us;     /* intended instant (box clock) */
} sch_slot_t;

static sch_slot_t slots[BOX_SCHED_MAX];

static void sched_expired(struct k_timer *t)
{
	sch_slot_t *s = CONTAINER_OF(t, sch_slot_t, timer);

	if (s->pin != BOX_SCHED_NOTIFY_ONLY) {
		/* timing-critical half: the pulse, at the intended instant */
		gpio_cmd_t c = { GPIO_OP_PULSE, s->pin, s->width_us };
		box_gpio_exec(s->cfg, &c);
	}
	s->st = SCH_FIRED;
	box_event_signal();     /* wake the loop for the state/timer publish */
}

void box_sched_init(void)
{
	for (int i = 0; i < BOX_SCHED_MAX; i++) {
		k_timer_init(&slots[i].timer, sched_expired, NULL);
	}
}

int box_sched_arm(const box_config_t *c, uint8_t pin, uint8_t tid,
		  uint32_t width_us, uint64_t fire_us)
{
	sch_slot_t *s = NULL;

	for (int i = 0; i < BOX_SCHED_MAX; i++) {
		if (slots[i].st == SCH_FREE) {
			s = &slots[i];
			break;
		}
	}
	if (!s) {
		return -1;
	}
	s->cfg      = c;
	s->pin      = pin;
	s->tid      = tid;
	s->width_us = width_us;
	s->fire_us  = fire_us;
	s->st       = SCH_ARMED;

	uint64_t now = box_gpio_now_us();
	k_timer_start(&s->timer,
		      fire_us > now ? K_USEC(fire_us - now) : K_NO_WAIT,
		      K_NO_WAIT);
	return 0;
}

int box_sched_poll(box_sched_fired_t *out)
{
	for (int i = 0; i < BOX_SCHED_MAX; i++) {
		if (slots[i].st == SCH_FIRED) {
			out->tid     = slots[i].tid;
			out->fire_us = slots[i].fire_us;
			slots[i].st  = SCH_FREE;
			return 1;
		}
	}
	return 0;
}
