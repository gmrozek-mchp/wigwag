/*
 * Host unit tests for settings value handling.
 *
 * The behaviour worth pinning down is refusal. A value that does not fit must be rejected outright,
 * because a truncated passphrase or hostname is stored, looks right, and produces a failure with
 * nothing to point at — the same reasoning as LINEEDIT_TOO_LONG, one layer up.
 *
 *   make -C firmware/tests settings
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../src/settings.h"

#include <stdio.h>
#include <string.h>

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

static struct wigwag_settings s;

static void blank(void)
{
	memset(&s, 0, sizeof(s));
}

static void test_strings_round_trip(void)
{
	blank();

	CHECK(settings_apply(&s, CMD_KEY_SSID, "MyNetwork", 0) &&
	      strcmp(s.ssid, "MyNetwork") == 0, "ssid stored");
	CHECK(settings_apply(&s, CMD_KEY_PASS, "correct horse battery", 0) &&
	      strcmp(s.pass, "correct horse battery") == 0, "passphrase with spaces stored");
	CHECK(settings_apply(&s, CMD_KEY_BROKER, "mqtt.example.lan", 0) &&
	      strcmp(s.broker, "mqtt.example.lan") == 0, "broker stored");
	CHECK(settings_apply(&s, CMD_KEY_CLIENT, "wigwag-desk", 0) &&
	      strcmp(s.client, "wigwag-desk") == 0, "client stored");

	CHECK(strcmp(settings_get_str(&s, CMD_KEY_SSID), "MyNetwork") == 0, "ssid read back");
	CHECK(strcmp(settings_get_str(&s, CMD_KEY_BROKER), "mqtt.example.lan") == 0, "broker read back");
	CHECK(settings_get_str(&s, CMD_KEY_PORT) == NULL, "numeric keys have no string form");
	CHECK(settings_get_str(&s, CMD_KEY_NONE) == NULL, "unknown key has no string form");
}

static void test_empty_values_are_legal(void)
{
	blank();

	/* An open network, and a broker that wants no username. */
	CHECK(settings_apply(&s, CMD_KEY_PASS, "", 0) && s.pass[0] == '\0', "empty passphrase");
	CHECK(settings_apply(&s, CMD_KEY_USER, "", 0) && s.user[0] == '\0', "empty username");
}

static void test_too_long_is_refused_not_truncated(void)
{
	char big[256];
	size_t i;

	blank();

	for (i = 0; i < sizeof(big) - 1U; i++) {
		big[i] = 'x';
	}
	big[sizeof(big) - 1U] = '\0';

	CHECK(!settings_apply(&s, CMD_KEY_SSID, big, 0), "over-long ssid refused");
	CHECK(s.ssid[0] == '\0', "and nothing was written");
	CHECK(!settings_apply(&s, CMD_KEY_PASS, big, 0), "over-long passphrase refused");
	CHECK(s.pass[0] == '\0', "and nothing was written");
	CHECK(!settings_apply(&s, CMD_KEY_BROKER, big, 0), "over-long broker refused");

	/* Exactly at the limit must be accepted: 32 for an SSID, 63 for a WPA-2 passphrase. */
	{
		char ssid[SET_SSID_SZ];
		char pass[SET_PASS_SZ];

		memset(ssid, 'a', sizeof(ssid) - 1U);
		ssid[sizeof(ssid) - 1U] = '\0';
		memset(pass, 'b', sizeof(pass) - 1U);
		pass[sizeof(pass) - 1U] = '\0';

		CHECK(settings_apply(&s, CMD_KEY_SSID, ssid, 0) && strlen(s.ssid) == 32U,
		      "a 32-character ssid fits (%zu)", strlen(s.ssid));
		CHECK(settings_apply(&s, CMD_KEY_PASS, pass, 0) && strlen(s.pass) == 63U,
		      "a 63-character passphrase fits (%zu)", strlen(s.pass));
	}

	/* One over the limit must not. */
	{
		char ssid[SET_SSID_SZ + 1];

		memset(ssid, 'a', sizeof(ssid) - 1U);
		ssid[sizeof(ssid) - 1U] = '\0';
		CHECK(!settings_apply(&s, CMD_KEY_SSID, ssid, 0), "33 characters refused");
	}
}

static void test_numeric_ranges(void)
{
	blank();

	CHECK(settings_apply(&s, CMD_KEY_PORT, NULL, 1883) && s.port == 1883, "port 1883");
	CHECK(settings_apply(&s, CMD_KEY_PORT, NULL, 65535) && s.port == 65535, "port 65535");
	/* Port 0 is not a port, and 65536 does not fit the field it would be truncated into. */
	CHECK(!settings_apply(&s, CMD_KEY_PORT, NULL, 0), "port 0 refused");
	CHECK(!settings_apply(&s, CMD_KEY_PORT, NULL, 65536), "port 65536 refused");
	CHECK(s.port == 65535, "a refused port leaves the old value alone");

	CHECK(settings_apply(&s, CMD_KEY_SEC, NULL, 0) && s.sec == 0, "sec 0 (open)");
	CHECK(settings_apply(&s, CMD_KEY_SEC, NULL, 6) && s.sec == 6, "sec 6");
	/* enum rnwf_sec_type stops at 6; a higher value would be sent to the module verbatim. */
	CHECK(!settings_apply(&s, CMD_KEY_SEC, NULL, 7), "sec 7 refused");
	CHECK(!settings_apply(&s, CMD_KEY_SEC, NULL, 255), "sec 255 refused");
}

static void test_null_and_unknown(void)
{
	blank();

	CHECK(!settings_apply(&s, CMD_KEY_SSID, NULL, 0), "NULL string value refused");
	CHECK(!settings_apply(&s, CMD_KEY_NONE, "x", 0), "unknown key refused");
}

int main(void)
{
	printf("settings host tests\n");

	test_strings_round_trip();
	test_empty_values_are_legal();
	test_too_long_is_refused_not_truncated();
	test_numeric_ranges();
	test_null_and_unknown();

	printf("%d checks, %d failures\n", checks, failures);
	return (failures == 0) ? 0 : 1;
}
