/*
 * wigwag — firmware entry point.
 *
 * Wires the pieces together and owns the AT loop. Everything with behaviour of its own lives
 * elsewhere: rnwf_at.c speaks to the module, rnwf_uart.c carries the bytes, link.c decides whether
 * the device may believe what it shows, and lamp.c/lamp_pwm.c render it.
 *
 * No dynamic allocation and no floating point anywhere — Rule 5, ADR-0008.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "button.h"
#include "button_gpio.h"
#include "console.h"
#include "lamp.h"
#include "lamp_pwm.h"
#include "link.h"
#include "rnwf_at.h"
#include "rnwf_at_cmds.h"
#include "rnwf_uart.h"
#include "settings_store.h"
#include "transport.h"
#include "wdog.h"
#include "wdog_wdt.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <string.h>

/* 10 ms: fast enough to drain the receive ring well inside its 256-byte capacity at 115200. */
#define AT_POLL_MS 10

static struct rnwf_at at_client;
static struct link link_state;
static struct button button_state;
static struct transport tport;

/*
 * Live settings: build-time defaults from Kconfig, overlaid by whatever is stored, editable over the
 * console (D37/D56). Static because the AT client borrows pointers into it and never copies.
 */
static struct wigwag_settings settings;

/*
 * The AT client's view of those settings.
 *
 * Not const any more, and the strings now point into `settings` rather than at flash literals — which
 * is what makes them configurable without a rebuild, and what costs ~300 bytes of RAM (settings.h).
 * The topic names stay literals: they are the protocol (CONTEXT.md), not configuration.
 */
static struct rnwf_at_config module_cfg = {
	.keep_alive_s = 60,

	.state_topic = "wigwag/state",
	.online_topic = "wigwag/online",
	.host_online_topic = "wigwag/host_online",
	.brightness_topic = "wigwag/brightness",
};

/** Point the AT configuration at the loaded settings. An empty field means "not configured". */
static void module_cfg_from_settings(void)
{
	module_cfg.ssid = settings.ssid;
	module_cfg.passphrase = settings.pass;
	module_cfg.sec_type = settings.sec;
	module_cfg.broker_host = settings.broker;
	module_cfg.broker_port = settings.port;
	module_cfg.client_id = settings.client;

	/*
	 * NULL rather than "" for the optional pair: rnwf_at.c skips the configuration step entirely
	 * when these are NULL, and sending an empty username to a broker that wants none is not the
	 * same as not sending one.
	 */
	module_cfg.username = (settings.user[0] != '\0') ? settings.user : NULL;
	module_cfg.password = (settings.mqttpass[0] != '\0') ? settings.mqttpass : NULL;
}

/*
 * Build {"event":"press","ms":<n>} without printf.
 *
 * Hand-rolled because main's stack is the tight one at 860 of 1024 B (D78), already carrying printk
 * formatting and the AT script's vsnprintf; adding another varargs frame to the publish path is the
 * wrong direction. Fixed prefix, decimal digits, fixed suffix — no varargs, no float, bounded.
 */
static size_t press_payload(char *out, size_t cap, uint32_t ms)
{
	static const char pre[] = "{\"event\":\"press\",\"ms\":";
	static const char post[] = "}";
	char digits[11];
	size_t n = 0;
	size_t d = 0;

	if (cap < sizeof(pre) + sizeof(digits) + sizeof(post)) {
		return 0;
	}

	memcpy(out, pre, sizeof(pre) - 1U);
	n = sizeof(pre) - 1U;

	if (ms == 0U) {
		digits[d++] = '0';
	}
	while (ms > 0U) {
		digits[d++] = (char)('0' + (ms % 10U));
		ms /= 10U;
	}
	while (d > 0U) {
		out[n++] = digits[--d];	/* reverse into place */
	}

	out[n++] = post[0];
	out[n] = '\0';

	return n;
}

static void publish_press(uint32_t ms)
{
	char payload[48];

	if (press_payload(payload, sizeof(payload), ms) == 0U) {
		return;
	}

	/*
	 * Not retained: a button press is an event, not a state. A retained press would be replayed
	 * to every future subscriber, including this device after a reboot (CONTEXT.md).
	 */
	if (rnwf_at_publish(&at_client, "wigwag/button", payload, false) == 0) {
		printk("wigwag: published press %u ms\n", ms);
	} else {
		/*
		 * Dropped because the link is not up. Correct: D35 says the host decides what a press
		 * means, and a press queued now and delivered minutes later would be a lie about when
		 * it happened.
		 */
		printk("wigwag: press %u ms dropped, link down\n", ms);
	}
}

static void on_message(void *user, const char *topic, const char *payload)
{
	enum wigwag_state state;

	ARG_UNUSED(user);

	/* The host liveness topic belongs to link supervision, not to the lamps. */
	if (link_note_message(&link_state, topic, payload, module_cfg.host_online_topic,
			      (uint32_t)k_uptime_get())) {
		printk("wigwag: host_online = %s\n", payload);
		return;
	}

	if (strcmp(topic, module_cfg.brightness_topic) == 0) {
		uint8_t level;

		if (lamp_brightness_parse(payload, &level)) {
			printk("wigwag: brightness %u\n", level);
			lamp_pwm_set_brightness(level);
		} else {
			/* Keep the current setting rather than guess at a malformed number. */
			printk("wigwag: bad brightness payload, ignored\n");
		}
		return;
	}

	if (!wigwag_state_parse(payload, &state)) {
		/*
		 * Keep showing whatever we had. Inventing a state from an unparseable payload is the
		 * one thing worse than showing a slightly old one, and link supervision is what
		 * catches a host that has genuinely stopped making sense.
		 */
		printk("wigwag: unparseable state payload, ignored\n");
		return;
	}

	printk("wigwag: state %s\n", wigwag_state_str(state));
	lamp_pwm_set_state(state);
}

static void on_link(void *user, bool linked)
{
	ARG_UNUSED(user);

	/* One of several inputs to the link condition — link.h explains why it is not the only one. */
	link_note_at(&link_state, linked, (uint32_t)k_uptime_get());
	printk("wigwag: at link %s\n", linked ? "up" : "down");
}

/*
 * The AT service loop: drain the module UART, advance the AT state machine, re-evaluate the link
 * condition, and report transitions. Never returns.
 *
 * This runs on the **main thread**, which is a deliberate choice rather than an accident. Zephyr's
 * main thread is an ordinary thread — the kernel calls main() from its initialization thread at
 * CONFIG_MAIN_THREAD_PRIORITY — so using it as a service loop is idiomatic, and a separate thread
 * would cost roughly 600 bytes: ~7 % of this part's entire SRAM, for a name.
 *
 * It would also not fix the thing that looks like it should. main's measured peak is
 * max(init depth, loop depth) because init finishes before the loop starts; splitting them
 * duplicates the capacity rather than reducing the maximum. main is at 860 of 1024 B (D78) because
 * of printk formatting and the AT script's vsnprintf, and the cure for that is narrower format
 * strings, not another stack.
 *
 * Revisit when any of these becomes true:
 *   - a second context needs the AT client concurrently. button.c will want to publish from an
 *     interrupt; a flag consumed by this loop is enough, but a message queue feeding a dedicated
 *     thread is the textbook shape if it grows past that;
 *   - main's stack stops having comfortable headroom once credentials lengthen its commands.
 *
 * The watchdog was the one trigger on this list that looked like it might force a split, and it did
 * not: proving both contexts are alive needs each to check in with wdog.c, which is independent of
 * how many threads there are. What it did change is that this loop no longer feeds unconditionally.
 */
static void at_service(bool at_ready)
{
	int64_t deadline = k_uptime_get();

	while (true) {
		static enum rnwf_at_state at_reported = RNWF_AT_ST_IDLE;
		static enum link_condition link_reported = LINK_LINKED;
		static bool announced_online;
		uint32_t now;

		if (at_ready) {
			now = (uint32_t)k_uptime_get();
			rnwf_uart_poll(&at_client);
			rnwf_at_tick(&at_client, now);

			/*
			 * Report transitions only. A device whose sole output is three lamps is
			 * otherwise unobservable during bring-up: with no module attached this loops
			 * reset -> timeout -> backoff in silence, indistinguishable from a crash.
			 */
			if (at_client.state != at_reported) {
				static const char *const names[] = {
					"IDLE", "RESETTING", "SCRIPT", "READY", "BACKOFF",
				};

				at_reported = at_client.state;
				printk("wigwag: at %s (errors %u timeouts %u polls %u "
				       "overruns %u)\n", names[at_reported], at_client.errors,
				       at_client.timeouts, at_client.polls,
				       rnwf_uart_overruns());
			}
		}

		/*
		 * The birth half of wigwag/online. AT+MQTTLWT already registered `0` as the will, so
		 * the broker reports an unclean death on our behalf (CONTEXT.md, ADR-0003) — but a will
		 * with no positive counterpart is useless: a subscriber cannot tell "never seen" from
		 * "connected".
		 *
		 * Retained, so a subscriber arriving later sees the current truth rather than a stale
		 * `0` from some previous death.
		 *
		 * Keyed on the AT client reaching READY, deliberately *not* on the link condition.
		 * This topic means "connected to the broker", which stays true when the host daemon
		 * dies; publishing 0 then would be the device reporting itself offline because
		 * somebody else went away.
		 */
		if (at_ready && at_client.state == RNWF_AT_ST_READY && !announced_online) {
			if (rnwf_at_publish(&at_client, module_cfg.online_topic, "1", true) == 0) {
				printk("wigwag: online = 1\n");
				announced_online = true;
			}
		} else if (at_client.state != RNWF_AT_ST_READY) {
			/* Republish on the next connection; the will covers the gap. */
			announced_online = false;
		}

		{
			/*
			 * Sampled here rather than on an interrupt (D86). At 10 ms this is well inside
			 * the debounce window, so no press can be missed and no edge needs latching.
			 */
			struct button_result ev =
				button_sample(&button_state, button_gpio_pressed(),
					      (uint32_t)k_uptime_get());

			if (ev.event == BUTTON_EVENT_PRESS) {
				publish_press(ev.ms);
			} else if (ev.event == BUTTON_EVENT_LONG) {
				/* D58 will enter provisioning mode here. */
				printk("wigwag: long press at %u ms (provisioning, not yet built)\n",
				       ev.ms);
			}
		}

		/*
		 * Commands arrive on the console UART, drained here rather than on their own thread —
		 * the bytes are already buffered by an ISR, so this is only parsing, and a keystroke
		 * waiting up to 10 ms is imperceptible.
		 */
		console_poll();

		link_tick(&link_state, (uint32_t)k_uptime_get());

		if (link_get(&link_state) != link_reported) {
			link_reported = link_get(&link_state);
			printk("wigwag: link %s (%s)\n",
			       (link_reported == LINK_LINKED) ? "LINKED" : "UNLINKED",
			       link_reason_str(link_state.reason));
		}

		/*
		 * link.c answers "is the Wi-Fi path trustworthy"; transport.c answers "which path is the
		 * device listening to, and may it be believed" (D104). The lamps follow the transport, so
		 * a trusted USB host outranks a healthy Wi-Fi link and a quiet USB host goes fail-visible
		 * even while Wi-Fi is up.
		 */
		{
			static enum transport_kind tport_reported = TRANSPORT_NONE;
			static bool trust_reported;
			uint32_t t_now = (uint32_t)k_uptime_get();

			transport_note_wifi(&tport, link_is_trusted(&link_state), t_now);
			transport_tick(&tport, t_now);
			lamp_pwm_set_link(transport_is_trusted(&tport));

			if (transport_active(&tport) != tport_reported ||
			    transport_is_trusted(&tport) != trust_reported) {
				tport_reported = transport_active(&tport);
				trust_reported = transport_is_trusted(&tport);
				printk("wigwag: transport %s %s (%s)\n",
				       transport_kind_str(tport_reported),
				       trust_reported ? "TRUSTED" : "untrusted",
				       transport_reason_str(tport.reason));
			}
		}

		/*
		 * End of a complete pass: bytes drained, AT ticked, button sampled, link re-evaluated.
		 * Beat here rather than at the top of the loop so the beat attests to the work, not to
		 * having been scheduled.
		 */
		now = (uint32_t)k_uptime_get();
		wdog_beat(WDOG_TASK_AT, now);

		/*
		 * Feeding lives here because this loop is the one with a natural 10 ms cadence — but it
		 * feeds on wdog.c's verdict, not its own liveness. If the render thread stops, this loop
		 * keeps running and deliberately stops feeding.
		 */
		wdog_wdt_service(now);

		deadline += AT_POLL_MS;
		k_sleep(K_TIMEOUT_ABS_MS(deadline));
	}
}

int main(void)
{
	static const struct rnwf_at_callbacks cb = {
		.on_message = on_message,
		.on_link = on_link,
	};
	bool at_ready;

	printk("wigwag: starting\n");

	/* Before anything else can obscure it: why did the last run end? */
	wdog_report_reset_cause();

	/*
	 * Rename the thread we are on, so a stack or thread report says "at" rather than "main".
	 * Compiled out entirely when CONFIG_THREAD_NAME is off, which it is in the shipping build —
	 * the name exists for the measurement overlay (prj_stacks.conf), where a report full of
	 * thread-object addresses is nearly useless.
	 */
	(void)k_thread_name_set(k_current_get(), "at");

	/*
	 * Settings before anything that uses them. A failed mount is reported and survivable: the
	 * build-time defaults stand, and a light running on stale configuration beats one that refuses
	 * to boot (settings_store.h).
	 */
	{
		int ret = settings_load(&settings);

		if (ret != 0) {
			printk("wigwag: settings store unavailable (%d), using defaults\n", ret);
		}
		module_cfg_from_settings();
	}

	if (lamp_pwm_init() != 0) {
		return -ENODEV;
	}

	/* Calibration and brightness are settings, so apply them before the first frame is judged. */
	lamp_pwm_set_brightness(settings.brightness);
	for (size_t i = 0; i < LAMP_COUNT; i++) {
		lamp_pwm_set_gain((enum lamp_id)i, settings.gain[i]);
	}

	/*
	 * Which transport owns the lamps is a setting, not a discovery (ADR-0022). `wifi_configured` is
	 * only "is there an SSID to try", which decides whether the wireless path can work at all.
	 */
	transport_init(&tport, (enum wigwag_transport)settings.transport,
		       settings.ssid[0] != '\0');
	printk("wigwag: transport %s (configured)\n",
	       transport_kind_str(transport_active(&tport)));

	/* Same warning as the console gives, for a device that was configured and then rebooted. */
	if (settings.transport == WIGWAG_TRANSPORT_USB && settings.ssid[0] != '\0') {
		printk("wigwag: note: ssid \"%s\" configured but transport is usb\n", settings.ssid);
	}

	(void)console_init(&settings, &tport);

	link_init(&link_state, LINK_HOST_GRACE_MS);

	button_init(&button_state);
	if (button_gpio_init() != 0) {
		/* A missing button is not fatal — the lamps are the point of the device. */
		printk("wigwag: continuing without the button\n");
	}

	/*
	 * The module is brought up only when Wi-Fi is the configured transport *and* there is a network
	 * to join. A wired device never starts it: there is nothing for it to do, and letting it reset,
	 * time out and back off forever is waste measured at 947 AT timeouts in one session (D118).
	 */
	if (!transport_wants_module(&tport)) {
		printk("wigwag: wired transport, module not started\n");
		at_ready = false;
	} else if (settings.ssid[0] == '\0') {
		printk("wigwag: no ssid configured; set one, or `set transport usb`\n");
		at_ready = false;
	} else {
		at_ready = (rnwf_uart_init(&at_client, &module_cfg, &cb) == 0);
		if (at_ready) {
			printk("wigwag: module UART up, connecting to \"%s\" (%s)\n", module_cfg.ssid,
			       (settings.pass[0] != '\0') ? "passphrase set" : "open");
			rnwf_at_start(&at_client, (uint32_t)k_uptime_get());
		}
	}

	/*
	 * Armed last, deliberately. Every task in enum wdog_task must already be beating, and the lamp
	 * selftest holds the CPU for 1.6 s during lamp_pwm_init() — arming before it would guarantee a
	 * reset loop that looked like a hardware fault.
	 */
	(void)wdog_wdt_init((uint32_t)k_uptime_get());

	/* Becomes the AT service loop and never returns. */
	at_service(at_ready);

	return 0;
}
