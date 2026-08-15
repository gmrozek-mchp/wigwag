/*
 * Zephyr renderer for the lamps: the PWM and threading half of lamp.c.
 *
 * Owns a thread so a long AT transmission cannot stutter an animation. uart_poll_out() blocks its
 * caller for up to ~24 ms on a full-length command (rnwf_uart.c), which is two and a half frames —
 * visible as a hitch in the breathe. Rendering therefore runs at a cooperative priority above the
 * AT loop.
 *
 * Gamma correction and per-lamp polarity live here rather than in lamp.c, so the animation stays a
 * pure function of time and the hardware's quirks stay at the edge.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LAMP_PWM_H
#define LAMP_PWM_H

#include "lamp.h"

/** Bind the three lamps and start the render thread. Returns 0, or -ENODEV if a lamp is missing. */
int lamp_pwm_init(void);

/**
 * Adopt a new state. Timestamped internally, because WAIT's escalation is measured from when the
 * state arrived, not from boot.
 *
 * Idempotent: re-setting the current state does not restart the escalation timer, so a stream of
 * repeated BUSY heartbeats cannot hold WAIT off forever.
 */
void lamp_pwm_set_state(enum wigwag_state state);

/**
 * Set the runtime master brightness, 0-255, from `wigwag/brightness`.
 *
 * Scales what the lamps report. It cannot silence the fail-visible pattern, which floors at
 * LAMP_FAULT_MIN_BRIGHTNESS — see lamp.h.
 */
void lamp_pwm_set_brightness(uint8_t brightness);

/** Update the link condition. False overrides every state with the fail-visible pattern. */
void lamp_pwm_set_link(bool trusted);

/**
 * Set one lamp's calibration gain, 0-255.
 *
 * Devicetree supplies the starting values (D91) and remains the record of what a *board* needs, but
 * calibration is judged by eye on an assembled unit — which is impossible if changing it requires a
 * rebuild. The console sets this live and the settings store remembers it (D37/D56).
 */
void lamp_pwm_set_gain(enum lamp_id lamp, uint8_t gain);

#endif /* LAMP_PWM_H */
