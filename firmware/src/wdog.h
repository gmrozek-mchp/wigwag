/*
 * Watchdog liveness accounting — pure logic, no Zephyr, no watchdog peripheral.
 *
 * The watchdog exists because link.c cannot supervise itself. It watches the module, the broker and
 * the host, but if the AT loop wedges then supervision wedges with it and the lamps freeze on a
 * confident, stale display — the exact failure Rule 4 and ADR-0007 forbid, arrived at from the
 * inside instead of the outside.
 *
 * The point of this file is that **feeding must be earned, not automatic**. A watchdog fed from one
 * loop only proves that loop is alive: the render thread could be dead with the lamps frozen on a
 * stale state, and the watchdog would happily keep the device running. So every task that must be
 * alive for the display to be trustworthy checks in here, and the feeder refuses to feed unless all
 * of them are recent.
 *
 * Thread safety: each task writes only its own slot, one aligned 32-bit word, and the reader only
 * compares. That is safe without a lock on this architecture, and a lock would be worse — it would
 * let a wedged task hold one.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WDOG_H
#define WDOG_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Tasks whose liveness the display depends on.
 *
 * Adding one here without also making it check in will stop the device feeding, which is the safe
 * direction to fail: a device that reboots is better than one that lies.
 */
enum wdog_task {
	WDOG_TASK_AT = 0,	/* the AT service loop: module, link supervision, publishes */
	WDOG_TASK_LAMP,		/* the render thread: without it the lamps are frozen */
	WDOG_TASK_COUNT,
};

/*
 * How stale a check-in may be before that task counts as dead.
 *
 * Both tasks run every 10 ms, so 500 ms is fifty missed turns — far beyond jitter, and comfortably
 * past the ~24 ms the AT loop can block transmitting a full-length command. Detection is this plus
 * the hardware timeout, which stays well inside D34's 10 s budget.
 */
#define WDOG_MAX_AGE_MS 500U

void wdog_init(uint32_t now_ms);

/** Check in. Called by each task from its own loop. */
void wdog_beat(enum wdog_task task, uint32_t now_ms);

/** True only if every task has checked in recently enough. */
bool wdog_all_alive(uint32_t now_ms);

/** Which task is the most overdue, and by how long. Diagnostics for the console. */
enum wdog_task wdog_stalest(uint32_t now_ms, uint32_t *age_ms);

const char *wdog_task_str(enum wdog_task task);

#endif /* WDOG_H */
