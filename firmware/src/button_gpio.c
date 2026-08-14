/*
 * Zephyr GPIO binding for the button. See button_gpio.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "button_gpio.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

static const struct gpio_dt_spec sw = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

int button_gpio_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&sw)) {
		printk("wigwag: button not ready\n");
		return -ENODEV;
	}

	/*
	 * GPIO_INPUT plus the devicetree's GPIO_PULL_UP. The pull-up is not optional on this board:
	 * SW0 has no external one, so without it the pin floats and reads as noise (user guide §4.2).
	 */
	ret = gpio_pin_configure_dt(&sw, GPIO_INPUT);
	if (ret != 0) {
		printk("wigwag: button configure failed (%d)\n", ret);
		return ret;
	}

	printk("wigwag: button on %s pin %u\n", sw.port->name, sw.pin);
	return 0;
}

bool button_gpio_pressed(void)
{
	int val = gpio_pin_get_dt(&sw);

	/*
	 * Negative means a read error. Treat it as released: a fault that reported "pressed" would
	 * publish a press every debounce window, and a flood of false presses is worse than silence
	 * on a topic the host is entitled to trust.
	 */
	return val > 0;
}
