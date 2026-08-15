/*
 * Which way is the truth arriving, and may it be believed?
 *
 * **The transport is configured, not inferred** (ADR-0022, D119). `set transport usb|wifi` decides who
 * owns the lamps; nothing the outside world does can change it. This file then answers only the second
 * question — is the configured transport currently trustworthy — which is the part that genuinely
 * depends on evidence.
 *
 * That is the third shape this has taken in two days, and the reasoning is worth keeping. It began as
 * "whichever side is talking wins", which meant a cable plugged in for power or for configuration
 * silently repurposed the device. It then latched, so at least the substitution could not happen
 * twice — but automatic *and* irreversible is worse than either alone. The mistake in both was treating
 * the choice as something to detect. ADR-0013 had already settled the pattern for the broker address —
 * configured, not auto-discovered — and this should have followed it from the start.
 *
 * What that buys beyond not surprising anyone: no silent substitution is possible in either direction.
 * A configured transport that is not working shows the fail-visible pattern rather than quietly
 * answering with a different machine's work (Rule 4, ADR-0007) — and the two transports genuinely do
 * not report the same thing, since a daemon aggregates the sessions of *its own machine* (D30).
 *
 * link.c remains the Wi-Fi supervisor and feeds its verdict in here; this sits above it.
 *
 * **Why the wired path needs a heartbeat and MQTT does not.** Over MQTT the host publishes
 * `host_online` once, retained, and registers `0` as its Last Will; the broker holds the value for a
 * late subscriber and reports the death on the host's behalf. A serial line has neither — nothing
 * retains, and nothing notices a daemon that stops. So the wired path demands *periodic* positive
 * evidence, which is what D75 concluded the hard way: absence of bad news is not evidence of a link.
 *
 * Pure logic, injected clock, no Zephyr — host-tested rather than discovered on a desk.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TRANSPORT_H
#define TRANSPORT_H

#include "settings.h"

#include <stdbool.h>
#include <stdint.h>

enum transport_kind {
	TRANSPORT_NONE = 0,	/* Wi-Fi is the configured transport, but there is no SSID to try */
	TRANSPORT_USB,		/* console serial, via the bridge on the product (D102) */
	TRANSPORT_WIFI,		/* the RNWF02 and MQTT, as ADR-0002 and ADR-0003 */
};

/** Diagnostic only. Behaviour depends on the kind and on `trusted`, never on the reason. */
enum transport_reason {
	TRANSPORT_REASON_STARTING = 0,
	TRANSPORT_REASON_OK,
	TRANSPORT_REASON_HOST_QUIET,	/* wired, and the host has stopped talking */
	TRANSPORT_REASON_HOST_BYE,	/* wired, and the host said goodbye: an orderly `host off` */
	TRANSPORT_REASON_WIFI_DOWN,	/* wireless, and link.c does not trust it */
	TRANSPORT_REASON_NOTHING,	/* wireless, and no SSID is configured */
};

/**
 * How long a host may be silent before the wired path stops being believed.
 *
 * Five missed beats at the daemon's 2 s cadence, and the same 10 s budget D34 sets for the MQTT path —
 * so both transports go fail-visible on the same schedule, which is one number to remember.
 */
#define TRANSPORT_HOST_TTL_MS 10000U

struct transport {
	/** From settings, fixed for this boot. Changing it means `save` and a reboot. */
	enum wigwag_transport configured;

	enum transport_kind active;
	bool trusted;
	enum transport_reason reason;

	/* Wired evidence */
	bool host_seen;			/* has a host ever spoken? distinguishes STARTING from QUIET */
	bool host_bye;			/* an orderly goodbye, believed until it speaks again */
	uint32_t host_last_ms;

	/* Wireless evidence */
	bool wifi_configured;		/* an SSID exists to try */
	bool wifi_trusted;		/* link.c's verdict */
};

void transport_init(struct transport *t, enum wigwag_transport configured, bool wifi_configured);

/**
 * A host spoke on the console — `host on` or `state`, per cmd_is_host_activity().
 *
 * Configuration commands deliberately do not count: the console and the wired transport share one
 * wire, so a person setting up Wi-Fi types on the line a daemon would use. This only ever affects
 * *trust*, never which transport owns the device.
 */
void transport_note_host(struct transport *t, uint32_t now_ms);

/** The host said goodbye — drops trust at once rather than waiting out the timeout. */
void transport_note_host_bye(struct transport *t, uint32_t now_ms);

/** link.c's verdict for the Wi-Fi path. */
void transport_note_wifi(struct transport *t, bool trusted, uint32_t now_ms);

/** Recompute trust. Call regularly; the timeout needs a clock. */
void transport_tick(struct transport *t, uint32_t now_ms);

static inline bool transport_is_trusted(const struct transport *t)
{
	return t->trusted;
}

static inline enum transport_kind transport_active(const struct transport *t)
{
	return t->active;
}

/**
 * True when the module should be brought up at all.
 *
 * A wired device never starts the RNWF02: there is nothing for it to do, and letting it reset, time
 * out and back off forever against a network it will never use is waste an end-to-end session measured
 * at 947 AT timeouts (D118).
 */
static inline bool transport_wants_module(const struct transport *t)
{
	return t->configured == WIGWAG_TRANSPORT_WIFI;
}

const char *transport_kind_str(enum transport_kind kind);
const char *transport_reason_str(enum transport_reason reason);

#endif /* TRANSPORT_H */
