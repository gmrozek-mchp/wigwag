/*
 * Zephyr watchdog binding. See wdog_wdt.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wdog_wdt.h"

#include "wdog.h"

#include <soc.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/printk.h>

/*
 * 2000 ms, which the driver rounds to the nearest power of two of its 1.024 kHz clock: CYC2048,
 * 2.0 s (wdt_mchp_g1.c wdt_get_period_idx()). Worst-case detection is WDOG_MAX_AGE_MS plus this,
 * so 2.5 s — comfortably inside D34's 10 s "device must not show a stale state" budget, and far
 * enough above the 10 ms loop period that no ordinary jitter can reach it.
 *
 * Normal mode, not windowed: a minimum window catches a task that runs too *fast*, which is not a
 * failure this device has. It would also make the feed itself capable of causing a reset, and a
 * watchdog that can kill a healthy device is a liability rather than a safeguard.
 */
#define WDOG_TIMEOUT_MS 2000U

static const struct device *wdt = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));
static bool armed;

/*
 * RSTC.RCAUSE, read directly rather than through hwinfo — deliberately, because on this family the
 * hwinfo driver gives a confidently wrong answer.
 *
 * hwinfo_mchp_g1.c was written against the JH revision of RSTC, where RCAUSE is an 8-bit register at
 * offset 0x00. On PL10 it is 32-bit at offset **0x04** (0x00 is CTRLA), and the flags sit one bit
 * lower: EXT 3, WDT 4, SYST 5, with LOCKUP at 6 where JH has SYST. Datasheet §16.6.2, cross-checked
 * against the pack header's RSTC_RCAUSE_*_Pos. So the driver reads CTRLA — which returns 0, the
 * symptom that sent me looking — and would misreport a watchdog reset as RESET_PIN even at the right
 * address. Filed in docs/upstreaming-to-zephyr.md.
 *
 * Nothing is cleared afterwards: §16.6.2 says each reset sets its own bit and writes all others to
 * 0, so the register is self-maintaining and read-only. hwinfo_clear_reset_cause() has no
 * implementation here for exactly that reason.
 *
 * The address comes from the devicetree node so there is still one source of truth for it, even
 * though that node is deliberately left disabled.
 */
#define RCAUSE (((rstc_registers_t *)DT_REG_ADDR(DT_NODELABEL(rstc)))->RSTC_RCAUSE)

void wdog_report_reset_cause(void)
{
	uint32_t rcause = RCAUSE;

	/*
	 * Loud for the watchdog, because that is the one that means this firmware has a bug, and a
	 * silent reboot on a device whose only output is three lamps is indistinguishable from a slow
	 * connection.
	 */
	if ((rcause & RSTC_RCAUSE_WDT_Msk) != 0U) {
		printk("wigwag: RESET BY WATCHDOG (rcause %x)\n", rcause);
	} else if ((rcause & (RSTC_RCAUSE_LOCKUP_Msk | RSTC_RCAUSE_BORVDD_Msk)) != 0U) {
		/* A CPU lockup or a sagging supply: also this device's problem, also worth saying. */
		printk("wigwag: reset by %s (rcause %x)\n",
		       ((rcause & RSTC_RCAUSE_LOCKUP_Msk) != 0U) ? "lockup" : "brownout", rcause);
	}

	/*
	 * Silent for POR, EXT and SYST — power-on, the reset button and the debugger. Those are the
	 * ordinary ways this board starts, and a line on every boot would train the eye to skip the
	 * one line here that matters.
	 */
}

int wdog_wdt_init(uint32_t now_ms)
{
	static const struct wdt_timeout_cfg timeout = {
		.window = {
			.min = 0,
			.max = WDOG_TIMEOUT_MS,
		},
		/*
		 * No callback: reset directly. A callback would run in interrupt context on a device
		 * we have already decided is unreliable, to print a message on a UART that may be the
		 * thing that wedged. The reset cause register carries the same information, safely.
		 */
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	int ret;

	wdog_init(now_ms);

	if (wdt == NULL || !device_is_ready(wdt)) {
		printk("wigwag: no watchdog, running unguarded\n");
		return -ENODEV;
	}

	ret = wdt_install_timeout(wdt, &timeout);
	if (ret < 0) {
		printk("wigwag: watchdog install failed (%d), running unguarded\n", ret);
		return ret;
	}

	/*
	 * WDT_OPT_PAUSE_HALTED_BY_DBG: this part is halted by the debugger constantly during
	 * development, and a watchdog that reboots the device every time a breakpoint is hit makes
	 * debugging impossible. The PL10's WDT does this by default — the driver accepts the option
	 * and applies nothing (wdt_mchp_g1.c wdt_apply_options()) — so asking for it costs nothing
	 * and documents the requirement.
	 */
	ret = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (ret < 0) {
		printk("wigwag: watchdog setup failed (%d), running unguarded\n", ret);
		return ret;
	}

	armed = true;
	printk("wigwag: watchdog armed, %u ms, %u tasks must beat within %u ms\n", WDOG_TIMEOUT_MS,
	       (unsigned)WDOG_TASK_COUNT, WDOG_MAX_AGE_MS);

	return 0;
}

void wdog_wdt_service(uint32_t now_ms)
{
	if (!armed) {
		return;
	}

	if (wdog_all_alive(now_ms)) {
		(void)wdt_feed(wdt, 0);
		return;
	}

	/*
	 * Deliberately not fed. Say so once per stall — the reset that follows is about 1.5 s away,
	 * and this line plus the reset-cause line on the next boot is the whole diagnostic trail.
	 * Rate-limited because a stall that resolves itself would otherwise flood a 115200 console
	 * with one line every 10 ms, and printk from this loop is not free.
	 */
	{
		static uint32_t last_gripe_ms;
		static bool griped;
		uint32_t age = 0;
		enum wdog_task who = wdog_stalest(now_ms, &age);

		if (!griped || (uint32_t)(now_ms - last_gripe_ms) > 250U) {
			printk("wigwag: NOT FEEDING, %s task stale %u ms\n", wdog_task_str(who), age);
			last_gripe_ms = now_ms;
			griped = true;
		}
	}
}
