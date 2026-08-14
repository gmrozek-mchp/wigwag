/*
 * Host unit tests for button debounce and press classification.
 *
 * Debounce is the classic place for off-by-one bugs, and the failure mode on hardware is a button
 * that "sometimes double-reports" — hard to reproduce, easy to assert here.
 *
 *   make -C firmware/tests button
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../src/button.h"

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

static struct button b;

/*
 * Feed a steady level for `ms`, sampling every 10 ms as the real loop does, collecting events.
 *
 * Counts iterations rather than comparing timestamps against an end time. The obvious
 * `end = now + ms; while (now < end)` is not wrap-safe — at 0xFFFFFFFA + 100 the end wraps below
 * the start and the loop never runs, which is precisely the bug
 * test_clock_wrap_is_survivable() exists to catch, and the first version of this helper had it.
 */
static struct button_result hold(bool pressed, uint32_t ms, uint32_t *now)
{
	struct button_result last = { .event = BUTTON_EVENT_NONE, .ms = 0 };
	uint32_t steps = ms / 10U;
	uint32_t i;

	for (i = 0; i < steps; i++) {
		struct button_result r = button_sample(&b, pressed, *now);

		if (r.event != BUTTON_EVENT_NONE) {
			last = r;
		}
		*now += 10;
	}

	return last;
}

static void test_first_sample_is_adopted_silently(void)
{
	uint32_t now = 1000;
	struct button_result r;

	/* Booting with the button already down must not manufacture a press. */
	button_init(&b);
	r = button_sample(&b, true, now);

	CHECK(r.event == BUTTON_EVENT_NONE, "first sample emitted an event");
	CHECK(button_is_down(&b), "first sample did not adopt the level");
	CHECK(b.presses == 0, "presses counted on adoption");
}

static void test_short_press_reports_duration(void)
{
	uint32_t now = 0;
	struct button_result r;

	button_init(&b);
	hold(false, 100, &now);

	hold(true, 150, &now);
	r = hold(false, 100, &now);

	CHECK(r.event == BUTTON_EVENT_PRESS, "no press event on release");
	/*
	 * Duration is measured from the debounced press to the debounced release, so it lags the
	 * raw edges by one debounce window at each end. Allow a sampling period of slack either way
	 * rather than asserting an exact figure.
	 */
	CHECK(r.ms >= 120 && r.ms <= 180, "duration %u ms for a ~150 ms press", r.ms);
	CHECK(b.presses == 1, "presses = %u", b.presses);
}

static void test_chatter_does_not_produce_a_press(void)
{
	uint32_t now = 0;
	int events = 0;
	int i;

	button_init(&b);
	hold(false, 100, &now);

	/* 5 ms of alternating samples: never steady long enough to settle. */
	for (i = 0; i < 20; i++) {
		struct button_result r = button_sample(&b, (i % 2) == 0, now);

		if (r.event != BUTTON_EVENT_NONE) {
			events++;
		}
		now += 5;
	}

	CHECK(events == 0, "chatter produced %d events", events);
	CHECK(b.presses == 0, "chatter counted %u presses", b.presses);
	CHECK(b.chatter > 0, "chatter was not counted");
}

static void test_bounce_on_press_still_yields_one_press(void)
{
	uint32_t now = 0;
	struct button_result r;
	int presses = 0;

	button_init(&b);
	hold(false, 100, &now);

	/* A real switch: a few milliseconds of bounce, then a solid press. */
	button_sample(&b, true, now);  now += 3;
	button_sample(&b, false, now); now += 4;
	button_sample(&b, true, now);  now += 2;
	button_sample(&b, false, now); now += 3;

	{
		uint32_t end = now + 200;

		while (now < end) {
			r = button_sample(&b, true, now);
			if (r.event == BUTTON_EVENT_PRESS) {
				presses++;
			}
			now += 10;
		}
	}
	r = hold(false, 100, &now);

	CHECK(r.event == BUTTON_EVENT_PRESS, "no press after a bouncy edge");
	CHECK(presses == 0, "a press was emitted while still held (%d)", presses);
	CHECK(b.presses == 1, "bouncy press counted %u times", b.presses);
}

static void test_long_hold_fires_once_while_held(void)
{
	uint32_t now = 0;
	int longs = 0;
	uint32_t end;

	button_init(&b);
	hold(false, 100, &now);

	end = now + BUTTON_LONG_MS + 2000U;
	while (now < end) {
		struct button_result r = button_sample(&b, true, now);

		if (r.event == BUTTON_EVENT_LONG) {
			longs++;
			CHECK(r.ms >= BUTTON_LONG_MS, "long fired early at %u ms", r.ms);
		}
		now += 10;
	}

	CHECK(longs == 1, "long fired %d times for one hold", longs);
	CHECK(button_is_down(&b), "should still be held");
}

static void test_long_hold_still_reports_the_release(void)
{
	/*
	 * D35: the host gets every press with its true duration regardless of what the device did
	 * locally. A hold that triggered provisioning must still publish, not be swallowed.
	 */
	uint32_t now = 0;
	struct button_result r;

	button_init(&b);
	hold(false, 100, &now);
	hold(true, BUTTON_LONG_MS + 500U, &now);
	r = hold(false, 100, &now);

	CHECK(r.event == BUTTON_EVENT_PRESS, "long hold swallowed the release");
	CHECK(r.ms >= BUTTON_LONG_MS, "release reported %u ms after a long hold", r.ms);
}

static void test_short_press_never_fires_long(void)
{
	uint32_t now = 0;
	int longs = 0;
	uint32_t end;

	button_init(&b);
	hold(false, 100, &now);

	end = now + BUTTON_LONG_MS - 500U;
	while (now < end) {
		if (button_sample(&b, true, now).event == BUTTON_EVENT_LONG) {
			longs++;
		}
		now += 10;
	}
	hold(false, 100, &now);

	CHECK(longs == 0, "a sub-threshold hold fired long %d times", longs);
}

static void test_two_presses_are_independent(void)
{
	uint32_t now = 0;
	struct button_result a, c;

	button_init(&b);
	hold(false, 100, &now);
	hold(true, 120, &now);
	a = hold(false, 200, &now);
	hold(true, 300, &now);
	c = hold(false, 100, &now);

	CHECK(a.event == BUTTON_EVENT_PRESS && c.event == BUTTON_EVENT_PRESS, "missed a press");
	CHECK(c.ms > a.ms, "second press (%u ms) not longer than first (%u ms)", c.ms, a.ms);
	CHECK(b.presses == 2, "counted %u presses", b.presses);
}

static void test_clock_wrap_is_survivable(void)
{
	/* uint32 milliseconds wrap after ~49 days; a press across the boundary must still work. */
	uint32_t now = 0xFFFFFF00U;
	struct button_result r;

	button_init(&b);
	hold(false, 100, &now);   /* wraps during this */
	hold(true, 150, &now);
	r = hold(false, 100, &now);

	CHECK(r.event == BUTTON_EVENT_PRESS, "no press across the clock wrap");
	CHECK(r.ms >= 120 && r.ms <= 180, "duration %u ms across the wrap", r.ms);
}

int main(void)
{
	printf("button host tests\n");

	test_first_sample_is_adopted_silently();
	test_short_press_reports_duration();
	test_chatter_does_not_produce_a_press();
	test_bounce_on_press_still_yields_one_press();
	test_long_hold_fires_once_while_held();
	test_long_hold_still_reports_the_release();
	test_short_press_never_fires_long();
	test_two_presses_are_independent();
	test_clock_wrap_is_survivable();

	printf("%d checks, %d failures\n", checks, failures);
	return (failures == 0) ? 0 : 1;
}
