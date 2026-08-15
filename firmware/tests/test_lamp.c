/*
 * Host unit tests for lamp animation and state parsing.
 *
 * These encode CONTEXT.md's lamp table as assertions, so the behaviours are pinned before any of it
 * is judged by eye on hardware — an inverted or mistimed lamp looks plausible, which is how the
 * polarity bug survived earlier in this phase.
 *
 *   make -C firmware/tests lamp
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../src/lamp.h"

#include <stdio.h>
#include <string.h>

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

/* --------------------------------------------------------------- behaviours */

static void test_idle_is_green_only_and_dim(void)
{
	struct lamp_frame f = lamp_render(WIGWAG_IDLE, true, 1000, 0);

	CHECK(f.level[LAMP_GREEN] == LAMP_IDLE_DIM, "green %u", f.level[LAMP_GREEN]);
	CHECK(f.level[LAMP_YELLOW] == 0, "yellow lit in IDLE: %u", f.level[LAMP_YELLOW]);
	CHECK(f.level[LAMP_RED] == 0, "red lit in IDLE: %u", f.level[LAMP_RED]);
	/*
	 * "Dim" means clearly less than the states that want attention, which sit at full. Asserted
	 * on the rendered duty rather than the perceptual level: the level is on a cube-root scale,
	 * so a number that looks like "half" is nothing of the sort. An earlier version of this check
	 * used `level < 128` and would have rejected the value later chosen by eye.
	 */
	CHECK(f.level[LAMP_GREEN] < 255, "IDLE green is at full brightness");
	CHECK(lamp_gamma_pulse(f.level[LAMP_GREEN], 2000000) < 2000000U / 4U,
	      "IDLE green duty is not dim: %u ns of 2000000",
	      lamp_gamma_pulse(f.level[LAMP_GREEN], 2000000));

	/* Steady: the same at any time. */
	CHECK(lamp_render(WIGWAG_IDLE, true, 99999, 0).level[LAMP_GREEN] == LAMP_IDLE_DIM,
	      "IDLE green is not steady");
}

static void test_busy_breathes_yellow_only(void)
{
	uint8_t min = 255, max = 0;
	uint32_t t;

	for (t = 0; t < LAMP_BUSY_PERIOD_MS; t += 10) {
		struct lamp_frame f = lamp_render(WIGWAG_BUSY, true, t, 0);

		if (f.level[LAMP_YELLOW] < min) {
			min = f.level[LAMP_YELLOW];
		}
		if (f.level[LAMP_YELLOW] > max) {
			max = f.level[LAMP_YELLOW];
		}
		CHECK(f.level[LAMP_GREEN] == 0 && f.level[LAMP_RED] == 0,
		      "BUSY lit another lamp at t=%u", t);
	}

	CHECK(min < 16, "BUSY never gets dark (min %u)", min);
	CHECK(max > 240, "BUSY never gets bright (max %u)", max);

	/* One cycle per period: the level at t and t+period must match. */
	CHECK(lamp_render(WIGWAG_BUSY, true, 300, 0).level[LAMP_YELLOW] ==
	      lamp_render(WIGWAG_BUSY, true, 300 + LAMP_BUSY_PERIOD_MS, 0).level[LAMP_YELLOW],
	      "BUSY is not periodic at its stated rate");
}

static void test_wait_is_steady_red_then_blinks(void)
{
	uint32_t t;
	bool saw_off = false;

	/* Before the escalation: solid red, every frame. */
	for (t = 0; t < LAMP_WAIT_ESCALATE_MS; t += 250) {
		struct lamp_frame f = lamp_render(WIGWAG_WAIT, true, t, 0);

		CHECK(f.level[LAMP_RED] == 255, "WAIT red not solid at t=%u: %u", t,
		      f.level[LAMP_RED]);
		CHECK(f.level[LAMP_GREEN] == 0 && f.level[LAMP_YELLOW] == 0,
		      "WAIT lit another lamp at t=%u", t);
	}

	/* After it: blinking, so some frames must be dark. */
	for (t = LAMP_WAIT_ESCALATE_MS; t < LAMP_WAIT_ESCALATE_MS + 4000U; t += 100) {
		if (lamp_render(WIGWAG_WAIT, true, t, 0).level[LAMP_RED] == 0) {
			saw_off = true;
		}
	}

	CHECK(saw_off, "WAIT never escalated to a blink after %u ms", LAMP_WAIT_ESCALATE_MS);
}

static void test_wait_escalation_is_relative_to_the_state(void)
{
	/*
	 * The 30 s runs from when WAIT was adopted, not from boot. A device that has been up for an
	 * hour and has just been asked a question must show steady red, not a blink.
	 */
	struct lamp_frame f = lamp_render(WIGWAG_WAIT, true, 3600000, 3600000);

	CHECK(f.level[LAMP_RED] == 255, "a fresh WAIT on an old device blinked immediately");
}

static void test_error_is_both_lamps_steady(void)
{
	/*
	 * Two lamps at once happens in no other state, and being static it cannot be confused with the
	 * fail-visible wigwag or with WAIT's slow red blink — the pair that matters most, since one
	 * means "it needs you" and this means "it already died".
	 */
	uint32_t t;

	for (t = 0; t < 5000; t += 10) {
		struct lamp_frame f = lamp_render(WIGWAG_ERROR, true, t, 0);

		CHECK(f.level[LAMP_RED] == 255, "ERROR red not full at t=%u (%u)", t, f.level[LAMP_RED]);
		CHECK(f.level[LAMP_YELLOW] == 255, "ERROR yellow not full at t=%u (%u)", t,
		      f.level[LAMP_YELLOW]);
		CHECK(f.level[LAMP_GREEN] == 0, "ERROR lit green at t=%u", t);
	}
}

/* ------------------------------------------------------------ fail-visible */

static void test_unlinked_overrides_every_state(void)
{
	/*
	 * Compared over a cycle rather than frame by frame, deliberately.
	 *
	 * A two-lamp alternation shows exactly one lamp at any given instant, so a single frame of the
	 * wigwag's red half is identical to steady WAIT, and its yellow half to BUSY at the peak of the
	 * breathe. That is inherent to alternating and not a fault: what distinguishes fail-visible is
	 * *motion*, over a second, which is how anyone actually looks at a lamp on a desk. The previous
	 * pattern kept both lamps lit at irregular levels and so differed at every instant — the price
	 * was that it read as two lamps flickering at random rather than as a signal.
	 */
	const enum wigwag_state states[] = { WIGWAG_IDLE, WIGWAG_BUSY, WIGWAG_WAIT, WIGWAG_ERROR };
	size_t i;

	for (i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
		bool differs = false;
		uint32_t t;

		for (t = 0; t < LAMP_WIGWAG_PERIOD_MS * 2U; t += 10) {
			struct lamp_frame f = lamp_render(states[i], false, t, 0);
			struct lamp_frame trusted = lamp_render(states[i], true, t, 0);

			CHECK(f.level[LAMP_GREEN] == 0, "green lit while unlinked in %s at t=%u",
			      wigwag_state_str(states[i]), t);

			if (memcmp(&f, &trusted, sizeof(f)) != 0) {
				differs = true;
			}
		}

		CHECK(differs, "unlinked is indistinguishable from %s across a whole cycle",
		      wigwag_state_str(states[i]));
	}
}

static void test_unlinked_wigwags_between_red_and_yellow(void)
{
	/*
	 * The specific lie to prevent: a device that cannot confirm anything must never resemble
	 * "everything is fine". Green stays off throughout, exactly one of red/yellow is lit at any
	 * moment, and both are seen across a cycle.
	 */
	bool saw_red = false, saw_yellow = false;
	uint32_t t;

	for (t = 0; t < LAMP_WIGWAG_PERIOD_MS * 3U; t += 10) {
		struct lamp_frame f = lamp_render(WIGWAG_IDLE, false, t, 0);

		CHECK(f.level[LAMP_GREEN] == 0, "green lit while unlinked at t=%u", t);

		/* Exactly one, never both — that is what separates it from ERROR. */
		CHECK((f.level[LAMP_RED] > 0) != (f.level[LAMP_YELLOW] > 0),
		      "wigwag lit %s at t=%u",
		      (f.level[LAMP_RED] > 0) ? "both" : "neither", t);

		if (f.level[LAMP_RED] > 0) {
			saw_red = true;
		}
		if (f.level[LAMP_YELLOW] > 0) {
			saw_yellow = true;
		}
	}

	CHECK(saw_red && saw_yellow, "did not alternate (red %d yellow %d)", saw_red, saw_yellow);
}

static void test_wigwag_is_never_mistakable_for_error(void)
{
	/* Different meanings, so they must not be able to look the same at any instant. */
	uint32_t t;

	for (t = 0; t < LAMP_WIGWAG_PERIOD_MS * 2U; t += 10) {
		struct lamp_frame fail = lamp_render(WIGWAG_IDLE, false, t, 0);
		struct lamp_frame err = lamp_render(WIGWAG_ERROR, true, t, 0);

		CHECK(memcmp(&fail, &err, sizeof(fail)) != 0,
		      "fail-visible and ERROR identical at t=%u", t);
	}
}

static void test_wigwag_is_not_the_busy_breathe(void)
{
	/* If the flicker resembled BUSY it would read as "working", which is a lie, not a warning. */
	int same = 0;
	uint32_t t;

	for (t = 0; t < LAMP_BUSY_PERIOD_MS; t += 10) {
		if (lamp_render(WIGWAG_BUSY, false, t, 0).level[LAMP_YELLOW] ==
		    lamp_render(WIGWAG_BUSY, true, t, 0).level[LAMP_YELLOW]) {
			same++;
		}
	}

	CHECK(same < 20, "flicker tracks the breathe too closely (%d matching frames)", same);
}

/* ------------------------------------------------------------------ gamma */

static void test_gamma_endpoints_and_monotonicity(void)
{
	const uint32_t period = 2000000;
	uint32_t prev = 0;
	unsigned l;

	CHECK(lamp_gamma_pulse(0, period) == 0, "level 0 is not off: %u",
	      lamp_gamma_pulse(0, period));
	CHECK(lamp_gamma_pulse(255, period) == period, "level 255 is not full: %u",
	      lamp_gamma_pulse(255, period));

	for (l = 0; l <= 255; l++) {
		uint32_t p = lamp_gamma_pulse((uint8_t)l, period);

		CHECK(p >= prev, "not monotonic at level %u (%u then %u)", l, prev, p);
		CHECK(p <= period, "level %u overflowed the period: %u", l, p);
		prev = p;
	}
}

static void test_gamma_has_no_dead_zone(void)
{
	/*
	 * The bug this exists to prevent: computing level^3/255^2 first truncates to zero below
	 * level 41, so every dim level rendered as fully off — found on hardware as "the dim green
	 * lamp does not light". Anything the animation can ask for above a couple of counts must
	 * produce *some* light.
	 */
	const uint32_t period = 2000000;
	unsigned l;

	for (l = 3; l <= 255; l++) {
		CHECK(lamp_gamma_pulse((uint8_t)l, period) > 0,
		      "level %u produces no output at all", l);
	}

	/* And IDLE's dim green in particular, since that is the level that failed. */
	CHECK(lamp_gamma_pulse(LAMP_IDLE_DIM, period) > 0, "IDLE green renders as off");
}

static void test_gamma_is_perceptual_not_linear(void)
{
	/* Half perceptual level should be far less than half duty, or the curve is not doing its job. */
	const uint32_t period = 2000000;
	uint32_t half = lamp_gamma_pulse(128, period);

	CHECK(half < period / 4U, "gamma looks linear: level 128 gave %u of %u", half, period);
	CHECK(half > period / 100U, "gamma is too aggressive: level 128 gave %u of %u", half,
	      period);
}

/* -------------------------------------------------- brightness and gain */

static void test_brightness_is_perceptual_not_linear_in_duty(void)
{
	/*
	 * Halving brightness must look like half, which means halving the *level*. Scaling the duty
	 * instead would land near 79 % apparent brightness, because gamma cubes.
	 */
	CHECK(lamp_scale(255, 255, 128, false) == 128, "half brightness gave level %u",
	      lamp_scale(255, 255, 128, false));
	CHECK(lamp_scale(255, 255, 255, false) == 255, "full brightness altered the level");
	CHECK(lamp_scale(128, 255, 255, false) == 128, "unity gain altered the level");
}

static void test_gain_calibrates_per_lamp(void)
{
	/* A lamp that reads too bright gets its gain lowered; the level drops proportionally. */
	CHECK(lamp_scale(255, 128, 255, false) == 128, "gain 128 gave %u",
	      lamp_scale(255, 128, 255, false));
	CHECK(lamp_scale(255, 0, 255, false) == 0, "gain 0 did not turn the lamp off");

	/* Gain and brightness compose. */
	CHECK(lamp_scale(255, 128, 128, false) == 64, "gain*brightness gave %u",
	      lamp_scale(255, 128, 128, false));
}

static void test_brightness_zero_darkens_reporting_lamps(void)
{
	CHECK(lamp_scale(255, 255, 0, false) == 0, "brightness 0 left a reporting lamp lit");
	CHECK(lamp_scale(LAMP_IDLE_DIM, 255, 0, false) == 0, "brightness 0 left IDLE green lit");
}

static void test_brightness_cannot_silence_the_fault_pattern(void)
{
	/*
	 * Rule 4 / ADR-0007. Brightness is a preference about lamps that report; the flicker is the
	 * device admitting it does not know. Dimmed to nothing it would be indistinguishable from a
	 * device that is switched off, which is the silent lie the design exists to prevent.
	 */
	unsigned b;

	for (b = 0; b < LAMP_FAULT_MIN_BRIGHTNESS; b++) {
		uint8_t out = lamp_scale(255, 255, (uint8_t)b, true);

		CHECK(out >= LAMP_FAULT_MIN_BRIGHTNESS,
		      "fault pattern dimmed to %u at brightness %u", out, b);
	}

	/* Above the floor it tracks the request like anything else. */
	CHECK(lamp_scale(255, 255, 200, true) == 200, "fault pattern ignored a brightness above the floor");
}

static void test_brightness_parse_accepts_plain_decimals(void)
{
	struct { const char *in; uint8_t expect; } ok[] = {
		{ "0", 0 }, { "1", 1 }, { "128", 128 }, { "255", 255 },
		{ " 64", 64 }, { "64\n", 64 }, { "64\r\n", 64 }, { "007", 7 },
	};
	size_t i;

	for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
		uint8_t got = 99;

		CHECK(lamp_brightness_parse(ok[i].in, &got), "rejected \"%s\"", ok[i].in);
		CHECK(got == ok[i].expect, "\"%s\" parsed as %u", ok[i].in, got);
	}
}

static void test_brightness_parse_rejects_rather_than_guesses(void)
{
	const char *bad[] = { "", " ", "256", "999", "-1", "abc", "12x", "1.5",
			      "{\"brightness\":50}", "0x40" };
	size_t i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		uint8_t got = 42;

		CHECK(!lamp_brightness_parse(bad[i], &got), "accepted \"%s\" as %u", bad[i], got);
		CHECK(got == 42, "\"%s\" clobbered the previous value", bad[i]);
	}
}

/* ---------------------------------------------------------------- parsing */

static void test_parse_accepts_real_payloads(void)
{
	struct {
		const char *payload;
		enum wigwag_state expect;
	} cases[] = {
		{ "{\"state\":\"IDLE\",\"reason\":\"Stop\",\"sessions\":1}", WIGWAG_IDLE },
		{ "{\"state\":\"BUSY\",\"reason\":\"PreToolUse\",\"sessions\":2}", WIGWAG_BUSY },
		{ "{\"state\":\"WAIT\",\"reason\":\"permission_prompt\",\"sessions\":2}",
		  WIGWAG_WAIT },
		{ "{\"state\":\"ERROR\",\"reason\":\"StopFailure\",\"sessions\":1}", WIGWAG_ERROR },
		{ "{ \"state\" : \"BUSY\" }", WIGWAG_BUSY },
		{ "{\"sessions\":3,\"state\":\"WAIT\"}", WIGWAG_WAIT },
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		enum wigwag_state got;

		CHECK(wigwag_state_parse(cases[i].payload, &got), "rejected \"%s\"",
		      cases[i].payload);
		CHECK(got == cases[i].expect, "\"%s\" parsed as %s", cases[i].payload,
		      wigwag_state_str(got));
	}
}

static void test_parse_rejects_rather_than_guesses(void)
{
	const char *bad[] = {
		"",
		"{}",
		"not json",
		"{\"state\":\"SLEEPY\"}",
		"{\"state\":\"ERRORISH\"}",
		"{\"reason\":\"WAIT for permission\"}",	/* the value must belong to "state" */
		"{\"state\":}",
		"{\"states\":\"IDLE\"}",
	};
	size_t i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		enum wigwag_state got = WIGWAG_ERROR;

		CHECK(!wigwag_state_parse(bad[i], &got), "accepted \"%s\" as %s", bad[i],
		      wigwag_state_str(got));
	}
}

static void test_state_parse_word(void)
{
	enum wigwag_state st;

	/* The console path: a bare word, not JSON. */
	CHECK(wigwag_state_parse_word("IDLE", &st) && st == WIGWAG_IDLE, "word IDLE");
	CHECK(wigwag_state_parse_word("BUSY", &st) && st == WIGWAG_BUSY, "word BUSY");
	CHECK(wigwag_state_parse_word("WAIT", &st) && st == WIGWAG_WAIT, "word WAIT");
	CHECK(wigwag_state_parse_word("ERROR", &st) && st == WIGWAG_ERROR, "word ERROR");

	/* Uppercase, exact, no synonyms - the conventions say so and this is where it is enforced. */
	CHECK(!wigwag_state_parse_word("busy", &st), "lowercase refused");
	CHECK(!wigwag_state_parse_word("Busy", &st), "mixed case refused");
	CHECK(!wigwag_state_parse_word("ERRORISH", &st), "prefix match refused");
	CHECK(!wigwag_state_parse_word("BUS", &st), "truncation refused");
	CHECK(!wigwag_state_parse_word("", &st), "empty refused");
	CHECK(!wigwag_state_parse_word(NULL, &st), "NULL refused");
	CHECK(!wigwag_state_parse_word("BUSY ", &st), "trailing space refused (caller must tokenise)");

	/*
	 * The two entry points must never disagree. The JSON one accepts what the word one accepts,
	 * wrapped; nothing the word one rejects should sneak through as a bare JSON value.
	 */
	{
		static const char *const names[] = { "IDLE", "BUSY", "WAIT", "ERROR" };
		size_t i;

		for (i = 0; i < 4; i++) {
			char json[64];
			enum wigwag_state a, b;

			snprintf(json, sizeof(json), "{\"state\":\"%s\"}", names[i]);
			CHECK(wigwag_state_parse_word(names[i], &a) &&
			      wigwag_state_parse(json, &b) && a == b,
			      "%s agrees between word and JSON entry points", names[i]);
		}
	}
}

int main(void)
{
	printf("lamp host tests\n");

	test_state_parse_word();

	test_idle_is_green_only_and_dim();
	test_busy_breathes_yellow_only();
	test_wait_is_steady_red_then_blinks();
	test_wait_escalation_is_relative_to_the_state();
	test_error_is_both_lamps_steady();
	test_unlinked_overrides_every_state();
	test_unlinked_wigwags_between_red_and_yellow();
	test_wigwag_is_never_mistakable_for_error();
	test_wigwag_is_not_the_busy_breathe();
	test_brightness_is_perceptual_not_linear_in_duty();
	test_gain_calibrates_per_lamp();
	test_brightness_zero_darkens_reporting_lamps();
	test_brightness_cannot_silence_the_fault_pattern();
	test_brightness_parse_accepts_plain_decimals();
	test_brightness_parse_rejects_rather_than_guesses();
	test_gamma_endpoints_and_monotonicity();
	test_gamma_has_no_dead_zone();
	test_gamma_is_perceptual_not_linear();
	test_parse_accepts_real_payloads();
	test_parse_rejects_rather_than_guesses();

	printf("%d checks, %d failures\n", checks, failures);
	return (failures == 0) ? 0 : 1;
}
