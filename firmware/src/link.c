/*
 * Link supervision. See link.h for why this is separate from the AT client.
 *
 * The rule is deliberately conservative: LINKED requires positive evidence of every hop, and
 * anything unproven counts as UNLINKED. Fail-visible means erring toward the amber flicker, never
 * toward a confident lamp (Rule 4, ADR-0007).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "link.h"

#include <string.h>

void link_init(struct link *l, uint32_t host_grace_ms)
{
	memset(l, 0, sizeof(*l));
	l->host_grace_ms = (host_grace_ms != 0U) ? host_grace_ms : LINK_HOST_GRACE_MS;
	l->condition = LINK_UNLINKED;
	l->reason = LINK_REASON_STARTING;
}

static void evaluate(struct link *l, uint32_t now_ms)
{
	enum link_condition was = l->condition;

	if (!l->at_linked) {
		l->condition = LINK_UNLINKED;
		/*
		 * Keep STARTING until something has actually been up, so a device that has never
		 * connected is distinguishable from one that lost a working link.
		 */
		l->reason = (l->reason == LINK_REASON_STARTING) ? LINK_REASON_STARTING
								: LINK_REASON_AT_DOWN;
	} else if (l->host_known && !l->host_online) {
		/*
		 * The module is connected and the broker is answering, but the daemon is gone — its
		 * Last Will said so. Nothing is producing state, so whatever is on the lamps is
		 * already going stale. This is the case a module-only keepalive cannot see.
		 */
		l->condition = LINK_UNLINKED;
		l->reason = LINK_REASON_HOST_GONE;
	} else if (!l->host_known) {
		/*
		 * Connected but the host has never been heard from. Allow a grace period for the
		 * retained host_online to arrive, then stop pretending.
		 */
		if ((int32_t)(now_ms - (l->linked_at_ms + l->host_grace_ms)) >= 0) {
			l->condition = LINK_UNLINKED;
			l->reason = LINK_REASON_HOST_UNKNOWN;
		} else {
			l->condition = LINK_UNLINKED;
			l->reason = LINK_REASON_STARTING;
		}
	} else {
		l->condition = LINK_LINKED;
		l->reason = LINK_REASON_OK;
	}

	if (l->condition != was) {
		l->transitions++;
	}
}

void link_note_at(struct link *l, bool at_linked, uint32_t now_ms)
{
	if (at_linked && !l->at_linked) {
		l->linked_at_ms = now_ms;

		/*
		 * A fresh AT session means a fresh subscription, so the previous host_online reading
		 * is not evidence about this one. Forget it and wait to be told again — the retained
		 * value arrives within the grace period.
		 */
		l->host_known = false;
		l->host_online = false;
	}

	l->at_linked = at_linked;
	evaluate(l, now_ms);
}

bool link_note_message(struct link *l, const char *topic, const char *payload,
		       const char *host_online_topic, uint32_t now_ms)
{
	if (host_online_topic == NULL || topic == NULL || payload == NULL) {
		return false;
	}

	if (strcmp(topic, host_online_topic) != 0) {
		return false;
	}

	/*
	 * CONTEXT.md defines the payload as "1" or "0". Treat exactly "1" as alive and everything
	 * else — including anything unexpected — as not alive, because guessing in the optimistic
	 * direction is what produces a confidently wrong lamp.
	 */
	l->host_online = (strcmp(payload, "1") == 0);
	l->host_known = true;
	evaluate(l, now_ms);

	return true;
}

void link_tick(struct link *l, uint32_t now_ms)
{
	evaluate(l, now_ms);
}

const char *link_reason_str(enum link_reason reason)
{
	switch (reason) {
	case LINK_REASON_STARTING:
		return "starting";
	case LINK_REASON_AT_DOWN:
		return "module/broker down";
	case LINK_REASON_HOST_GONE:
		return "host gone";
	case LINK_REASON_HOST_UNKNOWN:
		return "host never seen";
	case LINK_REASON_OK:
		return "ok";
	default:
		return "?";
	}
}
