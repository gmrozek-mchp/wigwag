/*
 * Host unit tests for watchdog liveness accounting.
 *
 * The behaviour under test is a refusal: the device must stop feeding when any task it depends on
 * goes quiet. Getting this wrong in the safe direction causes spurious reboots; getting it wrong in
 * the unsafe direction leaves a watchdog that guards nothing while the lamps show a stale state.
 *
 *   make -C firmware/tests wdog
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../src/wdog.h"

#include <stdio.h>

static int failures;
static int checks;

#define CHECK(cond, ...)                                                                           \
	do {                                                                                       \
		checks++;                                                                          \
		if (!(cond)) {                                                                     \
			failures++;                                                                \
			printf("  FAIL %s:%d: ", __func__, __LINE__);                              \
			printf(__VA_ARGS__);                                                       \
			printf("\n");                                                              \
		}                                                                                  \
	} while (0)

static void beat_all(uint32_t now)
{
	wdog_beat(WDOG_TASK_AT, now);
	wdog_beat(WDOG_TASK_LAMP, now);
}

static void test_starts_alive(void)
{
	/* Zero-initialised timestamps would look ancient and reboot the device before it ran. */
	wdog_init(50000);

	CHECK(wdog_all_alive(50000), "not alive immediately after init");
	CHECK(wdog_all_alive(50000 + WDOG_MAX_AGE_MS), "not alive at exactly the age limit");
}

static void test_all_beating_stays_alive(void)
{
	uint32_t t;

	wdog_init(0);
	for (t = 0; t < 60000; t += 10) {
		beat_all(t);
		CHECK(wdog_all_alive(t) || t == 0, "went dead while both tasks were beating at %u", t);
	}
}

static void test_one_silent_task_stops_the_feed(void)
{
	/*
	 * The case that matters. The AT loop keeps running - so a watchdog fed from there alone
	 * would be fed forever - while the render thread has died and the lamps are frozen.
	 */
	uint32_t t;

	wdog_init(0);
	beat_all(0);

	for (t = 10; t <= WDOG_MAX_AGE_MS; t += 10) {
		wdog_beat(WDOG_TASK_AT, t);
		CHECK(wdog_all_alive(t), "gave up too early at %u ms", t);
	}

	wdog_beat(WDOG_TASK_AT, WDOG_MAX_AGE_MS + 10U);
	CHECK(!wdog_all_alive(WDOG_MAX_AGE_MS + 10U),
	      "still feeding with the lamp thread silent for %u ms", WDOG_MAX_AGE_MS + 10U);
}

static void test_either_task_can_be_the_dead_one(void)
{
	wdog_init(0);
	beat_all(0);
	wdog_beat(WDOG_TASK_LAMP, 5000);
	CHECK(!wdog_all_alive(5000), "a silent AT loop did not stop the feed");

	wdog_init(0);
	beat_all(0);
	wdog_beat(WDOG_TASK_AT, 5000);
	CHECK(!wdog_all_alive(5000), "a silent lamp thread did not stop the feed");
}

static void test_recovery_resumes_feeding(void)
{
	/* A task that is merely late, not dead, must be able to come back. */
	wdog_init(0);
	beat_all(0);

	CHECK(!wdog_all_alive(WDOG_MAX_AGE_MS + 100U), "precondition: should be stale");
	beat_all(WDOG_MAX_AGE_MS + 100U);
	CHECK(wdog_all_alive(WDOG_MAX_AGE_MS + 100U), "did not recover after both tasks beat");
}

static void test_stalest_identifies_the_culprit(void)
{
	uint32_t age = 0;
	enum wdog_task who;

	wdog_init(1000);
	wdog_beat(WDOG_TASK_AT, 4000);
	wdog_beat(WDOG_TASK_LAMP, 2000);

	who = wdog_stalest(5000, &age);
	CHECK(who == WDOG_TASK_LAMP, "blamed %s instead of lamp", wdog_task_str(who));
	CHECK(age == 3000, "reported age %u, expected 3000", age);
}

static void test_clock_wrap_is_survivable(void)
{
	/* Milliseconds wrap after ~49 days; the device must not reboot when they do. */
	uint32_t base = 0xFFFFFF00U;
	uint32_t i;

	wdog_init(base);
	for (i = 0; i < 100; i++) {
		uint32_t t = base + (i * 10U);	/* wraps partway through */

		beat_all(t);
		CHECK(wdog_all_alive(t), "went dead across the clock wrap at offset %u", i * 10U);
	}
}

int main(void)
{
	printf("wdog host tests\n");

	test_starts_alive();
	test_all_beating_stays_alive();
	test_one_silent_task_stops_the_feed();
	test_either_task_can_be_the_dead_one();
	test_recovery_resumes_feeding();
	test_stalest_identifies_the_culprit();
	test_clock_wrap_is_survivable();

	printf("%d checks, %d failures\n", checks, failures);
	return (failures == 0) ? 0 : 1;
}
