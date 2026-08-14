/*
 * Zephyr renderer for the lamps. See lamp_pwm.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lamp_pwm.h"

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/*
 * ~100 Hz render, as planned. Fast enough that the 4 Hz ERROR alternation and the flicker land on
 * clean edges, slow enough to be nothing on a 24 MHz part.
 */
#define FRAME_MS 10

/*
 * 512 bytes, and now measured rather than guessed: the high-water mark is **348 bytes** after an
 * exercise covering every lamp behaviour, both link transitions and a full reconnect, leaving 164 B
 * of headroom. Verified by raising this to 768 and confirming the reported usage stayed at 348
 * while the unused figure moved — see firmware/README.md for the method.
 */
#define LAMP_STACK_SZ	512
#define LAMP_PRIORITY	-1	/* cooperative: must preempt the AT loop's blocking transmit */

static const struct pwm_dt_spec lamps[LAMP_COUNT] = {
	[LAMP_GREEN] = PWM_DT_SPEC_GET(DT_ALIAS(lamp_green)),
	[LAMP_YELLOW] = PWM_DT_SPEC_GET(DT_ALIAS(lamp_yellow)),
	[LAMP_RED] = PWM_DT_SPEC_GET(DT_ALIAS(lamp_red)),
};

static struct {
	enum wigwag_state state;
	uint32_t state_since_ms;
	bool trusted;
	struct k_spinlock lock;
} shared = {
	/*
	 * Start untrusted, so the lamps show the fail-visible pattern from the first frame until
	 * something proves otherwise. A device that boots looking idle is a device that lies for
	 * however long its first connection takes.
	 */
	.state = WIGWAG_IDLE,
	.trusted = false,
};

static void render(const struct lamp_frame *f)
{
	size_t i;

	for (i = 0; i < LAMP_COUNT; i++) {
		/*
		 * Polarity comes from each lamp's devicetree flags, which pwm_set_pulse_dt() applies
		 * — this board mixes active-low and active-high lamps, and the PCB will be uniformly
		 * active high, so nothing here may assume either.
		 */
		(void)pwm_set_pulse_dt(&lamps[i], lamp_gamma_pulse(f->level[i], lamps[i].period));
	}
}

static void lamp_thread(void *a, void *b, void *c)
{
	int64_t deadline = k_uptime_get();

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		enum wigwag_state state;
		uint32_t since;
		bool trusted;
		struct lamp_frame f;
		k_spinlock_key_t key = k_spin_lock(&shared.lock);

		state = shared.state;
		since = shared.state_since_ms;
		trusted = shared.trusted;
		k_spin_unlock(&shared.lock, key);

		f = lamp_render(state, trusted, (uint32_t)k_uptime_get(), since);
		render(&f);

		/*
		 * Absolute deadlines, so a slow frame does not push the animation's rate around
		 * (D70). The phase comes from the clock anyway, but this keeps the cadence even.
		 */
		deadline += FRAME_MS;
		k_sleep(K_TIMEOUT_ABS_MS(deadline));
	}
}

/*
 * Power-on lamp test: each lamp to full for a moment, then all three together.
 *
 * Worth having permanently on an appliance whose only output is light — it proves every channel and
 * every wire at maximum brightness, once, where a dim steady state proves nothing. Added after
 * "nothing on PA24" turned out to be a pin conflict plus a duty cycle too small to see: at full
 * brightness both faults would have been obvious in the first second after boot.
 *
 * Runs on the caller's thread before the renderer starts, so it cannot race the animation.
 */
static void lamp_selftest(void)
{
	static const char *const names[LAMP_COUNT] = { "green", "yellow", "red" };
	size_t i;

	for (i = 0; i < LAMP_COUNT; i++) {
		printk("wigwag: lamp test %s (ch%u)\n", names[i], lamps[i].channel);
		(void)pwm_set_pulse_dt(&lamps[i], lamps[i].period);
		k_msleep(400);
		(void)pwm_set_pulse_dt(&lamps[i], 0);
	}

	for (i = 0; i < LAMP_COUNT; i++) {
		(void)pwm_set_pulse_dt(&lamps[i], lamps[i].period);
	}
	k_msleep(400);
	for (i = 0; i < LAMP_COUNT; i++) {
		(void)pwm_set_pulse_dt(&lamps[i], 0);
	}
}

K_THREAD_STACK_DEFINE(lamp_stack, LAMP_STACK_SZ);
static struct k_thread lamp_tid;

int lamp_pwm_init(void)
{
	size_t i;

	for (i = 0; i < LAMP_COUNT; i++) {
		if (!pwm_is_ready_dt(&lamps[i])) {
			printk("wigwag: lamp %u not ready\n", (unsigned)i);
			return -ENODEV;
		}
	}

	printk("wigwag: lamps on %s ch%u/%u/%u, flags %x/%x/%x\n", lamps[0].dev->name,
	       lamps[LAMP_GREEN].channel, lamps[LAMP_YELLOW].channel, lamps[LAMP_RED].channel,
	       lamps[LAMP_GREEN].flags, lamps[LAMP_YELLOW].flags, lamps[LAMP_RED].flags);

	lamp_selftest();

	(void)k_thread_create(&lamp_tid, lamp_stack, K_THREAD_STACK_SIZEOF(lamp_stack),
			      lamp_thread, NULL, NULL, NULL, LAMP_PRIORITY, 0, K_NO_WAIT);
	(void)k_thread_name_set(&lamp_tid, "lamp");

	return 0;
}

void lamp_pwm_set_state(enum wigwag_state state)
{
	k_spinlock_key_t key = k_spin_lock(&shared.lock);

	/*
	 * Only restart the escalation clock on a real change. The host sends BUSY repeatedly as a
	 * TTL heartbeat (CONTEXT.md), and treating each one as new would be harmless there but
	 * would reset WAIT's 30 s timer on any state that repeats — the escalation must measure how
	 * long you have been waiting, not how long since the last message.
	 */
	if (state != shared.state) {
		shared.state = state;
		shared.state_since_ms = (uint32_t)k_uptime_get();
	}

	k_spin_unlock(&shared.lock, key);
}

void lamp_pwm_set_link(bool trusted)
{
	k_spinlock_key_t key = k_spin_lock(&shared.lock);

	shared.trusted = trusted;
	k_spin_unlock(&shared.lock, key);
}
