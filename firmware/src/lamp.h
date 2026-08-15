/*
 * Lamp animation — pure logic, no Zephyr, no PWM.
 *
 * Turns (state, link condition, time) into three perceptual brightness levels. The Zephyr side
 * (lamp_pwm.c) applies gamma correction and per-lamp polarity and pushes them at the hardware, the
 * same split as rnwf_at.c against rnwf_uart.c — so every behaviour in CONTEXT.md's table can be
 * tested on the host without a board.
 *
 * Levels here are *perceptual*: 0 is off, 255 is full, and equal steps look like equal steps.
 * Gamma belongs to the renderer, not to the animation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LAMP_H
#define LAMP_H

#include <stdbool.h>
#include <stdint.h>

/** The four states, exactly as CONTEXT.md names them. There is no UNKNOWN — that is a link condition. */
enum wigwag_state {
	WIGWAG_IDLE = 0,
	WIGWAG_BUSY,
	WIGWAG_WAIT,
	WIGWAG_ERROR,
};

enum lamp_id {
	LAMP_GREEN = 0,
	LAMP_YELLOW,
	LAMP_RED,
	LAMP_COUNT,
};

struct lamp_frame {
	uint8_t level[LAMP_COUNT];
};

/*
 * Behaviour constants, from CONTEXT.md's table. Named rather than inline so the numbers can be
 * argued about in one place.
 */
/*
 * "steady, dim" — present, not attention-seeking. 128 renders as 12.6 % duty through
 * lamp_gamma_pulse(), chosen by eye on hardware after sweeping 48/64/80/96/128/160.
 *
 * Note the bench understates the product: the Curiosity Nano drives an LED through a series
 * resistor at a few mA, while the PCB drives 10 mm diffused lamps from the 5 V rail through FETs at
 * 20-60 mA (D26, ADR-0009). The same duty will be several times brighter there, so this is a
 * candidate for revisiting once real lamps exist — and `wigwag/brightness` (CONTEXT.md) is the
 * proper place for per-desk trimming rather than this constant.
 */
#define LAMP_IDLE_DIM		128
#define LAMP_BUSY_PERIOD_MS	1250	/* breathing ~0.8 Hz */
#define LAMP_WAIT_ESCALATE_MS	30000	/* steady, then slow blink after 30 s */
#define LAMP_WAIT_BLINK_MS	2000	/* the slow blink, 0.5 Hz */
/*
 * Fail-visible: red and yellow alternating at 1 Hz — a wigwag, which is the railroad crossing signal
 * this project is named after and reads as "do not proceed" to anyone who sees it.
 *
 * This used to blend both lamps at irregular levels to simulate a flickering amber, on the theory that
 * no legitimate state is arrhythmic. **There is no amber lamp** — three discrete, physically separated
 * lamps cannot mix a colour — so on real hardware it read as two lamps flickering at random rather
 * than as a signal. A clean alternation is unmistakable, and it is distinguished from every state
 * below by being the only one that lights two lamps in sequence.
 */
#define LAMP_WIGWAG_PERIOD_MS	1000	/* full cycle: 500 ms red, 500 ms yellow */

/*
 * Floor applied to the fail-visible pattern, whatever the runtime brightness says.
 *
 * Rule 4 / ADR-0007: brightness is a preference about lamps that *report*. The flicker is not a
 * report, it is the device admitting it does not know - and a device dimmed to nothing would be
 * indistinguishable from one that is switched off, which is the silent lie the whole design exists
 * to prevent. So brightness can dim it, but not below this. A smoke alarm you cannot turn down to
 * zero.
 */
#define LAMP_FAULT_MIN_BRIGHTNESS 96

/**
 * Render one frame.
 *
 * @param state          the aggregate state last received from the host
 * @param linked         the link condition; false overrides everything with the fail-visible wigwag
 * @param now_ms         a monotonic millisecond clock, for animation phase
 * @param state_since_ms when @p state was adopted, for the WAIT escalation
 */
struct lamp_frame lamp_render(enum wigwag_state state, bool linked, uint32_t now_ms,
			      uint32_t state_since_ms);

/**
 * Extract the state from a `wigwag/state` payload.
 *
 * Deliberately not a JSON parser: it looks for the `"state"` key's value and matches one of four
 * literals. Returns false if the payload does not clearly say one of them — and the caller must
 * then keep whatever it had rather than guess, because inventing a state is how a lamp lies.
 */
bool wigwag_state_parse(const char *payload, enum wigwag_state *out);

/**
 * Match a bare state word: `BUSY`, not `{"state":"BUSY"}`.
 *
 * The console carries states as bare words (ADR-0018, D104) while MQTT carries JSON, and both share
 * one vocabulary table — see lamp.c. Exact and uppercase: the conventions say four states, uppercase,
 * no synonyms, and this is where that is enforced for the typed path.
 */
bool wigwag_state_parse_word(const char *word, enum wigwag_state *out);

const char *wigwag_state_str(enum wigwag_state state);

/**
 * Perceptual level -> PWM pulse width, in the same units as @p period.
 *
 * The eye's response to luminance is roughly a cube root, so this cubes the level to cancel it: a
 * linear ramp in level is *seen* as a linear ramp in brightness. Lives here, with the tested logic,
 * rather than in the renderer — it is pure integer arithmetic, and the first version of it sat on
 * the Zephyr side where no test could reach it and shipped a bug for exactly that reason.
 */
uint32_t lamp_gamma_pulse(uint8_t level, uint32_t period);

/**
 * Apply per-lamp calibration and runtime brightness to a perceptual level.
 *
 * Both scales act on the *perceptual* level rather than the duty, so halving brightness looks like
 * half. Scaling the duty instead would look like about 79 %, because gamma cubes.
 *
 * @param gain       per-lamp calibration from devicetree, 0-255 (255 = unchanged)
 * @param brightness runtime master, 0-255, from `wigwag/brightness`
 * @param fault      true when showing the fail-visible pattern; enforces
 *                   LAMP_FAULT_MIN_BRIGHTNESS so the device cannot be dimmed into silence
 */
uint8_t lamp_scale(uint8_t level, uint8_t gain, uint8_t brightness, bool fault);

/**
 * Parse a `wigwag/brightness` payload: a bare decimal 0-255, per CONTEXT.md — not JSON.
 *
 * Returns false on anything else, including out of range, and the caller must then keep the value
 * it had. Guessing a brightness from a malformed message is a smaller lie than guessing a state,
 * but it is the same kind.
 */
bool lamp_brightness_parse(const char *payload, uint8_t *out);

#endif /* LAMP_H */
