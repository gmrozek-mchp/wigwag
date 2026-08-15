/*
 * Host unit tests for console command parsing.
 *
 * The interesting cases are all refusals and one preservation: a typo'd key must not silently
 * configure nothing, a bad number must not become zero, and a passphrase containing spaces must
 * survive intact — that last one would present as "wrong password" and be miserable to diagnose.
 *
 *   make -C firmware/tests cmd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../src/cmd.h"

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

/* cmd_parse() edits in place, so every case needs its own writable copy. */
static bool run(const char *text, struct cmd *out)
{
	static char buf[CMD_LINE_MAX * 2];

	snprintf(buf, sizeof(buf), "%s", text);

	return cmd_parse(buf, out);
}

static void test_bare_verbs(void)
{
	struct cmd c;

	CHECK(run("help", &c) && c.kind == CMD_HELP, "help");
	CHECK(run("?", &c) && c.kind == CMD_HELP, "? is help");
	CHECK(run("show", &c) && c.kind == CMD_SHOW, "show");
	CHECK(run("save", &c) && c.kind == CMD_SAVE, "save");
	CHECK(run("clear", &c) && c.kind == CMD_CLEAR, "clear");
	CHECK(run("reboot", &c) && c.kind == CMD_REBOOT, "reboot");
}

static void test_blank_and_whitespace_are_not_errors(void)
{
	struct cmd c;

	/* A terminal sends a bare CRLF every time you lean on Enter; that must not print an error. */
	CHECK(run("", &c) && c.kind == CMD_NONE, "empty line");
	CHECK(run("\r\n", &c) && c.kind == CMD_NONE, "bare CRLF");
	CHECK(run("   ", &c) && c.kind == CMD_NONE, "spaces only");
	CHECK(run("\t \r\n", &c) && c.kind == CMD_NONE, "tabs and CRLF");
	CHECK(run("  show  \r\n", &c) && c.kind == CMD_SHOW, "leading and trailing space around a verb");
}

static void test_unknown_verb_is_reported(void)
{
	struct cmd c;

	CHECK(!run("frobnicate", &c) && c.kind == CMD_UNKNOWN, "unknown verb");
	CHECK(!run("sset ssid x", &c) && c.kind == CMD_UNKNOWN, "near-miss verb is not guessed at");
	/* Case matters. Accepting "SHOW" would invite accepting "Set" and then "SSID". */
	CHECK(!run("SHOW", &c) && c.kind == CMD_UNKNOWN, "verbs are case-sensitive");
}

static void test_set_keys(void)
{
	struct cmd c;

	CHECK(run("set ssid MyNetwork", &c) && c.kind == CMD_SET && c.key == CMD_KEY_SSID &&
	      strcmp(c.value, "MyNetwork") == 0, "set ssid");
	CHECK(run("set broker mqtt.example.com", &c) && c.key == CMD_KEY_BROKER &&
	      strcmp(c.value, "mqtt.example.com") == 0, "set broker");
	CHECK(run("set client wigwag-desk", &c) && c.key == CMD_KEY_CLIENT, "set client");

	CHECK(!run("set nonsense x", &c) && c.kind == CMD_SET && c.key == CMD_KEY_NONE,
	      "unknown key refused, and reported as a set");
	CHECK(!run("set", &c) && c.kind == CMD_SET, "set with no key at all");
}

static void test_passphrase_with_spaces_survives(void)
{
	struct cmd c;

	/*
	 * The case that would be worst to get wrong: tokenising the value would truncate this to
	 * "correct" and the device would report a wrong password forever.
	 */
	CHECK(run("set pass correct horse battery staple", &c) && c.key == CMD_KEY_PASS &&
	      strcmp(c.value, "correct horse battery staple") == 0, "passphrase keeps its spaces");

	/* Multiple separators before the value collapse; separators inside it do not. */
	CHECK(run("set pass    two  words", &c) && strcmp(c.value, "two  words") == 0,
	      "leading separators skipped, internal ones kept, got \"%s\"", c.value);

	/* A trailing CR must go, but a trailing space is part of the value as typed. */
	CHECK(run("set pass secret\r\n", &c) && strcmp(c.value, "secret") == 0, "CRLF stripped");
}

static void test_empty_value_is_legal(void)
{
	struct cmd c;

	/* An open network has no passphrase, and this is how you say so. */
	CHECK(run("set pass", &c) && c.key == CMD_KEY_PASS && c.value != NULL &&
	      c.value[0] == '\0', "empty passphrase accepted");
	CHECK(run("set user ", &c) && c.key == CMD_KEY_USER && c.value[0] == '\0',
	      "empty username accepted");
}

static void test_numeric_keys_are_validated(void)
{
	struct cmd c;

	CHECK(run("set port 1883", &c) && c.num_valid && c.num == 1883, "port 1883");
	CHECK(run("set sec 0", &c) && c.num_valid && c.num == 0, "sec 0");

	/* A bad number must be refused, never coerced to 0 — that would silently mean "open". */
	CHECK(!run("set port abc", &c), "non-numeric port refused");
	CHECK(!run("set port 18x83", &c), "trailing junk refused");
	CHECK(!run("set port ", &c), "empty number refused");
	CHECK(!run("set port -1", &c), "negative refused");
	CHECK(!run("set port 4294967296", &c), "overflow refused");
	CHECK(run("set port 4294967295", &c) && c.num == 4294967295U, "u32 max accepted");
}

static void test_state_verb(void)
{
	struct cmd c;

	CHECK(run("state BUSY", &c) && c.kind == CMD_STATE && c.state == WIGWAG_BUSY, "state BUSY");
	CHECK(run("state IDLE", &c) && c.state == WIGWAG_IDLE, "state IDLE");
	CHECK(run("state WAIT", &c) && c.state == WIGWAG_WAIT, "state WAIT");
	CHECK(run("state ERROR", &c) && c.state == WIGWAG_ERROR, "state ERROR");
	CHECK(!run("state SLEEPY", &c) && c.kind == CMD_STATE, "unknown state refused");
	CHECK(!run("state", &c), "state with no argument refused");
}

static void test_brightness_and_gain(void)
{
	struct cmd c;

	CHECK(run("brightness 128", &c) && c.kind == CMD_BRIGHTNESS && c.num == 128, "brightness 128");
	CHECK(run("brightness 0", &c) && c.num == 0, "brightness 0 is legal");
	CHECK(run("brightness 255", &c) && c.num == 255, "brightness 255");
	CHECK(!run("brightness 256", &c), "brightness 256 refused");
	CHECK(!run("brightness", &c), "brightness with no argument refused");

	CHECK(run("gain green 200", &c) && c.kind == CMD_GAIN && c.lamp == LAMP_GREEN &&
	      c.num == 200, "gain green 200");
	CHECK(run("gain yellow 10", &c) && c.lamp == LAMP_YELLOW && c.num == 10, "gain yellow");
	CHECK(run("gain red 255", &c) && c.lamp == LAMP_RED && c.num == 255, "gain red");
	CHECK(!run("gain blue 10", &c), "there is no blue lamp");
	CHECK(!run("gain green 999", &c), "out-of-range gain refused");
	CHECK(!run("gain green", &c), "gain with no level refused");
	CHECK(!run("gain", &c), "bare gain refused");
}

static void test_echo_verb(void)
{
	struct cmd c;

	/* The daemon sends this so the shared wire carries protocol only (ADR-0018). */
	CHECK(run("echo off", &c) && c.kind == CMD_ECHO && c.num == 0U, "echo off");
	CHECK(run("echo on", &c) && c.kind == CMD_ECHO && c.num == 1U, "echo on");
	CHECK(!run("echo maybe", &c) && c.kind == CMD_ECHO, "echo needs on or off");
	CHECK(!run("echo", &c), "bare echo refused");
	CHECK(!run("echo 1", &c), "numeric echo refused - on|off only, so it reads unambiguously");
}

static void test_host_verb(void)
{
	struct cmd c;

	/* The serial stand-in for wigwag/host_online, which has no retention and no Last Will. */
	CHECK(run("host on", &c) && c.kind == CMD_HOST && c.num == 1U, "host on");
	CHECK(run("host off", &c) && c.kind == CMD_HOST && c.num == 0U, "host off (the goodbye)");
	CHECK(!run("host", &c) && c.kind == CMD_HOST, "bare host refused");
	CHECK(!run("host 1", &c), "numeric refused - on|off only, matching echo");
	CHECK(!run("hostile", &c) && c.kind == CMD_UNKNOWN, "a longer verb is not host");
}

static void test_secrets_are_marked(void)
{
	/* `show` relies on this. Getting it wrong prints a Wi-Fi password to anyone with a cable. */
	CHECK(cmd_key_is_secret(CMD_KEY_PASS), "wifi passphrase is secret");
	CHECK(cmd_key_is_secret(CMD_KEY_MQTTPASS), "broker password is secret");
	CHECK(!cmd_key_is_secret(CMD_KEY_SSID), "ssid is not secret");
	CHECK(!cmd_key_is_secret(CMD_KEY_BROKER), "broker host is not secret");
	CHECK(!cmd_key_is_secret(CMD_KEY_USER), "username is not secret");
	CHECK(!cmd_key_is_secret(CMD_KEY_PORT), "port is not secret");
	/* Unknown keys default to secret: print less rather than more. */
	CHECK(cmd_key_is_secret(CMD_KEY_NONE), "unknown key treated as secret");
}

static void test_key_names_round_trip(void)
{
	static const enum cmd_key all[] = {
		CMD_KEY_SSID, CMD_KEY_PASS, CMD_KEY_SEC, CMD_KEY_BROKER,
		CMD_KEY_PORT, CMD_KEY_CLIENT, CMD_KEY_USER, CMD_KEY_MQTTPASS,
	};
	size_t i;

	for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		char buf[CMD_LINE_MAX];
		struct cmd c;

		snprintf(buf, sizeof(buf), "set %s value", cmd_key_str(all[i]));
		CHECK(cmd_parse(buf, &c) || all[i] == CMD_KEY_SEC || all[i] == CMD_KEY_PORT,
		      "cmd_key_str(%d) = \"%s\" must parse back", (int)all[i], cmd_key_str(all[i]));
		if (all[i] != CMD_KEY_SEC && all[i] != CMD_KEY_PORT) {
			CHECK(c.key == all[i], "%s round-tripped to key %d", cmd_key_str(all[i]),
			      (int)c.key);
		}
	}

	CHECK(strcmp(cmd_key_str(CMD_KEY_NONE), "?") == 0, "unknown key prints as ?");
}

static void test_long_lines_do_not_overrun(void)
{
	/* A 63-character passphrase is legal, and is what CMD_LINE_MAX is sized for. */
	char buf[CMD_LINE_MAX * 2];
	struct cmd c;
	char pass[64];

	memset(pass, 'x', sizeof(pass) - 1U);
	pass[sizeof(pass) - 1U] = '\0';

	snprintf(buf, sizeof(buf), "set pass %s", pass);
	CHECK(strlen(buf) < CMD_LINE_MAX, "a max-length passphrase fits in CMD_LINE_MAX (%zu)",
	      strlen(buf));
	CHECK(cmd_parse(buf, &c) && strlen(c.value) == 63U, "63-char passphrase parsed whole");
}

int main(void)
{
	printf("cmd host tests\n");

	test_bare_verbs();
	test_blank_and_whitespace_are_not_errors();
	test_unknown_verb_is_reported();
	test_set_keys();
	test_passphrase_with_spaces_survives();
	test_empty_value_is_legal();
	test_numeric_keys_are_validated();
	test_state_verb();
	test_brightness_and_gain();
	test_echo_verb();
	test_host_verb();
	test_secrets_are_marked();
	test_key_names_round_trip();
	test_long_lines_do_not_overrun();

	printf("%d checks, %d failures\n", checks, failures);
	return (failures == 0) ? 0 : 1;
}
