/*
 * Transport trust. See transport.h — the *selection* is a setting, so all that is left here is
 * deciding whether the configured transport may currently be believed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "transport.h"

#include <string.h>

void transport_init(struct transport *t, enum wigwag_transport configured, bool wifi_configured)
{
	memset(t, 0, sizeof(*t));

	t->configured = configured;
	t->wifi_configured = wifi_configured;
	t->reason = TRANSPORT_REASON_STARTING;

	/*
	 * The active transport is known from the first instruction — it is configuration, not a
	 * discovery — but trust starts false, for the same reason lamp_pwm.c starts untrusted: a device
	 * that boots believing itself connected lies for however long its first connection takes.
	 */
	t->active = (configured == WIGWAG_TRANSPORT_USB) ? TRANSPORT_USB : TRANSPORT_WIFI;
	t->trusted = false;
}

void transport_note_host(struct transport *t, uint32_t now_ms)
{
	t->host_seen = true;
	t->host_last_ms = now_ms;

	/*
	 * Talking again cancels a goodbye. A daemon that said `host off` and then carried on has
	 * changed its mind, and the newer evidence is the better evidence.
	 */
	t->host_bye = false;
}

void transport_note_host_bye(struct transport *t, uint32_t now_ms)
{
	t->host_bye = true;
	t->host_last_ms = now_ms;
}

void transport_note_wifi(struct transport *t, bool trusted, uint32_t now_ms)
{
	(void)now_ms;

	t->wifi_trusted = trusted;
}

/** Is the host's last word recent enough to act on? Wrap-safe. */
static bool host_fresh(const struct transport *t, uint32_t now_ms)
{
	if (!t->host_seen || t->host_bye) {
		return false;
	}

	return (uint32_t)(now_ms - t->host_last_ms) <= TRANSPORT_HOST_TTL_MS;
}

void transport_tick(struct transport *t, uint32_t now_ms)
{
	if (t->configured == WIGWAG_TRANSPORT_USB) {
		t->active = TRANSPORT_USB;
		t->trusted = host_fresh(t, now_ms);

		if (t->trusted) {
			t->reason = TRANSPORT_REASON_OK;
		} else if (t->host_bye) {
			t->reason = TRANSPORT_REASON_HOST_BYE;
		} else if (t->host_seen) {
			t->reason = TRANSPORT_REASON_HOST_QUIET;
		} else {
			/* Nothing has ever spoken; distinguishable from a host that went away. */
			t->reason = TRANSPORT_REASON_STARTING;
		}

		return;
	}

	/*
	 * Wireless. Note this never falls back to the wire even when a host is talking on it — the
	 * console still works for configuration and diagnostics, but it does not get to take the lamps
	 * (ADR-0022). That is the whole point: plugging in a cable changes nothing.
	 */
	if (!t->wifi_configured) {
		t->active = TRANSPORT_NONE;
		t->trusted = false;
		t->reason = TRANSPORT_REASON_NOTHING;
		return;
	}

	t->active = TRANSPORT_WIFI;
	t->trusted = t->wifi_trusted;
	t->reason = t->wifi_trusted ? TRANSPORT_REASON_OK : TRANSPORT_REASON_WIFI_DOWN;
}

const char *transport_kind_str(enum transport_kind kind)
{
	switch (kind) {
	case TRANSPORT_USB:
		return "usb";
	case TRANSPORT_WIFI:
		return "wifi";
	case TRANSPORT_NONE:
	default:
		return "none";
	}
}

const char *transport_reason_str(enum transport_reason reason)
{
	switch (reason) {
	case TRANSPORT_REASON_OK:
		return "ok";
	case TRANSPORT_REASON_HOST_QUIET:
		return "host quiet";
	case TRANSPORT_REASON_HOST_BYE:
		return "host said goodbye";
	case TRANSPORT_REASON_WIFI_DOWN:
		return "wifi down";
	case TRANSPORT_REASON_NOTHING:
		return "no ssid configured";
	case TRANSPORT_REASON_STARTING:
	default:
		return "starting";
	}
}
