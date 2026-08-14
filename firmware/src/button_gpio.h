/*
 * Zephyr GPIO binding for the button: the hardware half of button.c.
 *
 * Polled, not interrupt-driven — see button.h and D86. That keeps this file to a pin read, with no
 * ISR, no callback registration and nothing to make ISR-safe.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BUTTON_GPIO_H
#define BUTTON_GPIO_H

#include <stdbool.h>

/** Configure the button pin. Returns 0, or -ENODEV if it is missing or not ready. */
int button_gpio_init(void);

/**
 * Read the button.
 *
 * True means pressed. Devicetree carries the active-low wiring, so no inversion happens here.
 * A read failure reports "not pressed": a stuck-on button would publish a continuous stream of
 * presses, which is worse than missing one.
 */
bool button_gpio_pressed(void);

#endif /* BUTTON_GPIO_H */
