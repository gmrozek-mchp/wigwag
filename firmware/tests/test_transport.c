/*
 * Host unit tests for transport selection.
 *
 * The rule under test is the latch (D117): once a host has spoken over the wire, this device is its
 * until reset, because the two transports do not carry the same information. What still moves is
 * *trust* — a quiet host means amber, not a stale lamp. Getting either half wrong would look like a
 * flickering lamp or, worse, a confident lamp about the wrong machine's work.
 *
 *   make -C firmware/tests transport
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../src/transport.h"

#include <stdio.h>

static int failures;
static int checks;

#define CHECK(cond, ...)                                                                           \
	do {                                                                                       \
		checks++;                                                                          \
		if (!(cond)) {                                                                     \
			failures++;                                                                \
			printf("  FAIL %s:%d: ", __func__, __LINE__);                              \
			printf(__VA_ARGS__);                                                       \
			printf("\n");                                                              \
		}                                                                                  \
	} while (0)

static struct transport t;

static void test_starts_on_nothing_untrusted(void)
{
	/* A device that boots believing itself connected lies until proven otherwise. */
	transport_init(&t, true);
	CHECK(transport_active(&t) == TRANSPORT_NONE, "starts on no transport");
	CHECK(!transport_is_trusted(&t), "and untrusted");
	CHECK(t.reason == TRANSPORT_REASON_STARTING, "reason is starting");
}

static void test_wifi_is_the_default_when_configured(void)
{
	transport_init(&t, true);
	transport_tick(&t, 1000);
	CHECK(transport_active(&t) == TRANSPORT_WIFI, "wifi selected when configured");
	CHECK(!transport_is_trusted(&t), "but not trusted until link.c says so");
	CHECK(t.reason == TRANSPORT_REASON_WIFI_DOWN, "reason names the wifi");

	transport_note_wifi(&t, true, 1000);
	transport_tick(&t, 1000);
	CHECK(transport_is_trusted(&t), "trusted once link.c agrees");
	CHECK(t.reason == TRANSPORT_REASON_OK, "reason ok");
}

static void test_nothing_configured_is_honest(void)
{
	/* No SSID and no host: the device must say so rather than pretend to be trying. */
	transport_init(&t, false);
	transport_tick(&t, 1000);
	CHECK(transport_active(&t) == TRANSPORT_NONE, "no transport");
	CHECK(!transport_is_trusted(&t), "untrusted");
	CHECK(t.reason == TRANSPORT_REASON_NOTHING, "reason says nothing is configured");
}

static void test_a_talking_host_claims_the_device(void)
{
	transport_init(&t, true);
	transport_note_wifi(&t, true, 0);
	transport_tick(&t, 1000);
	CHECK(transport_active(&t) == TRANSPORT_WIFI, "wifi first");

	/* One command from a host is enough, and it wins even against a healthy Wi-Fi link. */
	transport_note_host(&t, 2000);
	transport_tick(&t, 2000);
	CHECK(transport_active(&t) == TRANSPORT_USB, "usb takes over");
	CHECK(transport_is_trusted(&t), "and is trusted");
	CHECK(transport_usb_holds(&t), "usb_holds reports the same thing");
	CHECK(t.handovers == 2, "one handover to wifi, one to usb (%u)", t.handovers);
}

static void test_usb_claims_with_no_usbcfg_pin(void)
{
	/*
	 * The Curiosity Nano case, and the one that has to work for any of this to be testable: no
	 * USBCFG pin exists, so received bytes are the only evidence — and they must suffice.
	 */
	transport_init(&t, true);
	transport_note_host(&t, 500);
	transport_tick(&t, 500);
	CHECK(transport_active(&t) == TRANSPORT_USB && transport_is_trusted(&t),
	      "usb claims on bytes alone");
}

static void test_quiet_host_loses_trust_but_keeps_the_device(void)
{
	transport_init(&t, true);
	transport_note_wifi(&t, true, 0);
	transport_note_host(&t, 1000);
	transport_tick(&t, 1000);
	CHECK(transport_is_trusted(&t), "trusted while talking");

	/* Still fresh at exactly the TTL. */
	transport_tick(&t, 1000 + TRANSPORT_HOST_TTL_MS);
	CHECK(transport_is_trusted(&t), "still trusted at the TTL boundary");

	/* One millisecond past: amber, immediately. */
	transport_tick(&t, 1000 + TRANSPORT_HOST_TTL_MS + 1U);
	CHECK(!transport_is_trusted(&t), "untrusted the moment the host is stale");
	CHECK(t.reason == TRANSPORT_REASON_HOST_QUIET, "reason says the host went quiet");

	/*
	 * The heart of it. Wi-Fi is healthy the entire time and the device **never** switches to it,
	 * no matter how long the host stays silent, because Wi-Fi would be reporting a different
	 * machine's work rather than recovering this one's.
	 */
	transport_tick(&t, 1000 + 60000U);
	CHECK(transport_active(&t) == TRANSPORT_USB, "still on usb after a minute of silence");
	CHECK(!transport_is_trusted(&t), "and still honestly untrusted");

	/* And no churn while it sits there: the count must not move at all. */
	{
		uint32_t before = t.handovers;
		uint32_t now;

		for (now = 1000 + 60000U; now < 1000 + 3600000U; now += 5000U) {
			transport_tick(&t, now);
		}

		CHECK(transport_active(&t) == TRANSPORT_USB, "still on usb after an hour");
		CHECK(!transport_is_trusted(&t), "still untrusted");
		CHECK(t.handovers == before, "no churn across an hour of silence (%u -> %u)", before,
		      t.handovers);
	}
}

static void test_only_a_reset_clears_the_latch(void)
{
	/*
	 * transport_init() is what a reset does. Escaping the latch in the field means power-cycling,
	 * and since the cable carries the power, unplugging it is exactly that (ADR-0009).
	 */
	transport_init(&t, true);
	transport_note_host(&t, 1000);
	transport_tick(&t, 1000);
	CHECK(transport_active(&t) == TRANSPORT_USB, "latched");

	transport_init(&t, true);
	transport_note_wifi(&t, true, 0);
	transport_tick(&t, 0);
	CHECK(transport_active(&t) == TRANSPORT_WIFI, "a reset returns to wifi");
	CHECK(transport_is_trusted(&t), "and trusts it");
}

static void test_host_returning_regains_trust(void)
{
	transport_init(&t, true);
	transport_note_host(&t, 1000);
	transport_tick(&t, 1000);
	transport_tick(&t, 20000);
	CHECK(!transport_is_trusted(&t), "gone quiet");

	transport_note_host(&t, 20500);
	transport_tick(&t, 20500);
	CHECK(transport_active(&t) == TRANSPORT_USB && transport_is_trusted(&t),
	      "one word from the host restores trust");
	CHECK(t.handovers == 1, "and it was never a handover, just trust returning (%u)", t.handovers);
}

static void test_goodbye_drops_trust_without_releasing(void)
{
	transport_init(&t, true);
	transport_note_wifi(&t, true, 0);
	transport_note_host(&t, 1000);
	transport_tick(&t, 1000);
	CHECK(transport_is_trusted(&t), "trusted");

	/*
	 * An orderly goodbye is faster than the timeout, but it is not a release: "I am going away"
	 * says nothing about whether the Wi-Fi source reports the same machine (D117).
	 */
	transport_note_host_bye(&t, 2000);
	transport_tick(&t, 2000);
	CHECK(!transport_is_trusted(&t), "untrusted at once, not after 10 s");
	CHECK(t.reason == TRANSPORT_REASON_HOST_BYE, "reason distinguishes a goodbye from silence");
	CHECK(transport_active(&t) == TRANSPORT_USB, "still latched to usb");

	transport_tick(&t, 100000);
	CHECK(transport_active(&t) == TRANSPORT_USB, "and stays latched indefinitely");
}

static void test_talking_again_cancels_a_goodbye(void)
{
	transport_init(&t, false);
	transport_note_host(&t, 1000);
	transport_note_host_bye(&t, 2000);
	transport_tick(&t, 2000);
	CHECK(!transport_is_trusted(&t), "goodbye honoured");

	transport_note_host(&t, 2500);
	transport_tick(&t, 2500);
	CHECK(transport_is_trusted(&t), "newer evidence wins over an older goodbye");
	CHECK(t.reason == TRANSPORT_REASON_OK, "and the reason follows");
}

static void test_wifi_losing_trust_does_not_hand_over(void)
{
	/* There is nowhere to hand over *to*. Wi-Fi stays active and simply is not believed. */
	transport_init(&t, true);
	transport_note_wifi(&t, true, 0);
	transport_tick(&t, 1000);
	CHECK(transport_is_trusted(&t), "trusted");

	transport_note_wifi(&t, false, 2000);
	transport_tick(&t, 2000);
	CHECK(transport_active(&t) == TRANSPORT_WIFI, "still on wifi");
	CHECK(!transport_is_trusted(&t), "but untrusted");
	CHECK(t.reason == TRANSPORT_REASON_WIFI_DOWN, "reason says wifi");
}

static void test_never_trusted_without_evidence(void)
{
	/*
	 * The invariant worth stating outright: at no point may the device be trusted while neither
	 * source has produced positive evidence. Swept across a long timeline with nothing talking.
	 */
	uint32_t now;

	transport_init(&t, true);
	for (now = 0; now < 60000; now += 250) {
		transport_tick(&t, now);
		CHECK(!transport_is_trusted(&t) || t.reason == TRANSPORT_REASON_OK,
		      "trusted with no evidence at %u", now);
		if (transport_is_trusted(&t)) {
			failures++;
			printf("  FAIL trusted with no evidence at all at %u\n", now);
			break;
		}
	}
}

static void test_clock_wrap(void)
{
	/* Milliseconds wrap after ~49 days; a handover must not fire spuriously when they do. */
	uint32_t base = 0xFFFFF000U;
	uint32_t i;

	transport_init(&t, true);
	for (i = 0; i < 200U; i++) {
		uint32_t now = base + (i * 100U);	/* wraps partway through */

		transport_note_host(&t, now);
		transport_tick(&t, now);
		CHECK(transport_active(&t) == TRANSPORT_USB && transport_is_trusted(&t),
		      "usb held across the wrap at offset %u", i * 100U);
	}
}

int main(void)
{
	printf("transport host tests\n");

	test_starts_on_nothing_untrusted();
	test_wifi_is_the_default_when_configured();
	test_nothing_configured_is_honest();
	test_a_talking_host_claims_the_device();
	test_usb_claims_with_no_usbcfg_pin();
	test_quiet_host_loses_trust_but_keeps_the_device();
	test_only_a_reset_clears_the_latch();
	test_host_returning_regains_trust();
	test_goodbye_drops_trust_without_releasing();
	test_talking_again_cancels_a_goodbye();
	test_wifi_losing_trust_does_not_hand_over();
	test_never_trusted_without_evidence();
	test_clock_wrap();

	printf("%d checks, %d failures\n", checks, failures);
	return (failures == 0) ? 0 : 1;
}
