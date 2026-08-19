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

/*
 * Mirrors TMO_BOOT_MS in rnwf_at.c, which is 10 s because a real RNWF02PC on 3.1.0 takes ~4.0 s to
 * emit +BOOT (measured over five runs: 3849-4059 ms). One named constant, so raising the timeout
 * again means changing two lines rather than hunting bare 5001s through this file.
 */
#define BOOT_TIMEOUT_MS 10000U

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

static char events[12][96];
static int n_events;

static char last_ip[48];
static char scan_lines[8][96];
static int n_scan;
static int scan_done;

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

static void on_ip(void *user, const char *ip)
{
	(void)user;
	snprintf(last_ip, sizeof(last_ip), "%s", ip);
}

static void on_event(void *user, const char *line)
{
	(void)user;

	if (n_events < (int)(sizeof(events) / sizeof(events[0]))) {
		snprintf(events[n_events], sizeof(events[0]), "%s", line);
	}
	n_events++;
}

static bool saw_event(const char *needle)
{
	int i;

	for (i = 0; i < n_events && i < (int)(sizeof(events) / sizeof(events[0])); i++) {
		if (strstr(events[i], needle) != NULL) {
			return true;
		}
	}

	return false;
}

static void on_scan(void *user, const char *info)
{
	(void)user;

	if (info == NULL) {
		scan_done++;
		return;
	}

	if (n_scan < (int)(sizeof(scan_lines) / sizeof(scan_lines[0]))) {
		snprintf(scan_lines[n_scan], sizeof(scan_lines[0]), "%s", info);
	}
	n_scan++;
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
	.host_online_topic = "wigwag/host_online",
};

static struct fake fake;
static struct rnwf_at at;

static void setup(void)
{
	struct rnwf_at_io io = { .write = fake_write, .user = &fake };
	struct rnwf_at_callbacks cb = { .on_message = on_message, .on_link = on_link,
					.on_ip = on_ip, .on_scan = on_scan,
					.on_event = on_event };

	memset(&fake, 0, sizeof(fake));
	n_messages = 0;
	link_up_count = 0;
	link_down_count = 0;
	last_ip[0] = '\0';
	n_scan = 0;
	scan_done = 0;
	n_events = 0;

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

/*
 * Answer whatever the module would answer for the command just sent.
 *
 * One place, called by every loop that drives the script, because three loops each encoding this is
 * what made adding a step break the tests twice: once when host_online was added, and again when the
 * subscribes started waiting for their SUBACK.
 */
static void answer_module(const char *cmd)
{
	feed("OK\r\n");

	if (strstr(cmd, "AT+WSTA=1") != NULL) {
		feed("\r+WSTAAIP:1,\"192.168.1.42\"\r\n");
	} else if (strstr(cmd, "AT+MQTTCONN") != NULL) {
		feed("\r+MQTTCONNACK:0,0\r\n");
	} else if (strstr(cmd, RNWF_AT_MQTT_SUB) != NULL) {
		/* SUBACK granting QoS 1: a subscribe is not finished when its OK arrives. */
		feed("\r+MQTTSUB:1\r\n");
	}
}

/*
 * Drive the script to READY, answering each command the way the module would.
 *
 * Every step that waits for an AEC must be answered here, or this loop spins: it feeds OK forever
 * while the client sits waiting for something that never arrives. The iteration cap turns that
 * mistake into a fast, named failure instead of a hung test run -- learned by hanging one.
 */
static void bring_up(void)
{
	int guard = 0;

	rnwf_at_start(&at, 0);
	feed("\r+BOOT:RNWF02 v3.1.0\r\n");

	while (at.state == RNWF_AT_ST_SCRIPT) {
		const char *cmd = last_sent();

		if (++guard > 64) {
			CHECK(false, "bring_up stalled at \"%s\" (cmd '%s'): the step waits for an AEC "
				     "this helper does not send", rnwf_at_step_str(&at), cmd);
			return;
		}

		answer_module(cmd);
	}
}

/* ----------------------------------------------------------------- the tests */

static void test_subscribes_to_host_liveness(void)
{
	bool saw = false;
	size_t i;

	setup();
	bring_up();

	for (i = 0; i < fake.n_sent; i++) {
		if (strcmp(fake.sent[i], "AT+MQTTSUB=\"wigwag/host_online\",1") == 0) {
			saw = true;
		}
	}

	CHECK(saw, "did not subscribe to the host liveness topic");
}

static void test_keepalive_polls_while_ready(void)
{
	size_t before;

	setup();
	bring_up();
	before = fake.n_sent;

	/* Nothing before the interval elapses. */
	rnwf_at_tick(&at, 4000);
	CHECK(fake.n_sent == before, "polled too early: '%s'", last_sent());

	rnwf_at_tick(&at, 5100);
	CHECK(fake.n_sent == before + 1U, "did not poll after the interval");
	CHECK(strcmp(last_sent(), RNWF_AT_PING) == 0, "poll sent '%s'", last_sent());
	CHECK(at.state == RNWF_AT_ST_READY, "poll changed state to %d", at.state);

	/* An OK keeps the link and must not walk off the end of the script. */
	feed("OK\r\n");
	CHECK(at.state == RNWF_AT_ST_READY, "state %d after poll OK", at.state);
	CHECK(link_up_count == 1, "link re-announced after a poll OK (%d times)", link_up_count);
}

static void test_silent_module_is_detected(void)
{
	/*
	 * The failure observed on hardware (D75): the module stops answering and the client must
	 * stop claiming a link, rather than sitting in READY forever.
	 */
	setup();
	bring_up();

	rnwf_at_tick(&at, 5100);
	CHECK(strcmp(last_sent(), RNWF_AT_PING) == 0, "expected a poll");

	rnwf_at_tick(&at, 6000);
	CHECK(rnwf_at_is_linked(&at), "gave up before the poll timeout");

	rnwf_at_tick(&at, 7200);
	CHECK(!rnwf_at_is_linked(&at), "still linked after an unanswered poll");
	CHECK(link_down_count == 1, "on_link(false) fired %d times", link_down_count);
	CHECK(at.state == RNWF_AT_ST_BACKOFF, "state %d", at.state);
}

static void test_broker_loss_reported_by_module(void)
{
	/* +MQTTCONN:0 - the module is fine, the broker is not. The poll would never see this. */
	setup();
	bring_up();

	feed("\r+MQTTCONN:0\r\n");

	CHECK(!rnwf_at_is_linked(&at), "still linked after +MQTTCONN:0");
	CHECK(link_down_count == 1, "on_link(false) fired %d times", link_down_count);
}

static void test_connected_state_aec_is_harmless(void)
{
	setup();
	bring_up();

	feed("\r+MQTTCONN:1\r\n");

	CHECK(rnwf_at_is_linked(&at), "+MQTTCONN:1 dropped the link");
	CHECK(link_down_count == 0, "+MQTTCONN:1 reported a link loss");
}

static void test_reset_then_boot_starts_script(void)
{
	setup();
	rnwf_at_start(&at, 0);

	CHECK(strcmp(fake.sent[0], RNWF_AT_RESET) == 0, "first command was '%s'", fake.sent[0]);
	CHECK(at.state == RNWF_AT_ST_RESETTING, "state %d", at.state);

	feed("\r+BOOT:RNWF02\r\n");
	CHECK(at.state == RNWF_AT_ST_SCRIPT, "state %d after +BOOT", at.state);

	/*
	 * Order matters and is asserted, not assumed. Echo off first: until it lands the module replays
	 * every command back at us and each replay is counted as a dropped line. Verbosity second, before
	 * anything whose failure we would otherwise have to read as vendor prose.
	 */
	CHECK(strcmp(fake.sent[1], RNWF_AT_ECHO_OFF) == 0, "second command was '%s'", fake.sent[1]);

	feed("OK\r\n");
	CHECK(strcmp(fake.sent[2], RNWF_AT_SET_VERBOSITY) == 0, "third command was '%s'",
	      fake.sent[2]);
}

/*
 * A module that never answers must be reported against the reset, not against the first command.
 *
 * Found during RNWF02 bring-up: `test wifi` on a board whose module UART could not transmit said
 * FAIL at "module responding" — naming a command that had never been sent. step stays 0 through the
 * reset phase, and the old step_str() only special-cased state == RESETTING, so the moment the
 * timeout moved the client to BACKOFF the name became step 0's. The two failures a bring-up most
 * needs to tell apart — "nothing is listening" and "the module rejected ATV3" — read identically.
 */
static void test_pre_boot_failure_is_not_blamed_on_step_zero(void)
{
	setup();
	rnwf_at_start(&at, 0);
	CHECK(strcmp(rnwf_at_step_str(&at), "resetting the module") == 0, "while resetting: '%s'",
	      rnwf_at_step_str(&at));

	/* No +BOOT: the boot wait times out and the client backs off, still at step 0. */
	rnwf_at_tick(&at, BOOT_TIMEOUT_MS + 1U);
	CHECK(at.state == RNWF_AT_ST_BACKOFF, "state %d", at.state);
	CHECK(at.step == 0U, "step %u", at.step);
	CHECK(strcmp(rnwf_at_step_str(&at), "resetting the module") == 0,
	      "pre-boot timeout blamed on '%s'", rnwf_at_step_str(&at));

	/* Once +BOOT *has* arrived, a step-0 failure really is step 0, and must say so. */
	setup();
	rnwf_at_start(&at, 0);
	feed("\r+BOOT:RNWF02\r\n");
	CHECK(at.boot_seen, "boot_seen not set by +BOOT");
	CHECK(strcmp(rnwf_at_step_str(&at), "module responding") == 0, "after +BOOT: '%s'",
	      rnwf_at_step_str(&at));

	rnwf_at_tick(&at, 60000);
	CHECK(at.state == RNWF_AT_ST_BACKOFF, "state %d after step timeout", at.state);
	CHECK(strcmp(rnwf_at_step_str(&at), "module responding") == 0,
	      "post-boot timeout blamed on '%s'", rnwf_at_step_str(&at));

	/* And a retry re-arms it: the next attempt is a reset phase again, not step 0. */
	rnwf_at_tick(&at, 120000);
	CHECK(at.state == RNWF_AT_ST_RESETTING, "state %d after retry", at.state);
	CHECK(!at.boot_seen, "boot_seen survived a restart");
	CHECK(strcmp(rnwf_at_step_str(&at), "resetting the module") == 0, "on retry: '%s'",
	      rnwf_at_step_str(&at));
}

/*
 * The network boundary must track the script, not a remembered index.
 *
 * The bug this pins: main.c used to say `step <= 4` to mean "before the network is up". Inserting
 * ATE0 at the front of the script shifted association from 4 to 5, and a failed *association* then
 * advised the user to go and check their broker. Asserting the predicate against the step names means
 * the next inserted step fails a test instead of quietly producing wrong advice.
 */
static void test_network_boundary_follows_the_script(void)
{
	bool seen_association = false;

	setup();
	rnwf_at_start(&at, 0);
	feed("\r+BOOT:RNWF02\r\n");

	while (at.state == RNWF_AT_ST_SCRIPT) {
		const char *name = rnwf_at_step_str(&at);
		const char *cmd = last_sent();
		bool is_association = (strcmp(name, "associate and get an IP") == 0);

		if (is_association) {
			seen_association = true;
		}

		/*
		 * At or before association: still the network's problem. After it: the broker's.
		 * Association itself counts as "before", because failing there is a network failure.
		 */
		CHECK(rnwf_at_before_network(&at) == !(seen_association && !is_association),
		      "before_network wrong at step \"%s\"", name);

		answer_module(cmd);
	}

	CHECK(seen_association, "the script no longer has an association step");
	CHECK(at.state == RNWF_AT_ST_READY, "state %d", at.state);
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

/* The status code is the diagnosis; it must survive to the caller. */
/*
 * A subscribe is finished when its SUBACK arrives, not when its OK does.
 *
 * The bug this pins, found with mosquitto's log as the witness: the client advanced on OK and fired
 * the next AT+MQTTSUB while the previous SUBACK was still outstanding. The module refused the
 * overlapping command with ERROR:8.0 and it never reached the broker -- one SUBSCRIBE logged, then
 * nothing until the keep-alive. Which subscribe lost depended on broker latency, so the failure moved
 * between steps and looked non-deterministic.
 */
static void test_subscribe_waits_for_its_suback(void)
{
	const char *first;
	int guard = 0;

	setup();
	rnwf_at_start(&at, 0);
	feed("\r+BOOT:RNWF02\r\n");

	/* Walk to the first subscribe. */
	while (at.state == RNWF_AT_ST_SCRIPT &&
	       strstr(last_sent(), RNWF_AT_MQTT_SUB) == NULL && ++guard < 64) {
		const char *cmd = last_sent();

		feed("OK\r\n");

		if (strstr(cmd, "AT+WSTA=1") != NULL) {
			feed("\r+WSTAAIP:1,\"192.168.1.42\"\r\n");
		} else if (strstr(cmd, "AT+MQTTCONN") != NULL) {
			feed("\r+MQTTCONNACK:0,0\r\n");
		}
	}

	first = last_sent();
	CHECK(strstr(first, "wigwag/state") != NULL, "first subscribe was '%s'", first);

	/* OK alone must NOT move on: that is what collided on hardware. */
	feed("OK\r\n");
	CHECK(strcmp(last_sent(), first) == 0, "sent '%s' before its SUBACK", last_sent());

	/* The SUBACK releases it. */
	feed("\r+MQTTSUB:1\r\n");
	CHECK(strcmp(last_sent(), first) != 0, "did not proceed after the SUBACK");
	CHECK(strstr(last_sent(), "wigwag/host_online") != NULL, "next was '%s'", last_sent());

	/* A refused subscription (128) must fail the step rather than pass it. */
	feed("OK\r\n");
	feed("\r+MQTTSUB:128\r\n");
	CHECK(at.state == RNWF_AT_ST_BACKOFF, "state %d after +MQTTSUB:128", at.state);
	CHECK(strstr(at.last_error, "128") != NULL, "kept '%s'", at.last_error);
}

/*
 * A link-local address is not an address. The module announces its fe80:: SLAAC address the moment
 * the link comes up, long before DHCPv4; treating that as "the network accepted us" leaves the broker
 * steps racing DHCP. Observed on hardware as +WSTAAIP:1,"FE80::DF6A:DC30:93BF:60AE".
 */
static void test_link_local_does_not_satisfy_got_ip(void)
{
	int guard = 0;

	setup();
	rnwf_at_start(&at, 0);
	feed("\r+BOOT:RNWF02\r\n");

	while (at.state == RNWF_AT_ST_SCRIPT && strstr(last_sent(), "AT+WSTA=1") == NULL &&
	       ++guard < 64) {
		feed("OK\r\n");
	}

	feed("OK\r\n");
	CHECK(last_ip[0] == '\0', "reported an address too early: '%s'", last_ip);

	/* Link-local: must not advance, and must not be reported as the address. */
	feed("\r+WSTAAIP:1,\"FE80::DF6A:DC30:93BF:60AE\"\r\n");
	CHECK(strcmp(last_sent(), RNWF_AT_WSTA_ENABLE) == 0, "advanced on a link-local address");
	CHECK(last_ip[0] == '\0', "reported the link-local address as ours: '%s'", last_ip);

	/* The real lease does advance it. */
	feed("\r+WSTAAIP:1,\"10.10.10.159\"\r\n");
	CHECK(strcmp(last_ip, "10.10.10.159") == 0, "reported '%s'", last_ip);
	CHECK(strcmp(last_sent(), RNWF_AT_WSTA_ENABLE) != 0, "did not advance on the DHCP lease");
}

static void test_error_code_is_kept(void)
{
	setup();
	rnwf_at_start(&at, 0);
	feed("\r+BOOT:RNWF02\r\n");
	feed("ERROR:8.0\r\n");

	CHECK(at.state == RNWF_AT_ST_BACKOFF, "state %d", at.state);
	CHECK(strcmp(at.last_error, "ERROR:8.0") == 0, "kept '%s'", at.last_error);

	/* A retry must not report the previous attempt's code. */
	rnwf_at_start(&at, 60000);
	CHECK(at.last_error[0] == '\0', "stale code survived a restart: '%s'", at.last_error);

	/* Overlong codes are truncated, not overflowed. */
	setup();
	rnwf_at_start(&at, 0);
	feed("\r+BOOT:RNWF02\r\n");
	feed("ERROR:0.5,\"Incorrect Number of Parameters and then some more text\"\r\n");
	CHECK(strlen(at.last_error) == sizeof(at.last_error) - 1U, "len %zu", strlen(at.last_error));
	CHECK(strncmp(at.last_error, "ERROR:0.5", 9) == 0, "kept '%s'", at.last_error);
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

		/*
		 * The remaining subscribes still have to be acknowledged before the link is trusted.
		 * Drained in a loop rather than counted, so adding a script step does not break this
		 * test again — which is exactly what happened when host_online was added.
		 */
		while (at.state == RNWF_AT_ST_SCRIPT) {
			answer_module(last_sent());
		}
		CHECK(at.state == RNWF_AT_ST_READY, "state %d after the subscribes", at.state);
	}
}

static void test_timeout_backs_off_and_retries(void)
{
	setup();
	rnwf_at_start(&at, 0);

	/* No +BOOT ever arrives. */
	rnwf_at_tick(&at, BOOT_TIMEOUT_MS - 1U);
	CHECK(at.state == RNWF_AT_ST_RESETTING, "gave up too early");

	rnwf_at_tick(&at, BOOT_TIMEOUT_MS + 1U);
	CHECK(at.state == RNWF_AT_ST_BACKOFF, "state %d after boot timeout", at.state);
	CHECK(at.timeouts == 1U, "timeouts=%u", at.timeouts);

	/* Backoff must elapse before a retry, and the retry re-sends the reset. */
	{
		size_t before = fake.n_sent;

		rnwf_at_tick(&at, BOOT_TIMEOUT_MS + 500U);
		CHECK(fake.n_sent == before, "retried before the backoff elapsed");

		rnwf_at_tick(&at, BOOT_TIMEOUT_MS + 1100U);
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

/* Did the client send any command containing `needle`? */
static bool sent_any(const char *needle)
{
	size_t i;

	for (i = 0; i < fake.n_sent; i++) {
		if (strstr(fake.sent[i], needle) != NULL) {
			return true;
		}
	}

	return false;
}

/*
 * The bring-up ladder: each rung must stop where it was asked to and go no further.
 *
 * These exist because the rungs are only useful if their boundaries hold. A `test wifi` that quietly
 * carried on and connected to a broker would report a broker failure for a network question, which is
 * the confusion the split was introduced to end.
 */
static void test_reset_only_stops_at_boot(void)
{
	setup();
	rnwf_at_reset_only(&at, 0);

	CHECK(strcmp(fake.sent[0], RNWF_AT_RESET) == 0, "first command was '%s'", fake.sent[0]);
	CHECK(fake.n_sent == 1U, "sent %zu commands, expected only the reset", fake.n_sent);

	feed("\r+BOOT:RNWF02\r\n");

	CHECK(at.state == RNWF_AT_ST_STOPPED, "state %d after +BOOT", at.state);
	CHECK(at.boot_seen, "boot_seen not set");
	CHECK(fake.n_sent == 1U, "sent %zu commands; a liveness check must send nothing else",
	      fake.n_sent);
	CHECK(!rnwf_at_is_linked(&at), "claimed a link after a bare reset");
	CHECK(link_up_count == 0, "announced a link (%d times) after a bare reset", link_up_count);
}

static void test_network_only_stops_once_associated(void)
{
	setup();
	rnwf_at_start_network_only(&at, 0);
	feed("\r+BOOT:RNWF02\r\n");

	while (at.state == RNWF_AT_ST_SCRIPT) {
		const char *cmd = last_sent();

		feed("OK\r\n");

		if (strstr(cmd, "AT+WSTA=1") != NULL) {
			feed("\r+WSTAAIP:1,\"192.168.1.42\"\r\n");
		}
	}

	CHECK(at.state == RNWF_AT_ST_STOPPED, "state %d after association", at.state);
	CHECK(strcmp(last_ip, "192.168.1.42") == 0, "on_ip reported '%s'", last_ip);

	/* The whole point: no broker business, and no claim of a trustworthy link. */
	CHECK(!sent_any("AT+MQTT"), "sent MQTT commands during a network-only run");
	CHECK(!rnwf_at_is_linked(&at), "claimed a link while merely associated");
	CHECK(link_up_count == 0, "announced a link (%d times) while merely associated",
	      link_up_count);
}

static void test_network_only_retry_does_not_become_a_full_connect(void)
{
	int guard = 0;

	setup();
	rnwf_at_start_network_only(&at, 0);
	feed("\r+BOOT:RNWF02\r\n");

	/* The module rejects the association outright. */
	feed("OK\r\n");			/* ATE0 */
	feed("OK\r\n");			/* ATV3 */
	while (at.state == RNWF_AT_ST_SCRIPT && strstr(last_sent(), "AT+WSTA=1") == NULL &&
	       ++guard < 64) {
		feed("OK\r\n");
	}
	feed("\r+WSTAERR:20.3\r\n");
	CHECK(at.state == RNWF_AT_ST_BACKOFF, "state %d after +WSTAERR", at.state);

	/* Backoff elapses and it retries — as the same rung, not as a full connect. */
	rnwf_at_tick(&at, 2000);
	CHECK(at.state == RNWF_AT_ST_RESETTING, "state %d after the retry", at.state);

	feed("\r+BOOT:RNWF02\r\n");
	while (at.state == RNWF_AT_ST_SCRIPT) {
		const char *cmd = last_sent();

		feed("OK\r\n");

		if (strstr(cmd, "AT+WSTA=1") != NULL) {
			feed("\r+WSTAAIP:1,\"192.168.1.42\"\r\n");
		}
	}

	CHECK(at.state == RNWF_AT_ST_STOPPED, "state %d: a retry promoted itself", at.state);
	CHECK(!sent_any("AT+MQTT"), "a retried network-only run reached the broker");
}

static void test_scan_reports_each_network_then_completes(void)
{
	setup();
	rnwf_at_scan(&at, 0);

	CHECK(strcmp(fake.sent[0], RNWF_AT_RESET) == 0, "first command was '%s'", fake.sent[0]);

	feed("\r+BOOT:RNWF02\r\n");
	CHECK(strcmp(last_sent(), RNWF_AT_ECHO_OFF) == 0, "after boot sent '%s'", last_sent());

	feed("OK\r\n");
	CHECK(strcmp(last_sent(), RNWF_AT_SET_VERBOSITY) == 0, "then sent '%s'", last_sent());

	feed("OK\r\n");
	CHECK(strcmp(last_sent(), "AT+WSCN=1") == 0, "scan command was '%s'", last_sent());

	/* OK means scanning *started*; results and completion arrive as AECs. */
	feed("OK\r\n");
	CHECK(at.state == RNWF_AT_ST_SCRIPT, "state %d: OK must not end the scan", at.state);

	feed("\r+WSCNIND:-42,3,6,\"AA:BB:CC:DD:EE:FF\",\"riverside\"\r\n");
	feed("\r+WSCNIND:-77,0,11,\"11:22:33:44:55:66\",\"guest\"\r\n");
	CHECK(n_scan == 2, "reported %d scan results", n_scan);
	CHECK(strstr(scan_lines[0], "riverside") != NULL, "first result was '%s'", scan_lines[0]);
	CHECK(at.state == RNWF_AT_ST_SCRIPT, "results must not end the scan");

	feed("\r+WSCNDONE\r\n");
	CHECK(at.state == RNWF_AT_ST_STOPPED, "state %d after +WSCNDONE", at.state);
	CHECK(!sent_any("AT+WSTAC"), "a scan configured the station");
	CHECK(!rnwf_at_is_linked(&at), "a scan claimed a link");
}

/*
 * Every event the module sends must reach the caller, including the ones this client acts on and the
 * ones it does not model.
 *
 * The bug this pins: unmodelled AECs went into aecs_ignored and nowhere else, so a bring-up could not
 * tell an association that never happened from one that succeeded and then lost DHCP -- +WSTALU was
 * invisible -- and +WSTAERR's code, the module's own verdict, was discarded with it.
 */
static void test_every_event_is_reported(void)
{
	setup();
	rnwf_at_start(&at, 0);
	feed("\r+BOOT:RNWF02\r\n");

	CHECK(saw_event("+BOOT"), "+BOOT was not reported");

	/* Modelled but informative: the link came up. Ignored by the state machine, shown to the user. */
	feed("\r+WSTALU:1,\"AA:BB:CC:DD:EE:FF\",6\r\n");
	CHECK(saw_event("+WSTALU"), "+WSTALU was not reported");
	CHECK(at.state == RNWF_AT_ST_SCRIPT, "+WSTALU disturbed the script");

	/* Acted on *and* reported: the code is the whole point. */
	feed("\r+WSTAERR:20.3\r\n");
	CHECK(saw_event("+WSTAERR:20.3"), "+WSTAERR was not reported with its code");
	CHECK(at.state == RNWF_AT_ST_BACKOFF, "state %d after +WSTAERR", at.state);
}

int main(void)
{
	printf("rnwf_at host tests\n");

	test_reset_then_boot_starts_script();
	test_pre_boot_failure_is_not_blamed_on_step_zero();
	test_subscribes_to_host_liveness();
	test_keepalive_polls_while_ready();
	test_silent_module_is_detected();
	test_broker_loss_reported_by_module();
	test_connected_state_aec_is_harmless();
	test_network_boundary_follows_the_script();
	test_every_event_is_reported();
	test_reset_only_stops_at_boot();
	test_network_only_stops_once_associated();
	test_network_only_retry_does_not_become_a_full_connect();
	test_scan_reports_each_network_then_completes();
	test_leading_cr_aec_framing();
	test_ok_is_accepted_not_done();
	test_full_bring_up_and_command_text();
	test_lwt_precedes_connect();
	test_optional_steps_skipped();
	test_subrx_delivers_json_payload();
	test_subscribe_waits_for_its_suback();
	test_link_local_does_not_satisfy_got_ip();
	test_error_code_is_kept();
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
