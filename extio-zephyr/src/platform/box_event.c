/*
 * box_event.c -- service-loop wakeup semaphore (see box_event.h).
 */
#include "box_event.h"

/* Binary: many signals before the loop runs collapse into one wakeup, which is
 * exactly what we want -- the loop drains everything pending each pass. */
static K_SEM_DEFINE(box_evt, 0, 1);

void box_event_signal(void)
{
	k_sem_give(&box_evt);
}

void box_event_wait(k_timeout_t timeout)
{
	(void) k_sem_take(&box_evt, timeout);
}
