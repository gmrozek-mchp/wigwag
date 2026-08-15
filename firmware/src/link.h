/*
 * Link supervision — whether the device is allowed to believe what it is showing.
 *
 * CONTEXT.md calls this the *link condition*, deliberately not a state: LINKED or UNLINKED. It is
 * the input to the fail-visible rule (Rule 4, ADR-0007) — UNLINKED must drive the wigwag
 * rather than leave a stale lamp lit.
 *
 * The AT client reports its own connection progress, but that is not sufficient on its own. There
 * are three independent ways to lose the truth, and each needs its own evidence:
 *
 *   module or UART dies    -> the AT client's keepalive poll stops being answered
 *   broker or Wi-Fi dies   -> the module reports +MQTTCONN:0
 *   host daemon dies       -> wigwag/host_online goes to 0 via the daemon's Last Will
 *
 * The first two are the AT client's business and arrive here as on_link(false). The third is a
 * subscribed topic, and is why this module exists rather than the AT client simply owning the
 * answer: a module happily connected to a broker with nothing publishing to it is *not* a link
 * worth trusting, and only the application knows that.
 *
 * Demonstrated need: with its module process killed, the device sat in READY reporting LINKED for
 * fourteen seconds holding a stale state (D75). Absence of bad news is not evidence of a link.
 *
 * No allocation, no Zephyr dependency, clock injected — same rules as rnwf_at.c, so this is
 * testable on the host too.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LINK_H
#define LINK_H

#include <stdbool.h>
#include <stdint.h>

/** The link condition, in CONTEXT.md's vocabulary. */
enum link_condition {
	LINK_UNLINKED = 0,
	LINK_LINKED,
};

/**
 * Why the link is not trusted. Diagnostic only — behaviour depends on the condition, never on the
 * reason — but on a device whose only output is three lamps, knowing which one failed is worth the
 * bytes.
 */
enum link_reason {
	LINK_REASON_STARTING = 0,	/* nothing has been established yet */
	LINK_REASON_AT_DOWN,		/* module, UART, Wi-Fi or broker: the AT client said so */
	LINK_REASON_HOST_GONE,		/* host_online reported 0 */
	LINK_REASON_HOST_UNKNOWN,	/* connected, but the host has never been heard from */
	LINK_REASON_OK,
};

struct link {
	bool at_linked;
	bool host_online;
	bool host_known;

	enum link_condition condition;
	enum link_reason reason;

	/*
	 * Grace period after the AT client links, before demanding to have heard from the host.
	 * The retained host_online arrives a moment after subscribing, and flapping UNLINKED ->
	 * LINKED in that window would show a spurious wigwag on every connect.
	 */
	uint32_t linked_at_ms;
	uint32_t host_grace_ms;

	uint32_t transitions;
};

/** Grace period for the retained host_online to arrive after subscribing. */
#define LINK_HOST_GRACE_MS 3000

void link_init(struct link *l, uint32_t host_grace_ms);

/** Feed the AT client's on_link callback straight through. */
void link_note_at(struct link *l, bool at_linked, uint32_t now_ms);

/**
 * Offer a received message. Returns true if it was the host liveness topic and was consumed.
 * Anything else is left for the caller — the state topic belongs to lamp.c.
 */
bool link_note_message(struct link *l, const char *topic, const char *payload,
		       const char *host_online_topic, uint32_t now_ms);

/** Recompute the condition. Call regularly; the grace period needs a clock. */
void link_tick(struct link *l, uint32_t now_ms);

static inline enum link_condition link_get(const struct link *l)
{
	return l->condition;
}

static inline bool link_is_trusted(const struct link *l)
{
	return l->condition == LINK_LINKED;
}

const char *link_reason_str(enum link_reason reason);

#endif /* LINK_H */
