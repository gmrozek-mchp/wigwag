/*
 * Host unit tests for link supervision.
 *
 * The policy under test is deliberately pessimistic: LINKED requires positive evidence of every
 * hop, and anything unproven is UNLINKED. These tests exist because the failure they guard against
 * was observed on real hardware — a device sitting in READY, reporting LINKED, holding a stale
 * state, with nothing on the other end of the wire (D75).
 *
 *   make -C firmware/tests link
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../src/link.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond, ...)                                                                           \
	do {                                                                                       \
		checks++;                                                                          \
		if (!(cond)) {                                                                      \
			failures++;                                                                \
			printf("  FAIL %s:%d: ", __func__, __LINE__);                              \
			printf(__VA_ARGS__);                                                       \
			printf("\n");                                                              \
		}                                                                                  \
	} while (0)

#define HOST_TOPIC "wigwag/host_online"

static struct link l;

static void setup(void)
{
	link_init(&l, LINK_HOST_GRACE_MS);
}

/* Drive to a fully trusted link the way the real callbacks would. */
static void bring_up(uint32_t t)
{
	link_note_at(&l, true, t);
	link_note_message(&l, HOST_TOPIC, "1", HOST_TOPIC, t);
}

static void test_starts_untrusted(void)
{
	setup();

	CHECK(!link_is_trusted(&l), "trusted before anything was established");
	CHECK(l.reason == LINK_REASON_STARTING, "reason %s", link_reason_str(l.reason));
}

static void test_at_alone_is_not_enough(void)
{
	/*
	 * The heart of it: the module being connected does not mean the state on the lamps is
	 * being produced by anyone. A broker with no daemon behind it is not a link.
	 */
	setup();
	link_note_at(&l, true, 1000);

	CHECK(!link_is_trusted(&l), "trusted on the AT client's word alone");

	/* Within the grace period it is merely starting, not yet a failure. */
	link_tick(&l, 1000 + LINK_HOST_GRACE_MS - 1U);
	CHECK(!link_is_trusted(&l), "trusted during the grace period");
	CHECK(l.reason == LINK_REASON_STARTING, "reason %s during grace",
	      link_reason_str(l.reason));

	/* Past it, the host's silence is a definite fault. */
	link_tick(&l, 1000 + LINK_HOST_GRACE_MS + 1U);
	CHECK(!link_is_trusted(&l), "trusted with a host that never spoke");
	CHECK(l.reason == LINK_REASON_HOST_UNKNOWN, "reason %s past grace",
	      link_reason_str(l.reason));
}

static void test_full_evidence_is_trusted(void)
{
	setup();
	bring_up(1000);

	CHECK(link_is_trusted(&l), "not trusted with both hops proven");
	CHECK(l.reason == LINK_REASON_OK, "reason %s", link_reason_str(l.reason));
}

static void test_host_last_will_untrusts(void)
{
	/* The daemon dies; the broker publishes its Last Will. Module is perfectly healthy. */
	setup();
	bring_up(1000);

	link_note_message(&l, HOST_TOPIC, "0", HOST_TOPIC, 2000);

	CHECK(!link_is_trusted(&l), "still trusted after host_online = 0");
	CHECK(l.reason == LINK_REASON_HOST_GONE, "reason %s", link_reason_str(l.reason));
}

static void test_at_loss_untrusts(void)
{
	setup();
	bring_up(1000);

	link_note_at(&l, false, 2000);

	CHECK(!link_is_trusted(&l), "still trusted after the AT client dropped");
	CHECK(l.reason == LINK_REASON_AT_DOWN, "reason %s", link_reason_str(l.reason));
}

static void test_reconnect_requires_fresh_host_evidence(void)
{
	/*
	 * A new AT session means a new subscription, so the old host_online reading says nothing
	 * about it. Trusting the stale value would reintroduce exactly the bug this module exists
	 * to prevent, one layer up.
	 */
	setup();
	bring_up(1000);
	CHECK(link_is_trusted(&l), "precondition failed");

	link_note_at(&l, false, 2000);
	link_note_at(&l, true, 3000);

	CHECK(!link_is_trusted(&l), "trusted a reconnect using the previous host reading");

	link_note_message(&l, HOST_TOPIC, "1", HOST_TOPIC, 3100);
	CHECK(link_is_trusted(&l), "not trusted after fresh host evidence");
}

static void test_unexpected_payload_is_not_alive(void)
{
	/* Guessing optimistically about an unparseable payload is how a lamp lies. */
	const char *junk[] = { "", "0", "yes", "1 ", "true", "01" };
	size_t i;

	for (i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
		setup();
		link_note_at(&l, true, 1000);
		link_note_message(&l, HOST_TOPIC, junk[i], HOST_TOPIC, 1100);

		CHECK(!link_is_trusted(&l), "trusted host_online payload \"%s\"", junk[i]);
	}
}

static void test_other_topics_pass_through(void)
{
	setup();
	bring_up(1000);

	CHECK(!link_note_message(&l, "wigwag/state", "{\"state\":\"BUSY\"}", HOST_TOPIC, 2000),
	      "consumed a message that belongs to lamp.c");
	CHECK(link_is_trusted(&l), "a state message disturbed the link condition");
}

static void test_transitions_counted(void)
{
	setup();
	bring_up(1000);
	link_note_message(&l, HOST_TOPIC, "0", HOST_TOPIC, 2000);
	link_note_message(&l, HOST_TOPIC, "1", HOST_TOPIC, 3000);

	CHECK(l.transitions == 3U, "counted %u transitions, expected 3", l.transitions);
}

int main(void)
{
	printf("link host tests\n");

	test_starts_untrusted();
	test_at_alone_is_not_enough();
	test_full_evidence_is_trusted();
	test_host_last_will_untrusts();
	test_at_loss_untrusts();
	test_reconnect_requires_fresh_host_evidence();
	test_unexpected_payload_is_not_alive();
	test_other_topics_pass_through();
	test_transitions_counted();

	printf("%d checks, %d failures\n", checks, failures);
	return (failures == 0) ? 0 : 1;
}
