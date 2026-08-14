/*
 * Host unit tests for the RNWF02 AT client.
 *
 * Plain C, plain clang, no Zephyr and no hardware — which is the whole reason the core has no
 * Zephyr headers (ADR-0015, D66). native_sim would have given the same coverage on Linux; this
 * gives it on macOS too, and in a fraction of the time.
 *
 *   make -C firmware/tests
 *
 * The fake transport records every line written so tests can assert on the exact bytes that would
 * reach the module, and time is passed in, so timeouts and backoff are tested without sleeping.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../src/rnwf_at.h"
#include "../src/rnwf_at_cmds.h"

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

/* ------------------------------------------------------------ fake transport */

#define MAX_SENT 64

struct fake {
	char sent[MAX_SENT][RNWF_AT_TX_BUF_SZ];
	size_t n_sent;
	char partial[RNWF_AT_TX_BUF_SZ];
	size_t partial_len;
};

static int fake_write(void *user, const uint8_t *data, size_t len)
{
	struct fake *f = user;
	size_t i;

	for (i = 0; i < len; i++) {
		char c = (char)data[i];

		if (c == '\r') {
			continue;
		}
		if (c == '\n') {
			if (f->n_sent < MAX_SENT) {
				memcpy(f->sent[f->n_sent], f->partial, f->partial_len);
				f->sent[f->n_sent][f->partial_len] = '\0';
				f->n_sent++;
			}
			f->partial_len = 0;
			continue;
		}
		if (f->partial_len + 1U < sizeof(f->partial)) {
			f->partial[f->partial_len++] = c;
		}
	}

	return (int)len;
}

/* ------------------------------------------------------------- fake app side */

static char last_topic[64];
static char last_payload[256];
static int n_messages;
static int link_up_count;
static int link_down_count;

static void on_message(void *user, const char *topic, const char *payload)
{
	(void)user;
	snprintf(last_topic, sizeof(last_topic), "%s", topic);
	snprintf(last_payload, sizeof(last_payload), "%s", payload);
	n_messages++;
}

static void on_link(void *user, bool linked)
{
	(void)user;
	if (linked) {
		link_up_count++;
	} else {
		link_down_count++;
	}
}

static const struct rnwf_at_config cfg = {
	.ssid = "TestAP",
	.passphrase = "secretpass",
	.sec_type = RNWF_SEC_WPA2_PERSONAL,
	.broker_host = "broker.local",
	.broker_port = 1883,
	.client_id = "wigwag-1",
	.username = NULL,
	.password = NULL,
	.keep_alive_s = 60,
	.state_topic = "wigwag/state",
	.online_topic = "wigwag/online",
};

static struct fake fake;
static struct rnwf_at at;

static void setup(void)
{
	struct rnwf_at_io io = { .write = fake_write, .user = &fake };
	struct rnwf_at_callbacks cb = { .on_message = on_message, .on_link = on_link };

	memset(&fake, 0, sizeof(fake));
	n_messages = 0;
	link_up_count = 0;
	link_down_count = 0;

	rnwf_at_init(&at, &cfg, &io, &cb);
}

static void feed(const char *s)
{
	rnwf_at_feed(&at, (const uint8_t *)s, strlen(s));
}

static const char *last_sent(void)
{
	return (fake.n_sent > 0U) ? fake.sent[fake.n_sent - 1U] : "";
}

/* Drive the script to READY, answering each command the way the module would. */
static void bring_up(void)
{
	rnwf_at_start(&at, 0);
	feed("\r+BOOT:RNWF02 v3.1.0\r\n");

	while (at.state == RNWF_AT_ST_SCRIPT) {
		const char *cmd = last_sent();

		feed("OK\r\n");

		if (strstr(cmd, "AT+WSTA=1") != NULL) {
			feed("\r+WSTAAIP:1,\"192.168.1.42\"\r\n");
		} else if (strstr(cmd, "AT+MQTTCONN") != NULL) {
			feed("\r+MQTTCONNACK:0,0\r\n");
		}
	}
}

/* ----------------------------------------------------------------- the tests */

static void test_reset_then_boot_starts_script(void)
{
	setup();
	rnwf_at_start(&at, 0);

	CHECK(strcmp(fake.sent[0], RNWF_AT_RESET) == 0, "first command was '%s'", fake.sent[0]);
	CHECK(at.state == RNWF_AT_ST_RESETTING, "state %d", at.state);

	feed("\r+BOOT:RNWF02\r\n");
	CHECK(at.state == RNWF_AT_ST_SCRIPT, "state %d after +BOOT", at.state);

	/* Verbosity must be pinned before anything whose failure we would have to parse. */
	CHECK(strcmp(fake.sent[1], RNWF_AT_SET_VERBOSITY) == 0, "second command was '%s'",
	      fake.sent[1]);
}

static void test_leading_cr_aec_framing(void)
{
	/*
	 * The spec's AEC framing is <CR>+NAME:INFO<CR><LF>. The leading CR must close an empty
	 * line, not corrupt the AEC that follows.
	 */
	setup();
	rnwf_at_start(&at, 0);
	feed("\r+BOOT:x\r\n");
	CHECK(at.state == RNWF_AT_ST_SCRIPT, "leading-CR AEC was not recognised");

	/* Split across feed() calls, as a byte-at-a-time UART would deliver it. */
	setup();
	rnwf_at_start(&at, 0);
	feed("\r+BO");
	feed("OT:x\r");
	feed("\n");
	CHECK(at.state == RNWF_AT_ST_SCRIPT, "AEC split across feeds was not reassembled");
}

static void test_ok_is_accepted_not_done(void)
{
	/*
	 * The rule that most wanted encoding: for AT+WSTA=1, OK means accepted. The script must
	 * not advance until +WSTAAIP arrives.
	 */
	setup();
	rnwf_at_start(&at, 0);
	feed("\r+BOOT:x\r\n");

	while (strstr(last_sent(), "AT+WSTA=1") == NULL) {
		size_t before = fake.n_sent;

		feed("OK\r\n");
		CHECK(fake.n_sent > before || at.state != RNWF_AT_ST_SCRIPT,
		      "script stalled at '%s'", last_sent());
		if (fake.n_sent == before) {
			return;
		}
	}

	{
		size_t after_wsta = fake.n_sent;

		feed("OK\r\n");
		CHECK(fake.n_sent == after_wsta,
		      "advanced on OK alone; sent '%s'", last_sent());

		feed("\r+WSTAAIP:1,\"192.168.1.42\"\r\n");
		CHECK(fake.n_sent > after_wsta, "did not advance after +WSTAAIP");
		CHECK(strstr(last_sent(), RNWF_AT_MQTTC) != NULL,
		      "expected MQTT config next, got '%s'", last_sent());
	}
}

static void test_full_bring_up_and_command_text(void)
{
	setup();
	bring_up();

	CHECK(at.state == RNWF_AT_ST_READY, "state %d after bring-up", at.state);
	CHECK(rnwf_at_is_linked(&at), "not linked after bring-up");
	CHECK(link_up_count == 1, "on_link(true) fired %d times", link_up_count);

	/* Spot-check the exact wire text against the specification's syntax. */
	{
		bool saw_ssid = false, saw_sec = false, saw_cred = false;
		bool saw_host = false, saw_port = false, saw_lwt = false, saw_sub = false;
		size_t i;

		for (i = 0; i < fake.n_sent; i++) {
			const char *s = fake.sent[i];

			if (strcmp(s, "AT+WSTAC=1,\"TestAP\"") == 0) {
				saw_ssid = true;
			} else if (strcmp(s, "AT+WSTAC=2,3") == 0) {
				saw_sec = true;
			} else if (strcmp(s, "AT+WSTAC=3,\"secretpass\"") == 0) {
				saw_cred = true;
			} else if (strcmp(s, "AT+MQTTC=1,\"broker.local\"") == 0) {
				saw_host = true;
			} else if (strcmp(s, "AT+MQTTC=2,1883") == 0) {
				saw_port = true;
			} else if (strcmp(s, "AT+MQTTLWT=0,1,\"wigwag/online\",\"0\"") == 0) {
				saw_lwt = true;
			} else if (strcmp(s, "AT+MQTTSUB=\"wigwag/state\",1") == 0) {
				saw_sub = true;
			}
		}

		CHECK(saw_ssid, "SSID command not seen verbatim");
		CHECK(saw_sec, "security type command not seen verbatim");
		CHECK(saw_cred, "credentials command not seen verbatim");
		CHECK(saw_host, "broker host command not seen verbatim");
		CHECK(saw_port, "broker port command not seen verbatim");
		CHECK(saw_lwt, "last will command not seen verbatim");
		CHECK(saw_sub, "subscribe command not seen verbatim");
	}
}

static void test_lwt_precedes_connect(void)
{
	size_t i, lwt = 0, conn = 0;

	setup();
	bring_up();

	for (i = 0; i < fake.n_sent; i++) {
		if (strstr(fake.sent[i], RNWF_AT_MQTT_LWT) != NULL) {
			lwt = i;
		} else if (strstr(fake.sent[i], "AT+MQTTCONN") != NULL) {
			conn = i;
		}
	}

	CHECK(lwt != 0U && conn != 0U && lwt < conn,
	      "LWT must be registered before connecting (lwt=%zu conn=%zu)", lwt, conn);
}

static void test_optional_steps_skipped(void)
{
	/* username/password are NULL in cfg, so those commands must not appear at all. */
	size_t i;

	setup();
	bring_up();

	for (i = 0; i < fake.n_sent; i++) {
		CHECK(strstr(fake.sent[i], "AT+MQTTC=4,") == NULL,
		      "username set despite NULL config: '%s'", fake.sent[i]);
		CHECK(strstr(fake.sent[i], "AT+MQTTC=5,") == NULL,
		      "password set despite NULL config: '%s'", fake.sent[i]);
	}
}

static void test_subrx_delivers_json_payload(void)
{
	setup();
	bring_up();

	/*
	 * The real payload shape from CONTEXT.md: JSON, containing both commas and double quotes.
	 * Taking the payload as the tail after the fourth comma is what makes this survive.
	 */
	feed("\r+MQTTSUBRX:0,1,1,\"wigwag/state\",\"{\"state\":\"WAIT\",\"sessions\":2}\"\r\n");

	CHECK(n_messages == 1, "%d messages delivered", n_messages);
	CHECK(strcmp(last_topic, "wigwag/state") == 0, "topic was '%s'", last_topic);
	CHECK(strstr(last_payload, "\"state\":\"WAIT\"") != NULL,
	      "payload lost its structure: '%s'", last_payload);
	CHECK(strstr(last_payload, "\"sessions\":2") != NULL,
	      "payload truncated at a comma: '%s'", last_payload);
}

static void test_error_response_backs_off(void)
{
	setup();
	rnwf_at_start(&at, 0);
	feed("\r+BOOT:x\r\n");

	/* ATV3 formats failures as ERROR:<STATUS_CODE>, so the prefix must be enough. */
	feed("ERROR:12\r\n");

	CHECK(at.state == RNWF_AT_ST_BACKOFF, "state %d after ERROR", at.state);
	CHECK(at.errors == 1U, "errors=%u", at.errors);
}

static void test_connack_failure_backs_off(void)
{
	setup();
	rnwf_at_start(&at, 0);
	feed("\r+BOOT:x\r\n");

	while (at.state == RNWF_AT_ST_SCRIPT && strstr(last_sent(), "AT+MQTTCONN") == NULL) {
		feed("OK\r\n");
		if (strstr(last_sent(), "AT+WSTA=1") != NULL) {
			feed("\r+WSTAAIP:1,\"10.0.0.1\"\r\n");
		}
	}

	feed("OK\r\n");
	/* 130 = protocol error, per the spec's reason code table. */
	feed("\r+MQTTCONNACK:0,130\r\n");

	CHECK(at.state == RNWF_AT_ST_BACKOFF, "state %d after failed CONNACK", at.state);
}

static void test_connack_prefix_not_confused_with_connstate(void)
{
	/*
	 * +MQTTCONN and +MQTTCONNACK share a prefix. Matching must require ':' or end-of-line so
	 * the connection-state AEC never satisfies a wait for the acknowledgement.
	 */
	setup();
	rnwf_at_start(&at, 0);
	feed("\r+BOOT:x\r\n");

	while (at.state == RNWF_AT_ST_SCRIPT && strstr(last_sent(), "AT+MQTTCONN") == NULL) {
		feed("OK\r\n");
		if (strstr(last_sent(), "AT+WSTA=1") != NULL) {
			feed("\r+WSTAAIP:1,\"10.0.0.1\"\r\n");
		}
	}

	feed("OK\r\n");
	{
		size_t before = fake.n_sent;

		feed("\r+MQTTCONN:1\r\n");	/* connection state, not the ack */
		CHECK(fake.n_sent == before && at.state == RNWF_AT_ST_SCRIPT,
		      "+MQTTCONN was mistaken for +MQTTCONNACK");

		feed("\r+MQTTCONNACK:0,0\r\n");
		CHECK(strstr(last_sent(), RNWF_AT_MQTT_SUB) != NULL,
		      "real CONNACK did not advance to subscribe, sent '%s'", last_sent());

		/* Subscribe still has to be acknowledged before the link is trusted. */
		feed("OK\r\n");
		CHECK(at.state == RNWF_AT_ST_READY, "state %d after subscribe OK", at.state);
	}
}

static void test_timeout_backs_off_and_retries(void)
{
	setup();
	rnwf_at_start(&at, 0);

	/* No +BOOT ever arrives. */
	rnwf_at_tick(&at, 4999);
	CHECK(at.state == RNWF_AT_ST_RESETTING, "gave up too early");

	rnwf_at_tick(&at, 5001);
	CHECK(at.state == RNWF_AT_ST_BACKOFF, "state %d after boot timeout", at.state);
	CHECK(at.timeouts == 1U, "timeouts=%u", at.timeouts);

	/* Backoff must elapse before a retry, and the retry re-sends the reset. */
	{
		size_t before = fake.n_sent;

		rnwf_at_tick(&at, 5500);
		CHECK(fake.n_sent == before, "retried before the backoff elapsed");

		rnwf_at_tick(&at, 6100);
		CHECK(fake.n_sent > before, "never retried");
		CHECK(strcmp(last_sent(), RNWF_AT_RESET) == 0, "retry sent '%s'", last_sent());
	}
}

static void test_backoff_grows_and_caps(void)
{
	uint32_t first, second;

	setup();
	rnwf_at_start(&at, 0);
	feed("ERROR:1\r\n");
	first = at.backoff_ms;

	rnwf_at_tick(&at, at.retry_at_ms + 1U);
	feed("ERROR:1\r\n");
	second = at.backoff_ms;

	CHECK(second > first, "backoff did not grow (%u -> %u)", first, second);

	/* Hammer it and confirm the cap holds. */
	{
		int i;

		for (i = 0; i < 20; i++) {
			rnwf_at_tick(&at, at.retry_at_ms + 1U);
			feed("ERROR:1\r\n");
		}
		CHECK(at.backoff_ms <= 30000U, "backoff exceeded its cap: %u", at.backoff_ms);
	}
}

static void test_link_down_while_ready_reports_unlinked(void)
{
	/* ADR-0007: losing the link must be announced, not silently tolerated. */
	setup();
	bring_up();
	CHECK(link_down_count == 0, "link reported down during bring-up");

	feed("\r+WSTALD:1\r\n");

	CHECK(!rnwf_at_is_linked(&at), "still claims to be linked after +WSTALD");
	CHECK(link_down_count == 1, "on_link(false) fired %d times", link_down_count);
	CHECK(at.state == RNWF_AT_ST_BACKOFF, "state %d after link down", at.state);
}

static void test_publish_only_when_ready(void)
{
	setup();

	CHECK(rnwf_at_publish(&at, "wigwag/button", "{\"event\":\"press\"}", false) == -1,
	      "published while idle");

	bring_up();

	CHECK(rnwf_at_publish(&at, "wigwag/button", "{\"event\":\"press\"}", false) == 0,
	      "publish failed when ready");
	CHECK(strcmp(last_sent(),
		     "AT+MQTTPUB=0,0,0,\"wigwag/button\",\"{\"event\":\"press\"}\"") == 0,
	      "publish text was '%s'", last_sent());

	CHECK(rnwf_at_publish(&at, "wigwag/online", "1", true) == 0, "retained publish failed");
	CHECK(strcmp(last_sent(), "AT+MQTTPUB=0,0,1,\"wigwag/online\",\"1\"") == 0,
	      "retained publish text was '%s'", last_sent());
}

static void test_oversized_line_dropped_whole(void)
{
	size_t i;

	setup();
	bring_up();

	/* A line longer than the buffer must be dropped entirely, not wrapped and half-parsed. */
	feed("\r+MQTTSUBRX:0,0,0,\"wigwag/state\",\"");
	for (i = 0; i < RNWF_AT_RX_LINE_SZ * 2U; i++) {
		feed("x");
	}
	feed("\"\r\n");

	CHECK(n_messages == 0, "an oversized line was parsed anyway");
	CHECK(at.lines_dropped > 0U, "oversized line was not counted");

	/* And the assembler must still be usable afterwards. */
	feed("\r+MQTTSUBRX:0,0,0,\"wigwag/state\",\"IDLE\"\r\n");
	CHECK(n_messages == 1, "assembler did not recover after overflow");
}

static void test_junk_is_survivable(void)
{
	setup();
	bring_up();

	feed("\r\n\r\n");
	feed("garbage without a plus\r\n");
	feed("+UNKNOWNAEC:1,2,3\r\n");
	feed("+MQTTSUBRX:malformed\r\n");
	feed("\r\n");

	CHECK(at.state == RNWF_AT_ST_READY, "junk knocked the client out of READY");
	CHECK(n_messages == 0, "junk produced a message");
}

int main(void)
{
	printf("rnwf_at host tests\n");

	test_reset_then_boot_starts_script();
	test_leading_cr_aec_framing();
	test_ok_is_accepted_not_done();
	test_full_bring_up_and_command_text();
	test_lwt_precedes_connect();
	test_optional_steps_skipped();
	test_subrx_delivers_json_payload();
	test_error_response_backs_off();
	test_connack_failure_backs_off();
	test_connack_prefix_not_confused_with_connstate();
	test_timeout_backs_off_and_retries();
	test_backoff_grows_and_caps();
	test_link_down_while_ready_reports_unlinked();
	test_publish_only_when_ready();
	test_oversized_line_dropped_whole();
	test_junk_is_survivable();

	printf("%d checks, %d failures\n", checks, failures);
	return (failures == 0) ? 0 : 1;
}
