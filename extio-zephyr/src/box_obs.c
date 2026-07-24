/*
 * box_obs.c -- see box_obs.h.
 */
#include "box_obs.h"

static int      obs_active;
static uint32_t deferred;

void box_obs_set(int in_obs)
{
	obs_active = in_obs ? 1 : 0;
}

int box_obs_active(void)
{
	return obs_active;
}

void box_obs_defer(uint32_t what)
{
	deferred |= what;
}

uint32_t box_obs_take_deferred(void)
{
	uint32_t d;

	if (obs_active || deferred == 0) {
		return 0;
	}
	d = deferred;
	deferred = 0;
	return d;
}
