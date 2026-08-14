/*
 * wigwag — firmware entry point.
 *
 * Currently two things: the D49 lamp (TCC0 PWM, proven on hardware) and the RNWF02 AT client
 * talking to the module over SERCOM0. It is not the finished device — the received state does not
 * yet drive the lamps, which is lamp.c's job.
 *
 * On the PL10 Curiosity Nano the lamp is the board's own LED0, because PB02 carries both LED0 and
 * TCC0/WO2, and that LED is active low (D72) — polarity comes from devicetree and is printed at
 * boot so a silent regression cannot hide (D74).
 *
 * No dynamic allocation and no floating point anywhere — Rule 5, ADR-0008.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rnwf_at.h"
#include "rnwf_at_cmds.h"
#include "rnwf_uart.h"

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const struct pwm_dt_spec lamp_yellow = PWM_DT_SPEC_GET(DT_ALIAS(lamp_yellow));

static struct rnwf_at at_client;

/*
 * 125 steps of 10 ms = 1250 ms per cycle = 0.8 Hz, the BUSY breathe rate from CONTEXT.md.
 *
 * Scheduled on absolute deadlines rather than k_msleep(). A relative sleep measures the gap
 * *between* iterations, so every microsecond spent rendering or talking to the module is added to
 * the period: measured 1297 ms against an intended 1250 ms before this changed (D70). Absolute
 * deadlines make the rate the clock's business rather than the workload's.
 */
#define BREATHE_STEPS	125U
#define BREATHE_STEP_MS	10
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
 * Confirmed on hardware: perceived brightness is roughly the cube root of duty, so cubing here
 * cancels it and a linear ramp in `level` is *seen* as a linear ramp in brightness — an even fade
 * with no dwell at either end. Worth knowing when reading a scope: the duty cycle is heavily
 * skewed toward zero even though the light is not.
 *
 * Dividing the period first keeps every intermediate inside 32 bits.
 */
static uint32_t gamma_pulse(uint32_t level, uint32_t period)
{
	const uint32_t duty = (level * level * level) / (LEVEL_MAX * LEVEL_MAX);

	return (period / LEVEL_MAX) * duty;
}

static void on_message(void *user, const char *topic, const char *payload)
{
	ARG_UNUSED(user);

	/* lamp.c will map this onto the lamps; for now prove it arrives intact. */
	printk("wigwag: %s = %s\n", topic, payload);
}

static void on_link(void *user, bool linked)
{
	ARG_UNUSED(user);

	/*
	 * Rule 4 / ADR-0007. Losing the link must become the amber flicker, not a stale lamp; that
	 * behaviour belongs to lamp.c, so for now the transition is reported and nothing more.
	 */
	printk("wigwag: link %s\n", linked ? "LINKED" : "UNLINKED");
}

static const struct rnwf_at_config module_cfg = {
	/*
	 * Compile-time credentials for v1 (D56, ADR-0012). These placeholders are replaced by
	 * Kconfig from a gitignored credentials.conf before this talks to a real access point.
	 */
	.ssid = "wigwag-test",
	.passphrase = "",
	.sec_type = RNWF_SEC_OPEN,

	.broker_host = "localhost",
	.broker_port = 1883,
	.client_id = "wigwag-1",
	.username = NULL,
	.password = NULL,
	.keep_alive_s = 60,

	.state_topic = "wigwag/state",
	.online_topic = "wigwag/online",
};

int main(void)
{
	static const struct rnwf_at_callbacks cb = {
		.on_message = on_message,
		.on_link = on_link,
	};
	uint64_t cycles = 0U;
	uint32_t step = 0U;
	int64_t deadline;
	bool at_ready;
	int ret;

	printk("wigwag: starting\n");

	if (!pwm_is_ready_dt(&lamp_yellow)) {
		printk("wigwag: FAIL, PWM device not ready\n");
		return -ENODEV;
	}

	ret = pwm_get_cycles_per_sec(lamp_yellow.dev, lamp_yellow.channel, &cycles);
	printk("wigwag: lamp %s ch%u, %u ns period, %u cycles/s (ret %d)\n",
	       lamp_yellow.dev->name, lamp_yellow.channel, lamp_yellow.period,
	       (uint32_t)cycles, ret);
	printk("wigwag: polarity flags 0x%x (%s)\n", lamp_yellow.flags,
	       ((lamp_yellow.flags & PWM_POLARITY_INVERTED) != 0) ? "inverted, active-low lamp"
								 : "normal, active-high lamp");

	at_ready = (rnwf_uart_init(&at_client, &module_cfg, &cb) == 0);
	if (at_ready) {
		printk("wigwag: module UART up, connecting to \"%s\"\n", module_cfg.ssid);
		rnwf_at_start(&at_client, (uint32_t)k_uptime_get());
	}

	deadline = k_uptime_get();

	while (true) {
		static enum rnwf_at_state reported = RNWF_AT_ST_IDLE;
		uint32_t now;

		ret = pwm_set_pulse_dt(&lamp_yellow,
				       gamma_pulse(breathe_level(step), lamp_yellow.period));
		if (ret < 0) {
			printk("wigwag: FAIL, pwm_set_pulse_dt returned %d\n", ret);
			return ret;
		}

		if (at_ready) {
			/*
			 * Receive is interrupt-driven into a ring, so a 10 ms drain cadence is
			 * ample for AT traffic. Note that a long command blocks this loop while it
			 * transmits, so the lamp will stutter during a connect burst — which is
			 * precisely why lamp rendering moves to its own thread in lamp.c.
			 */
			now = (uint32_t)k_uptime_get();
			rnwf_uart_poll(&at_client);
			rnwf_at_tick(&at_client, now);

			/*
			 * Report transitions only. A device whose sole output is a lamp is
			 * otherwise unobservable during bring-up: with no module attached this
			 * loops reset -> timeout -> backoff in complete silence, which is
			 * indistinguishable from a crash.
			 */
			if (at_client.state != reported) {
				static const char *const names[] = {
					"IDLE", "RESETTING", "SCRIPT", "READY", "BACKOFF",
				};

				reported = at_client.state;
				printk("wigwag: at %s (errors %u timeouts %u overruns %u)\n",
				       names[reported], at_client.errors, at_client.timeouts,
				       rnwf_uart_overruns());
			}
		}

		step = (step + 1U) % BREATHE_STEPS;
		deadline += BREATHE_STEP_MS;
		k_sleep(K_TIMEOUT_ABS_MS(deadline));
	}

	return 0;
}
