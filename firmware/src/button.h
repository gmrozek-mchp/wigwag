/*
 * Button debounce and press classification — pure logic, no Zephyr, no GPIO.
 *
 * Takes raw samples plus a timestamp and produces settled events. Sampled rather than
 * interrupt-driven: a press lasts ~100 ms so 10 ms sampling is invisible, debounce needs tens of
 * milliseconds of settling regardless, and PL10's devicetree has no EIC node so an interrupt would
 * mean enabling a whole interrupt controller to save nothing (D86).
 *
 * Two consumers, deliberately separate:
 *   - every settled press is published raw with its duration, and the host decides what it means
 *     (D35, CONTEXT.md's `wigwag/button` topic);
 *   - a long hold is reported locally *while still held*, which is what will enter provisioning
 *     mode (D58) — you need to know it registered before you let go.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BUTTON_H
#define BUTTON_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 20 ms of settling. Tactile switches chatter for a few milliseconds; 20 ms is comfortably past
 * that and still far below the ~100 ms floor of a deliberate human press, so no real press is lost.
 */
#define BUTTON_DEBOUNCE_MS 20U

/* 3 s to enter provisioning mode — long enough that it cannot be hit by accident (D58). */
#define BUTTON_LONG_MS 3000U

enum button_event {
	BUTTON_EVENT_NONE = 0,
	/** Settled release. `ms` carries how long it was held. */
	BUTTON_EVENT_PRESS,
	/** The long-hold threshold passed while still held. Fires once per press. */
	BUTTON_EVENT_LONG,
};

struct button_result {
	enum button_event event;
	uint32_t ms;
};

struct button {
	bool raw;		/* last raw sample */
	bool stable;		/* debounced state: true = pressed */
	bool long_reported;	/* the long event has already fired for this press */
	bool have_raw;		/* a first sample has been seen */
	uint32_t raw_since_ms;	/* when the raw sample last changed */
	uint32_t pressed_at_ms;	/* when the debounced state became pressed */

	uint32_t presses;	/* diagnostics */
	uint32_t chatter;	/* raw changes rejected by debounce */
};

void button_init(struct button *b);

/**
 * Feed one sample.
 *
 * @param pressed raw reading, already corrected for active-low wiring
 * @param now_ms  monotonic milliseconds
 *
 * Returns at most one event per call. A press that crosses the long threshold produces
 * BUTTON_EVENT_LONG while held and BUTTON_EVENT_PRESS on release, in that order — the release
 * still reports its true duration, because the host is entitled to the raw fact (D35).
 */
struct button_result button_sample(struct button *b, bool pressed, uint32_t now_ms);

static inline bool button_is_down(const struct button *b)
{
	return b->stable;
}

#endif /* BUTTON_H */
