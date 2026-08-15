/*
 * Host unit tests for transport trust.
 *
 * The *selection* is a setting now (ADR-0022), so the load-bearing assertions are about what can and
 * cannot change it: nothing a host does on the wire may take the lamps from a wireless device, and a
 * configured transport that is not working must go fail-visible rather than quietly answering with the
 * other one — the two do not report the same machine's work.
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

static void test_configured_transport_is_known_immediately(void)
{
	/* It is configuration, not a discovery, so there is no "not yet decided" state to wait out. */
	transport_init(&t, WIGWAG_TRANSPORT_USB, false);
	CHECK(transport_active(&t) == TRANSPORT_USB, "usb from the first instruction");
	CHECK(!transport_is_trusted(&t), "but not trusted until something proves it");

	transport_init(&t, WIGWAG_TRANSPORT_WIFI, true);
	CHECK(transport_active(&t) == TRANSPORT_WIFI, "wifi from the first instruction");
	CHECK(!transport_is_trusted(&t), "and equally untrusted to begin with");
}

static void test_wired_trust_follows_the_heartbeat(void)
{
	transport_init(&t, WIGWAG_TRANSPORT_USB, false);
	transport_tick(&t, 100);
	CHECK(!transport_is_trusted(&t), "nothing has spoken yet");
	CHECK(t.reason == TRANSPORT_REASON_STARTING, "and that is distinguishable from going quiet");

	transport_note_host(&t, 1000);
	transport_tick(&t, 1000);
	CHECK(transport_is_trusted(&t), "trusted once a host speaks");
	CHECK(t.reason == TRANSPORT_REASON_OK, "reason ok");

	transport_tick(&t, 1000 + TRANSPORT_HOST_TTL_MS);
	CHECK(transport_is_trusted(&t), "still trusted at the TTL boundary");

	transport_tick(&t, 1000 + TRANSPORT_HOST_TTL_MS + 1U);
	CHECK(!transport_is_trusted(&t), "untrusted one millisecond past it");
	CHECK(t.reason == TRANSPORT_REASON_HOST_QUIET, "and says the host went quiet");
	CHECK(transport_active(&t) == TRANSPORT_USB, "still the configured transport");

	/* Indefinitely: there is nowhere else to go, and pretending otherwise would be the old bug. */
	transport_tick(&t, 3600000U);
	CHECK(transport_active(&t) == TRANSPORT_USB && !transport_is_trusted(&t),
	      "still usb, still untrusted, an hour later");

	transport_note_host(&t, 3600100U);
	transport_tick(&t, 3600100U);
	CHECK(transport_is_trusted(&t), "and one word restores it");
}

static void test_a_host_on_the_wire_cannot_take_a_wireless_device(void)
{
	/*
	 * The assertion this whole redesign exists for. A daemon talking on the console — because
	 * somebody plugged the device into a computer for power, or to configure it — must not be able
	 * to repurpose a wireless device.
	 */
	transport_init(&t, WIGWAG_TRANSPORT_WIFI, true);
	transport_note_wifi(&t, true, 0);
	transport_tick(&t, 1000);
	CHECK(transport_active(&t) == TRANSPORT_WIFI && transport_is_trusted(&t), "wireless and happy");

	transport_note_host(&t, 2000);
	transport_note_host(&t, 4000);
	transport_note_host(&t, 6000);
	transport_tick(&t, 6000);
	CHECK(transport_active(&t) == TRANSPORT_WIFI, "a chatty host does not take the lamps");
	CHECK(transport_is_trusted(&t), "and does not disturb the wireless verdict either");

	/* Not even while Wi-Fi is broken: fail-visible beats the wrong machine's work. */
	transport_note_wifi(&t, false, 7000);
	transport_note_host(&t, 7000);
	transport_tick(&t, 7000);
	CHECK(transport_active(&t) == TRANSPORT_WIFI, "still wireless with wifi down");
	CHECK(!transport_is_trusted(&t), "and honestly untrusted rather than borrowing the wire");
	CHECK(t.reason == TRANSPORT_REASON_WIFI_DOWN, "reason names the wifi");
}

static void test_a_quiet_wire_cannot_be_rescued_by_wifi(void)
{
	/* The mirror image, and equally important. */
	transport_init(&t, WIGWAG_TRANSPORT_USB, true);
	transport_note_wifi(&t, true, 0);
	transport_note_host(&t, 1000);
	transport_tick(&t, 1000);
	CHECK(transport_is_trusted(&t), "wired and trusted");

	transport_tick(&t, 60000);
	CHECK(transport_active(&t) == TRANSPORT_USB, "a healthy wifi link is not substituted in");
	CHECK(!transport_is_trusted(&t), "fail-visible instead");
}

static void test_wireless_with_no_ssid_is_honest(void)
{
	transport_init(&t, WIGWAG_TRANSPORT_WIFI, false);
	transport_tick(&t, 1000);
	CHECK(transport_active(&t) == TRANSPORT_NONE, "nothing to be on");
	CHECK(!transport_is_trusted(&t), "untrusted");
	CHECK(t.reason == TRANSPORT_REASON_NOTHING, "and says why");

	/* Even a talking host does not rescue it — the transport is still configured as wireless. */
	transport_note_host(&t, 2000);
	transport_tick(&t, 2000);
	CHECK(transport_active(&t) == TRANSPORT_NONE && !transport_is_trusted(&t),
	      "a host on the wire is not a network");
}

static void test_goodbye_and_its_cancellation(void)
{
	transport_init(&t, WIGWAG_TRANSPORT_USB, false);
	transport_note_host(&t, 1000);
	transport_tick(&t, 1000);
	CHECK(transport_is_trusted(&t), "trusted");

	/* Faster than waiting out the TTL, which is the only thing `host off` buys. */
	transport_note_host_bye(&t, 2000);
	transport_tick(&t, 2000);
	CHECK(!transport_is_trusted(&t), "untrusted at once");
	CHECK(t.reason == TRANSPORT_REASON_HOST_BYE, "distinguished from mere silence");

	transport_note_host(&t, 2500);
	transport_tick(&t, 2500);
	CHECK(transport_is_trusted(&t), "and talking again cancels it");
}

static void test_wants_module_follows_the_setting(void)
{
	/* A wired device never starts the RNWF02 at all (D118). */
	transport_init(&t, WIGWAG_TRANSPORT_USB, true);
	CHECK(!transport_wants_module(&t), "wired: no module, even with an ssid configured");

	transport_init(&t, WIGWAG_TRANSPORT_WIFI, false);
	CHECK(transport_wants_module(&t), "wireless: bring it up and let it report the trouble");
}

static void test_never_trusted_without_evidence(void)
{
	uint32_t now;

	/* Swept both ways, with nothing ever talking. */
	transport_init(&t, WIGWAG_TRANSPORT_USB, true);
	for (now = 0; now < 60000; now += 500) {
		transport_tick(&t, now);
		if (transport_is_trusted(&t)) {
			failures++;
			printf("  FAIL wired trusted with no host at %u\n", now);
			break;
		}
	}
	checks++;

	transport_init(&t, WIGWAG_TRANSPORT_WIFI, true);
	for (now = 0; now < 60000; now += 500) {
		transport_tick(&t, now);
		if (transport_is_trusted(&t)) {
			failures++;
			printf("  FAIL wireless trusted with no link at %u\n", now);
			break;
		}
	}
	checks++;
}

static void test_clock_wrap(void)
{
	uint32_t base = 0xFFFFF000U;
	uint32_t i;

	transport_init(&t, WIGWAG_TRANSPORT_USB, false);
	for (i = 0; i < 200U; i++) {
		uint32_t now = base + (i * 100U);	/* wraps partway through */

		transport_note_host(&t, now);
		transport_tick(&t, now);
		CHECK(transport_is_trusted(&t), "trust held across the wrap at offset %u", i * 100U);
	}
}

int main(void)
{
	printf("transport host tests\n");

	test_configured_transport_is_known_immediately();
	test_wired_trust_follows_the_heartbeat();
	test_a_host_on_the_wire_cannot_take_a_wireless_device();
	test_a_quiet_wire_cannot_be_rescued_by_wifi();
	test_wireless_with_no_ssid_is_honest();
	test_goodbye_and_its_cancellation();
	test_wants_module_follows_the_setting();
	test_never_trusted_without_evidence();
	test_clock_wrap();

	printf("%d checks, %d failures\n", checks, failures);
	return (failures == 0) ? 0 : 1;
}
