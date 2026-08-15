/*
 * Transport selection and supervision. See transport.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "transport.h"

#include <string.h>

void transport_init(struct transport *t, bool wifi_configured)
{
	memset(t, 0, sizeof(*t));

	t->wifi_configured = wifi_configured;
	t->reason = TRANSPORT_REASON_STARTING;

	/*
	 * Start untrusted and on nothing, for the same reason lamp_pwm.c starts untrusted: a device
	 * that boots believing itself connected lies for however long its first connection takes.
	 */
	t->active = TRANSPORT_NONE;
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

void transport_note_usb_cfg(struct transport *t, enum transport_usb_cfg cfg, uint32_t now_ms)
{
	(void)now_ms;

	t->usb_cfg = cfg;
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

static void select_wifi(struct transport *t, uint32_t now_ms)
{
	(void)now_ms;

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

void transport_tick(struct transport *t, uint32_t now_ms)
{
	enum transport_kind was = t->active;

	if (host_fresh(t, now_ms)) {
		/*
		 * A host is talking. That claims the device outright, even if USBCFG says otherwise:
		 * received bytes are stronger evidence of a live host than a pin, and on a board with no
		 * such pin they are the only evidence there is.
		 */
		t->active = TRANSPORT_USB;
		t->trusted = true;
		t->reason = TRANSPORT_REASON_OK;
		t->usb_claimed_ms = now_ms;

		if (was != TRANSPORT_USB) {
			t->handovers++;
		}
		return;
	}

	if (t->active == TRANSPORT_USB) {
		bool unplugged = (t->usb_cfg == TRANSPORT_USB_ABSENT);
		bool released = (uint32_t)(now_ms - t->usb_claimed_ms) > TRANSPORT_RELEASE_MS;

		/*
		 * USB held the device and the host has gone quiet. Untrusted immediately — that is Rule 4,
		 * and the lamps must say "I do not know" rather than hold the last thing they were told.
		 *
		 * But keep *holding* the transport for a short window rather than switching straight to
		 * Wi-Fi, whose retained state could be older still. Handing over to a second possibly-stale
		 * source the instant the first hiccups would trade a known unknown for an unknown one.
		 *
		 * An unplugged cable skips the window: USBCFG deasserting is unambiguous, and waiting out
		 * the release timer would just delay a recovery we already know is needed.
		 */
		t->trusted = false;

		if (t->host_bye) {
			t->reason = TRANSPORT_REASON_HOST_BYE;
		} else if (unplugged) {
			t->reason = TRANSPORT_REASON_UNPLUGGED;
		} else {
			t->reason = TRANSPORT_REASON_HOST_QUIET;
		}

		/*
		 * A goodbye is not a fault, so it releases at once too: the host told us it was going, and
		 * lingering on a transport we have been told is finished is pointless.
		 */
		if (!unplugged && !released && !t->host_bye) {
			return;
		}

		select_wifi(t, now_ms);

		if (t->active != was) {
			t->handovers++;
		}
		return;
	}

	select_wifi(t, now_ms);

	if (t->active != was) {
		t->handovers++;
	}
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
	case TRANSPORT_REASON_UNPLUGGED:
		return "usb unplugged";
	case TRANSPORT_REASON_WIFI_DOWN:
		return "wifi down";
	case TRANSPORT_REASON_NOTHING:
		return "nothing configured";
	case TRANSPORT_REASON_STARTING:
	default:
		return "starting";
	}
}
