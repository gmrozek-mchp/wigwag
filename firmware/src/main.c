/*
 * wigwag — D49 spike: TCC0 PWM on PIC32CM PL10.
 *
 * Breathes one lamp at 0.8 Hz, the BUSY behaviour from CONTEXT.md. On the PL10 Curiosity Nano
 * the lamp is the board's own LED0, because PB02 carries both LED0 and TCC0/WO2.
 *
 * The point of the spike is the gate in front of the PCB (D49): does mainline's
 * pwm_mchp_tcc_g1 driver bind to this part with devicetree work alone? If the LED breathes,
 * TCC0 WO0/WO1/WO2 can be committed to the layout.
 *
 * No dynamic allocation and no floating point anywhere — Rule 5, ADR-0008.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/printk.h>

static const struct pwm_dt_spec lamp_yellow = PWM_DT_SPEC_GET(DT_ALIAS(lamp_yellow));

/*
 * 125 steps of 10 ms = 1250 ms per cycle, intended as 0.8 Hz.
 *
 * Measured on hardware it is 1297 ms, i.e. 0.771 Hz — 3.8 % slow. The kernel tick is 10 kHz so
 * timeout rounding accounts for only 0.1 %; the rest is the loop's own work, mostly
 * pwm_set_pulse_dt() waiting on TCC SYNCBUSY across the clock domain boundary. Relative sleeps
 * accumulate that error by construction.
 *
 * Left as-is here because the spike's job is to prove TCC PWM, not to render lamps. lamp.c must
 * instead schedule on absolute deadlines (k_timer, or k_sleep with K_TIMEOUT_ABS_MS) so the rate
 * is set by the clock rather than by however long a render takes.
 */
#define BREATHE_STEPS	125U
#define BREATHE_STEP_MS	10U
#define LEVEL_MAX	255U

/*
 * Triangle wave, 0 -> LEVEL_MAX -> 0 across one breathe cycle.
 */
static uint32_t breathe_level(uint32_t step)
{
	const uint32_t peak = BREATHE_STEPS / 2U;
	const uint32_t last = BREATHE_STEPS - 1U;

	if (step < peak) {
		return (step * LEVEL_MAX) / peak;
	}

	return ((last - step) * LEVEL_MAX) / (last - peak);
}

/*
 * Perceptual correction. The eye's response to luminance is roughly logarithmic, so a linear
 * duty ramp reads as "snap on, then stall" rather than a breath. Cubing the level tracks it
 * closely enough for a diffused lamp and costs a couple of 32-bit multiplies — no powf(), no
 * soft-float library pulled into a 64 KB part.
 *
 * Dividing the period first keeps every intermediate inside 32 bits.
 */
static uint32_t gamma_pulse(uint32_t level, uint32_t period)
{
	const uint32_t duty = (level * level * level) / (LEVEL_MAX * LEVEL_MAX);

	return (period / LEVEL_MAX) * duty;
}

int main(void)
{
	uint64_t cycles = 0U;
	uint32_t cycle_count = 0U;
	uint32_t step = 0U;
	int ret;

	printk("wigwag: D49 spike, TCC0 PWM on PIC32CM PL10\n");

	if (!pwm_is_ready_dt(&lamp_yellow)) {
		printk("wigwag: FAIL, PWM device not ready\n");
		return -ENODEV;
	}

	ret = pwm_get_cycles_per_sec(lamp_yellow.dev, lamp_yellow.channel, &cycles);
	printk("wigwag: %s channel %u, period %u ns, %u cycles/s (ret %d)\n",
	       lamp_yellow.dev->name, lamp_yellow.channel, lamp_yellow.period,
	       (uint32_t)cycles, ret);

	while (true) {
		ret = pwm_set_pulse_dt(&lamp_yellow,
				       gamma_pulse(breathe_level(step), lamp_yellow.period));
		if (ret < 0) {
			printk("wigwag: FAIL, pwm_set_pulse_dt returned %d\n", ret);
			return ret;
		}

		step = (step + 1U) % BREATHE_STEPS;
		if (step == 0U) {
			/*
			 * One line per breathe cycle. Without it the spike is silent after boot,
			 * so confirming it is running means resetting the target — and the
			 * debugger cannot reset while the console port is open on the same USB
			 * device. Reporting peak pulse also shows the duty range actually
			 * reaching the hardware.
			 */
			cycle_count++;
			printk("wigwag: breathe cycle %u at %u ms, carrier %u Hz "
			       "(%u cycles/s, period %u ns), peak pulse %u ns\n",
			       cycle_count, (uint32_t)k_uptime_get(),
			       (uint32_t)(NSEC_PER_SEC / lamp_yellow.period), (uint32_t)cycles,
			       lamp_yellow.period, gamma_pulse(LEVEL_MAX, lamp_yellow.period));
		}

		k_msleep(BREATHE_STEP_MS);
	}

	return 0;
}
