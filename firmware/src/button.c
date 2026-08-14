/*
 * Button debounce and press classification. See button.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "button.h"

#include <string.h>

void button_init(struct button *b)
{
	memset(b, 0, sizeof(*b));
}

struct button_result button_sample(struct button *b, bool pressed, uint32_t now_ms)
{
	struct button_result r = { .event = BUTTON_EVENT_NONE, .ms = 0 };

	if (!b->have_raw) {
		/*
		 * Adopt the first sample without emitting anything. A device that boots with the
		 * button held down must not report a press it never saw begin — and on this board
		 * the pin is shared with the debugger, so the first reading after reset is not
		 * necessarily a user action.
		 */
		b->have_raw = true;
		b->raw = pressed;
		b->stable = pressed;
		b->raw_since_ms = now_ms;
		b->pressed_at_ms = now_ms;
		return r;
	}

	if (pressed != b->raw) {
		/* Raw edge: restart the settling window. */
		if (b->raw != b->stable) {
			/*
			 * The previous edge never settled, so it was chatter rather than a change.
			 * Counted because a switch that chatters more over time is a real symptom.
			 */
			b->chatter++;
		}
		b->raw = pressed;
		b->raw_since_ms = now_ms;
		return r;
	}

	/* Raw is steady. Has it been steady long enough to believe? */
	if (b->raw != b->stable && (uint32_t)(now_ms - b->raw_since_ms) >= BUTTON_DEBOUNCE_MS) {
		b->stable = b->raw;

		if (b->stable) {
			b->pressed_at_ms = now_ms;
			b->long_reported = false;
		} else {
			/*
			 * Released. Report the duration from the debounced press, so the number the
			 * host sees is the settled hold time rather than including chatter.
			 */
			b->presses++;
			r.event = BUTTON_EVENT_PRESS;
			r.ms = (uint32_t)(now_ms - b->pressed_at_ms);
			return r;
		}
	}

	/*
	 * Still held: announce the long threshold as it passes, not on release. Waiting for release
	 * would mean holding the button with no feedback and no way to know whether it took.
	 */
	if (b->stable && !b->long_reported &&
	    (uint32_t)(now_ms - b->pressed_at_ms) >= BUTTON_LONG_MS) {
		b->long_reported = true;
		r.event = BUTTON_EVENT_LONG;
		r.ms = (uint32_t)(now_ms - b->pressed_at_ms);
	}

	return r;
}
