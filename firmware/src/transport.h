/*
 * Which way is the truth arriving, and may it be believed?
 *
 * ADR-0018 decided the device runs **one transport at a time**, chosen from evidence rather than
 * configuration: USB serial when a live host is talking, Wi-Fi and MQTT otherwise. This is that
 * decision as a state machine (D104). It sits above link.c rather than replacing it — link.c
 * supervises the MQTT path and answers "is the Wi-Fi side trustworthy", which is one of the inputs
 * here.
 *
 * **Why USB needs a heartbeat when MQTT does not.** Over MQTT the host publishes `host_online` once,
 * retained, and registers `0` as its Last Will; the broker holds the value for a late subscriber and
 * reports the death on the host's behalf. A serial line has neither — nothing retains, and nothing
 * notices a daemon that stops. `USBCFG` does not close the gap either: a computer whose daemon has
 * crashed still enumerates. So the wired path demands *periodic* positive evidence, which is exactly
 * what D75 concluded the hard way: absence of bad news is not evidence of a link.
 *
 * Pure logic, injected clock, no Zephyr — so the handover rules are host-tested rather than discovered
 * on a desk.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

enum transport_kind {
	TRANSPORT_NONE = 0,	/* nothing configured and nothing talking */
	TRANSPORT_USB,		/* console serial, via the MCP2221A on the product (D102) */
	TRANSPORT_WIFI,		/* the RNWF02 and MQTT, as ADR-0002 and ADR-0003 */
};

/**
 * What the bridge's `USBCFG` pin says (D103).
 *
 * `UNKNOWN` is the normal answer on the Curiosity Nano, which has no such pin — its console comes
 * from the on-board debugger. The machine below is built so that absence degrades cleanly: received
 * bytes are the authoritative evidence of a live host, and the pin only makes *loss* detectable
 * sooner than the heartbeat timeout would.
 */
enum transport_usb_cfg {
	TRANSPORT_USB_UNKNOWN = 0,
	TRANSPORT_USB_ABSENT,	/* enumeration lost: cable out, or a charger that never enumerated */
	TRANSPORT_USB_PRESENT,
};

/** Diagnostic only. Behaviour depends on the kind and on `trusted`, never on the reason. */
enum transport_reason {
	TRANSPORT_REASON_STARTING = 0,
	TRANSPORT_REASON_OK,
	TRANSPORT_REASON_HOST_QUIET,	/* USB held the device and the host stopped talking */
	TRANSPORT_REASON_HOST_BYE,	/* the host said goodbye: an orderly `host off` */
	TRANSPORT_REASON_UNPLUGGED,	/* USBCFG deasserted, which is faster than waiting for the TTL */
	TRANSPORT_REASON_WIFI_DOWN,	/* Wi-Fi is the active transport and link.c does not trust it */
	TRANSPORT_REASON_NOTHING,	/* no Wi-Fi configured and no host talking */
};

/**
 * How long a host may be silent before the wired path stops being believed.
 *
 * Five missed beats at the daemon's existing 2 s cadence, and the same 10 s budget D34 sets for the
 * MQTT path — so both transports go fail-visible on the same schedule, which is one number to
 * remember rather than two.
 */
#define TRANSPORT_HOST_TTL_MS 10000U

/**
 * How long USB keeps the device after going quiet, before handing back to Wi-Fi.
 *
 * The device is **untrusted** for this whole window — the honest "I do not know" — rather than
 * switching straight to a Wi-Fi state that may itself be stale. Long enough that a host pausing for
 * breath does not cause a handover, short enough that unplugging the cable recovers promptly.
 */
#define TRANSPORT_RELEASE_MS 5000U

struct transport {
	enum transport_kind active;
	bool trusted;
	enum transport_reason reason;

	/* USB evidence */
	enum transport_usb_cfg usb_cfg;
	bool host_seen;			/* has a host ever spoken on this boot? */
	bool host_bye;			/* an orderly goodbye, believed until it speaks again */
	uint32_t host_last_ms;

	/* Wi-Fi evidence */
	bool wifi_configured;		/* an SSID exists to try */
	bool wifi_trusted;		/* link.c's verdict */

	uint32_t usb_claimed_ms;	/* when USB last held the device, for the release window */
	uint32_t handovers;		/* diagnostics: how often the active transport changed */
};

void transport_init(struct transport *t, bool wifi_configured);

/**
 * A host spoke on the console. Any recognised command counts — it is positive evidence that something
 * is alive and talking, which is the whole test.
 */
void transport_note_host(struct transport *t, uint32_t now_ms);

/** The host said goodbye. The serial analogue of `host_online` going to 0. */
void transport_note_host_bye(struct transport *t, uint32_t now_ms);

/** The bridge's `USBCFG` pin, or `UNKNOWN` on a board without one. */
void transport_note_usb_cfg(struct transport *t, enum transport_usb_cfg cfg, uint32_t now_ms);

/** link.c's verdict for the Wi-Fi path. */
void transport_note_wifi(struct transport *t, bool trusted, uint32_t now_ms);

/** Recompute. Call regularly; the timeouts need a clock. */
void transport_tick(struct transport *t, uint32_t now_ms);

static inline bool transport_is_trusted(const struct transport *t)
{
	return t->trusted;
}

static inline enum transport_kind transport_active(const struct transport *t)
{
	return t->active;
}

/** True while USB holds the device, so the caller can skip Wi-Fi bring-up entirely (ADR-0018). */
static inline bool transport_usb_holds(const struct transport *t)
{
	return t->active == TRANSPORT_USB;
}

const char *transport_kind_str(enum transport_kind kind);
const char *transport_reason_str(enum transport_reason reason);

#endif /* TRANSPORT_H */
