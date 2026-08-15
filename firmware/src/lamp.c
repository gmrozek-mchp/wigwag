/*
 * Lamp animation. See lamp.h.
 *
 * Everything here is integer arithmetic on a monotonic clock, so a frame is a pure function of
 * (state, link, time). That makes the behaviours testable, and it means a dropped or late frame
 * changes nothing — the next one lands where it should rather than continuing from where the last
 * one stopped.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lamp.h"

#include <string.h>

#define LEVEL_MAX 255U

/* Triangle wave over `period`, 0 -> 255 -> 0. */
static uint8_t triangle(uint32_t now_ms, uint32_t period_ms)
{
	uint32_t phase = now_ms % period_ms;
	uint32_t half = period_ms / 2U;

	if (phase < half) {
		return (uint8_t)((phase * LEVEL_MAX) / half);
	}

	return (uint8_t)(((period_ms - phase) * LEVEL_MAX) / half);
}

/* Square wave over `period`: true for the first half. */
static bool square(uint32_t now_ms, uint32_t period_ms)
{
	return (now_ms % period_ms) < (period_ms / 2U);
}

/*
 * The amber flicker. Rule 4 and ADR-0007: when the device cannot confirm what it is showing it must
 * look *obviously wrong*, so this is deliberately not an animation — an irregular stutter across
 * red and yellow with green held off, at a cadence that matches none of the real behaviours.
 *
 * Green is off on purpose. Green means "idle, ready for you", and the one thing the device must
 * never imply while blind is that everything is fine.
 */
static struct lamp_frame flicker(uint32_t now_ms)
{
	static const uint8_t amber_y[] = { 255, 40, 180, 20, 90, 255, 10, 140 };
	static const uint8_t amber_r[] = { 60, 200, 30, 255, 15, 120, 220, 45 };
	const uint32_t slot = (now_ms / LAMP_FLICKER_STEP_MS) %
			      (uint32_t)(sizeof(amber_y) / sizeof(amber_y[0]));
	struct lamp_frame f;

	f.level[LAMP_GREEN] = 0;
	f.level[LAMP_YELLOW] = amber_y[slot];
	f.level[LAMP_RED] = amber_r[slot];

	return f;
}

struct lamp_frame lamp_render(enum wigwag_state state, bool linked, uint32_t now_ms,
			      uint32_t state_since_ms)
{
	struct lamp_frame f;

	if (!linked) {
		return flicker(now_ms);
	}

	memset(&f, 0, sizeof(f));

	switch (state) {
	case WIGWAG_IDLE:
		/* Green, steady and dim: done, ready, and not asking for anything. */
		f.level[LAMP_GREEN] = LAMP_IDLE_DIM;
		break;

	case WIGWAG_BUSY:
		/* Yellow breathing. Working; no action needed. */
		f.level[LAMP_YELLOW] = triangle(now_ms, LAMP_BUSY_PERIOD_MS);
		break;

	case WIGWAG_WAIT:
		/*
		 * Red, steady — then a slow blink once it has been waiting long enough that you
		 * probably have not noticed. The escalation is the whole point of the state: it is
		 * the only one that wants your attention rather than merely reporting.
		 */
		if ((uint32_t)(now_ms - state_since_ms) >= LAMP_WAIT_ESCALATE_MS) {
			f.level[LAMP_RED] = square(now_ms, LAMP_WAIT_BLINK_MS) ? LEVEL_MAX : 0U;
		} else {
			f.level[LAMP_RED] = LEVEL_MAX;
		}
		break;

	case WIGWAG_ERROR:
		/* Red and yellow alternating fast: the turn died, and that is not a normal state. */
		if (square(now_ms, LAMP_ERROR_PERIOD_MS)) {
			f.level[LAMP_RED] = LEVEL_MAX;
		} else {
			f.level[LAMP_YELLOW] = LEVEL_MAX;
		}
		break;

	default:
		/*
		 * Unreachable if the parser did its job. Falling back to the flicker rather than to
		 * darkness or green keeps an impossible value visible instead of plausible.
		 */
		return flicker(now_ms);
	}

	return f;
}

/*
 * The state vocabulary, in exactly one place.
 *
 * Two carriers reach it by different routes — MQTT delivers a JSON payload, the console delivers a
 * bare word (ADR-0018, D104) — and both must agree on what the four legal states are spelled like.
 * Two tables would be two chances to drift, so there is one table and two entry points.
 */
static const struct state_name {
	const char *name;
	enum wigwag_state state;
} state_names[] = {
	{ "IDLE", WIGWAG_IDLE },
	{ "BUSY", WIGWAG_BUSY },
	{ "WAIT", WIGWAG_WAIT },
	{ "ERROR", WIGWAG_ERROR },
};

#define N_STATE_NAMES (sizeof(state_names) / sizeof(state_names[0]))

bool wigwag_state_parse_word(const char *word, enum wigwag_state *out)
{
	size_t i;

	if (word == NULL || out == NULL) {
		return false;
	}

	/*
	 * Exact match, uppercase only. `CONTEXT.md` and the conventions both say the states are
	 * uppercase with no synonyms, and a console that accepted `busy` would soon be asked to accept
	 * `Busy`, then `working`.
	 */
	for (i = 0; i < N_STATE_NAMES; i++) {
		if (strcmp(word, state_names[i].name) == 0) {
			*out = state_names[i].state;
			return true;
		}
	}

	return false;
}

bool wigwag_state_parse(const char *payload, enum wigwag_state *out)
{
	const char *p;
	size_t i;

	if (payload == NULL || out == NULL) {
		return false;
	}

	/*
	 * Find the "state" key rather than assuming field order, but do not pretend to parse JSON:
	 * a 64 KB part does not need a parser to read one enum out of a message this project also
	 * writes. Matching the key first is what stops "reason":"WAIT for input" from being read as
	 * a state.
	 */
	p = strstr(payload, "\"state\"");
	if (p == NULL) {
		return false;
	}

	p = strchr(p + 7, ':');
	if (p == NULL) {
		return false;
	}

	/* Skip whitespace and the opening quote. */
	p++;
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (*p == '"') {
		p++;
	}

	for (i = 0; i < N_STATE_NAMES; i++) {
		size_t n = strlen(state_names[i].name);

		if (strncmp(p, state_names[i].name, n) == 0) {
			/*
			 * Require a terminator, so "ERRORISH" is rejected rather than read as ERROR.
			 */
			char after = p[n];

			if (after == '"' || after == '\0' || after == ',' || after == '}' ||
			    after == ' ') {
				*out = state_names[i].state;
				return true;
			}
		}
	}

	return false;
}

uint32_t lamp_gamma_pulse(uint8_t level, uint32_t period)
{
	uint32_t v = period;

	/*
	 * period * (level/255)^3, scaled in three steps so every intermediate stays large and
	 * inside 32 bits. The obvious form — (level^3 / 255^2) then scale — throws the low end away:
	 * level^3 / 65025 truncates to zero below level 41, so **every level under 41 rendered as
	 * fully off** and 41..48 all produced the same duty. Found on hardware as "the dim green lamp
	 * does not light".
	 *
	 * Each step multiplies by at most 255 before dividing, so the worst case is
	 * 255 * 2e6 = 5.1e8, comfortably under 2^32. No 64-bit helpers are pulled in.
	 */
	v = (v / 255U) * level + ((v % 255U) * level) / 255U;
	v = (v / 255U) * level + ((v % 255U) * level) / 255U;
	v = (v / 255U) * level + ((v % 255U) * level) / 255U;

	return v;
}

uint8_t lamp_scale(uint8_t level, uint8_t gain, uint8_t brightness, bool fault)
{
	uint32_t master = brightness;
	uint32_t v;

	if (fault && master < LAMP_FAULT_MIN_BRIGHTNESS) {
		master = LAMP_FAULT_MIN_BRIGHTNESS;
	}

	/* Calibration first, then preference. Both stay inside 16 bits before each divide. */
	v = ((uint32_t)level * gain) / LEVEL_MAX;
	v = (v * master) / LEVEL_MAX;

	return (uint8_t)v;
}

bool lamp_brightness_parse(const char *payload, uint8_t *out)
{
	uint32_t v = 0;
	int digits = 0;

	if (payload == NULL || out == NULL) {
		return false;
	}

	while (*payload == ' ' || *payload == '\t') {
		payload++;
	}

	for (; *payload >= '0' && *payload <= '9'; payload++) {
		v = (v * 10U) + (uint32_t)(*payload - '0');
		digits++;
		if (v > LEVEL_MAX) {
			return false;	/* out of range, and cannot recover by reading further */
		}
	}

	/* Trailing whitespace or a newline is fine; anything else is not a number. */
	while (*payload == ' ' || *payload == '\t' || *payload == '\r' || *payload == '\n') {
		payload++;
	}

	if (digits == 0 || *payload != '\0') {
		return false;
	}

	*out = (uint8_t)v;
	return true;
}

const char *wigwag_state_str(enum wigwag_state state)
{
	switch (state) {
	case WIGWAG_IDLE:
		return "IDLE";
	case WIGWAG_BUSY:
		return "BUSY";
	case WIGWAG_WAIT:
		return "WAIT";
	case WIGWAG_ERROR:
		return "ERROR";
	default:
		return "?";
	}
}
