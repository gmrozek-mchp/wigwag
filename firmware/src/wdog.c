/*
 * Watchdog liveness accounting. See wdog.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wdog.h"

#include <stddef.h>

static volatile uint32_t last_beat_ms[WDOG_TASK_COUNT];

void wdog_init(uint32_t now_ms)
{
	unsigned i;

	/*
	 * Start every task "just seen" rather than at zero. Zero would look ancient and stop the
	 * device feeding before a single task had a chance to run.
	 */
	for (i = 0; i < WDOG_TASK_COUNT; i++) {
		last_beat_ms[i] = now_ms;
	}
}

void wdog_beat(enum wdog_task task, uint32_t now_ms)
{
	if (task < WDOG_TASK_COUNT) {
		last_beat_ms[task] = now_ms;
	}
}

bool wdog_all_alive(uint32_t now_ms)
{
	unsigned i;

	for (i = 0; i < WDOG_TASK_COUNT; i++) {
		/* Wrap-safe: the difference is correct across the 32-bit millisecond rollover. */
		if ((uint32_t)(now_ms - last_beat_ms[i]) > WDOG_MAX_AGE_MS) {
			return false;
		}
	}

	return true;
}

enum wdog_task wdog_stalest(uint32_t now_ms, uint32_t *age_ms)
{
	enum wdog_task worst = WDOG_TASK_AT;
	uint32_t worst_age = 0;
	unsigned i;

	for (i = 0; i < WDOG_TASK_COUNT; i++) {
		uint32_t age = (uint32_t)(now_ms - last_beat_ms[i]);

		if (age >= worst_age) {
			worst_age = age;
			worst = (enum wdog_task)i;
		}
	}

	if (age_ms != NULL) {
		*age_ms = worst_age;
	}

	return worst;
}

const char *wdog_task_str(enum wdog_task task)
{
	switch (task) {
	case WDOG_TASK_AT:
		return "at";
	case WDOG_TASK_LAMP:
		return "lamp";
	default:
		return "?";
	}
}
