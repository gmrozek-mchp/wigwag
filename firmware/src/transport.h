/*
 * Which way is the truth arriving, and may it be believed?
 *
 * ADR-0018 decided the device runs **one transport at a time**, chosen from evidence rather than
 * configuration: USB serial when a live host is talking, Wi-Fi and MQTT otherwise. This is that
 * decision as a state machine (D104). It sits above link.c rather than replacing it — link.c
 * supervises the MQTT path and answers "is the Wi-Fi side trustworthy", which is one of the inputs
 * here.
 *
 * **The USB claim latches for the rest of the boot (D117).** Once a host has spoken over the wire,
 * this device belongs to it until reset. That is not caution about flapping; it is because the two
 * transports do not carry the same information. A daemon aggregates the sessions of *its own machine*
 * (D30), so falling back from USB to Wi-Fi would not restore the display — it would silently start
 * answering a different question, swapping "what is my laptop doing" for "what is some other machine
 * doing" with nothing to mark the substitution. Amber means "I do not know what your laptop is
 * doing", and that is more honest than a confident lamp about somebody else's work (Rule 4,
 * ADR-0007).
 *
 * The latch costs nothing to escape, because this device is USB-powered (ADR-0009): **pulling the
 * cable is a power cycle**, so the obvious physical gesture already clears it. A `reboot` command or
 * a watchdog reset clears it too, being per-boot state.
 *
 * **Why USB needs a heartbeat when MQTT does not.** Over MQTT the host publishes `host_online` once,
 * retained, and registers `0` as its Last Will; the broker holds the value for a late subscriber and
 * reports the death on the host's behalf. A serial line has neither — nothing retains, and nothing
 * notices a daemon that stops. So the wired path demands *periodic* positive evidence, which is
 * exactly what D75 concluded the hard way: absence of bad news is not evidence of a link.
 *
 * The bridge's `USBCFG` pin is deliberately **not** an input here (D116). It cannot close that gap —
 * a computer whose daemon has crashed still enumerates — and with the latch above it has no remaining
 * job either, since there is no release for it to accelerate. Received bytes are both necessary and
 * sufficient.
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

/** Diagnostic only. Behaviour depends on the kind and on `trusted`, never on the reason. */
enum transport_reason {
	TRANSPORT_REASON_STARTING = 0,
	TRANSPORT_REASON_OK,
	TRANSPORT_REASON_HOST_QUIET,	/* USB held the device and the host stopped talking */
	TRANSPORT_REASON_HOST_BYE,	/* the host said goodbye: an orderly `host off` */
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

struct transport {
	enum transport_kind active;
	bool trusted;
	enum transport_reason reason;

	/*
	 * USB evidence. `host_seen` is the latch: once a host has spoken it stays set for the rest of
	 * the boot, and the device stays on USB whether or not that host is still there.
	 */
	bool host_seen;
	bool host_bye;			/* an orderly goodbye, believed until it speaks again */
	uint32_t host_last_ms;

	/* Wi-Fi evidence */
	bool wifi_configured;		/* an SSID exists to try */
	bool wifi_trusted;		/* link.c's verdict */

	uint32_t handovers;		/* diagnostics: how often the active transport changed */
};

void transport_init(struct transport *t, bool wifi_configured);

/**
 * A host spoke on the console — specifically `host` or `state`, per cmd_is_host_activity().
 *
 * Configuration commands deliberately do not count: the console and the transport share one wire, so a
 * person setting up Wi-Fi types on the line a daemon would use, and treating that as host activity
 * made the device claim itself mid-configuration and then flicker amber when they paused to read.
 */
void transport_note_host(struct transport *t, uint32_t now_ms);

/**
 * The host said goodbye. The serial analogue of `host_online` going to 0.
 *
 * Drops trust at once rather than waiting out the timeout, but **does not release the latch** (D117):
 * "I am going away" says nothing about whether the Wi-Fi source is the same information, so handing
 * over would be the same silent substitution by a politer route.
 */
void transport_note_host_bye(struct transport *t, uint32_t now_ms);

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

/**
 * True once USB has latched, so the caller can stop servicing the Wi-Fi path entirely (D118).
 *
 * Not merely an optimisation: while latched the Wi-Fi path can never be selected again this boot, so
 * continuing to retry it is pure waste — an end-to-end session accumulated 947 AT timeouts doing
 * exactly that.
 */
static inline bool transport_usb_holds(const struct transport *t)
{
	return t->active == TRANSPORT_USB;
}

const char *transport_kind_str(enum transport_kind kind);
const char *transport_reason_str(enum transport_reason reason);

#endif /* TRANSPORT_H */
