/*
 * Zephyr watchdog binding: the hardware half of wdog.c.
 *
 * Two jobs, and the second is the one that will actually be used. It arms the WDT and feeds it only
 * when wdog.c says every task is alive — and it reports, at boot, whether the last reset came from
 * the watchdog. Without that report a device whose only output is three lamps reboots silently, and
 * a reboot loop looks exactly like a slow connection.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WDOG_WDT_H
#define WDOG_WDT_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Print why the device last reset, and clear the cause so the next boot's report means something.
 *
 * Call before arming, and before anything that might itself reset: on a watchdog reboot this line is
 * the only evidence that survives.
 */
void wdog_report_reset_cause(void);

/**
 * Arm the watchdog and start requiring proof of life.
 *
 * Returns 0 once armed, or a negative errno. **A failure here is not fatal**: the device runs
 * unguarded rather than not at all, which is the right trade for a status light — but it says so on
 * the console, because an unguarded device that looks guarded is worse than either.
 *
 * Must be called after every task in enum wdog_task exists and is beating, and after any long
 * blocking init (the lamp selftest holds the CPU for 1.6 s, which would trip a 2 s window).
 */
int wdog_wdt_init(uint32_t now_ms);

/**
 * Feed the watchdog, but only if every task has checked in recently.
 *
 * Called from the AT loop. The refusal is the entire mechanism: this function existing on the AT
 * loop's thread does not mean the AT loop's aliveness is what keeps the device up.
 */
void wdog_wdt_service(uint32_t now_ms);

#endif /* WDOG_WDT_H */
