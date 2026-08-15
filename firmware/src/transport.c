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
	/*
	 * Note this does not clear host_seen. The latch survives a goodbye on purpose (D117): a daemon
	 * announcing its departure tells us nothing about whether the Wi-Fi source reports the same
	 * machine's work, so releasing would be the silent substitution this design exists to avoid.
	 */
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

	/*
	 * The latch. Once a host has spoken over the wire this device is its, for the rest of the boot,
	 * whether or not it is still talking — see transport.h for why falling back to Wi-Fi would be
	 * answering a different question rather than recovering.
	 *
	 * Trust still comes and goes with the heartbeat, so a quiet host means amber rather than a stale
	 * lamp. What does not come back is the *choice*: escaping needs a reset, and unplugging the cable
	 * is one, because the cable is also the power.
	 */
	if (t->host_seen) {
		t->active = TRANSPORT_USB;
		t->trusted = host_fresh(t, now_ms);

		if (t->trusted) {
			t->reason = TRANSPORT_REASON_OK;
		} else {
			t->reason = t->host_bye ? TRANSPORT_REASON_HOST_BYE
						: TRANSPORT_REASON_HOST_QUIET;
		}

		if (was != TRANSPORT_USB) {
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
	case TRANSPORT_REASON_WIFI_DOWN:
		return "wifi down";
	case TRANSPORT_REASON_NOTHING:
		return "nothing configured";
	case TRANSPORT_REASON_STARTING:
	default:
		return "starting";
	}
}
