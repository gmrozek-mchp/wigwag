/*
 * Host unit tests for transport selection.
 *
 * The handover rules are exactly the kind of thing that looks obvious and is not: what happens when a
 * host goes quiet while Wi-Fi is healthy, whether an unplugged cable recovers faster than a timeout,
 * and whether the device ever ends up *trusting* two sources or none. Getting those wrong on a desk
 * would look like a flickering lamp and be miserable to reason about, so they are asserted here.
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
	CHECK(t.usb_cfg == TRANSPORT_USB_UNKNOWN, "no pin by default");
	transport_note_host(&t, 500);
	transport_tick(&t, 500);
	CHECK(transport_active(&t) == TRANSPORT_USB && transport_is_trusted(&t),
	      "usb claims on bytes alone");
}

static void test_bytes_beat_a_contradicting_pin(void)
{
	/*
	 * If the pin says no host but commands are arriving, believe the commands. A wrong pin should
	 * not be able to veto evidence that something is demonstrably talking to us.
	 */
	transport_init(&t, true);
	transport_note_usb_cfg(&t, TRANSPORT_USB_ABSENT, 0);
	transport_note_host(&t, 100);
	transport_tick(&t, 100);
	CHECK(transport_active(&t) == TRANSPORT_USB && transport_is_trusted(&t),
	      "received bytes outrank the pin");
}

static void test_quiet_host_goes_untrusted_before_it_hands_over(void)
{
	transport_init(&t, true);
	transport_note_wifi(&t, true, 0);
	transport_note_host(&t, 1000);
	transport_tick(&t, 1000);
	CHECK(transport_is_trusted(&t), "trusted while talking");

	/* Still fresh at exactly the TTL. */
	transport_tick(&t, 1000 + TRANSPORT_HOST_TTL_MS);
	CHECK(transport_is_trusted(&t), "still trusted at the TTL boundary");

	/* One millisecond past: untrusted at once, but USB still holds the device. */
	transport_tick(&t, 1000 + TRANSPORT_HOST_TTL_MS + 1U);
	CHECK(!transport_is_trusted(&t), "untrusted the moment the host is stale");
	CHECK(transport_active(&t) == TRANSPORT_USB, "usb still holds during the release window");
	CHECK(t.reason == TRANSPORT_REASON_HOST_QUIET, "reason says the host went quiet");

	/*
	 * This is the important one. Wi-Fi is *healthy* the whole time, and the device still refuses
	 * to show its state during the window — trading a known unknown for a possibly-staler source
	 * is not an improvement.
	 */
	transport_tick(&t, 1000 + TRANSPORT_HOST_TTL_MS + TRANSPORT_RELEASE_MS);
	CHECK(!transport_is_trusted(&t), "still fail-visible with wifi up and waiting");

	/* Past the window, hand over to Wi-Fi and trust it. */
	transport_tick(&t, 1000 + TRANSPORT_HOST_TTL_MS + TRANSPORT_RELEASE_MS + 2U);
	CHECK(transport_active(&t) == TRANSPORT_WIFI, "handed over to wifi");
	CHECK(transport_is_trusted(&t), "and trusts it");
}

static void test_host_returning_reclaims_immediately(void)
{
	transport_init(&t, true);
	transport_note_host(&t, 1000);
	transport_tick(&t, 1000);
	transport_tick(&t, 20000);
	CHECK(!transport_is_trusted(&t), "gone quiet");

	transport_note_host(&t, 20500);
	transport_tick(&t, 20500);
	CHECK(transport_active(&t) == TRANSPORT_USB && transport_is_trusted(&t),
	      "one word from the host restores it");
}

static void test_goodbye_releases_at_once(void)
{
	transport_init(&t, true);
	transport_note_wifi(&t, true, 0);
	transport_note_host(&t, 1000);
	transport_tick(&t, 1000);
	CHECK(transport_is_trusted(&t), "trusted");

	/* An orderly goodbye is not a fault, so there is nothing to wait out. */
	transport_note_host_bye(&t, 2000);
	transport_tick(&t, 2000);
	CHECK(transport_active(&t) == TRANSPORT_WIFI, "released to wifi without waiting");
	CHECK(transport_is_trusted(&t), "and wifi is trusted");

	/* With no Wi-Fi to fall back to, a goodbye leaves the device honestly on nothing. */
	transport_init(&t, false);
	transport_note_host(&t, 1000);
	transport_tick(&t, 1000);
	transport_note_host_bye(&t, 2000);
	transport_tick(&t, 2000);
	CHECK(transport_active(&t) == TRANSPORT_NONE && !transport_is_trusted(&t),
	      "nothing left to believe");
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
}

static void test_unplugging_skips_the_release_window(void)
{
	transport_init(&t, true);
	transport_note_wifi(&t, true, 0);
	transport_note_host(&t, 1000);
	transport_tick(&t, 1000);

	/* Cable out. Unambiguous, so there is no reason to wait out a timer. */
	transport_note_usb_cfg(&t, TRANSPORT_USB_ABSENT, 1100);
	transport_tick(&t, 1100);
	CHECK(transport_is_trusted(&t), "a fresh host still wins while it is fresh");

	transport_tick(&t, 1000 + TRANSPORT_HOST_TTL_MS + 1U);
	CHECK(transport_active(&t) == TRANSPORT_WIFI, "unplugged releases without the window");
	CHECK(t.reason == TRANSPORT_REASON_OK || t.reason == TRANSPORT_REASON_WIFI_DOWN,
	      "reason belongs to the new transport");
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
	test_bytes_beat_a_contradicting_pin();
	test_quiet_host_goes_untrusted_before_it_hands_over();
	test_host_returning_reclaims_immediately();
	test_goodbye_releases_at_once();
	test_talking_again_cancels_a_goodbye();
	test_unplugging_skips_the_release_window();
	test_wifi_losing_trust_does_not_hand_over();
	test_never_trusted_without_evidence();
	test_clock_wrap();

	printf("%d checks, %d failures\n", checks, failures);
	return (failures == 0) ? 0 : 1;
}
