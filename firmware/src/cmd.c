/*
 * Console command parsing. See cmd.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cmd.h"

#include <string.h>

struct key_name {
	const char *name;
	enum cmd_key key;
	bool numeric;
	bool secret;
};

/*
 * The whole settable surface, in one table, so `show`, `set` and the help text cannot drift apart.
 * `numeric` keys are validated at parse time; a bad number is refused rather than stored as zero.
 */
/* Not ARRAY_SIZE(): this file stays free of Zephyr headers so the host tests can build it. */
#define N_KEYS (sizeof(keys) / sizeof((keys)[0]))

static const struct key_name keys[] = {
	{ "ssid",     CMD_KEY_SSID,     false, false },
	{ "pass",     CMD_KEY_PASS,     false, true  },
	{ "sec",      CMD_KEY_SEC,      true,  false },
	{ "broker",   CMD_KEY_BROKER,   false, false },
	{ "port",     CMD_KEY_PORT,     true,  false },
	{ "client",   CMD_KEY_CLIENT,   false, false },
	{ "user",     CMD_KEY_USER,     false, false },
	{ "mqttpass", CMD_KEY_MQTTPASS, false, true  },
	{ "transport", CMD_KEY_TRANSPORT, false, false },
};

static const struct key_name *key_lookup(const char *name)
{
	size_t i;

	for (i = 0; i < N_KEYS; i++) {
		if (strcmp(keys[i].name, name) == 0) {
			return &keys[i];
		}
	}

	return NULL;
}

bool cmd_is_host_activity(const struct cmd *c)
{
	if (c == NULL) {
		return false;
	}

	/* `host on` claims; `host off` is its opposite and must not. */
	if (c->kind == CMD_HOST) {
		return c->num != 0U;
	}

	return c->kind == CMD_STATE;
}

bool cmd_key_is_secret(enum cmd_key key)
{
	size_t i;

	for (i = 0; i < N_KEYS; i++) {
		if (keys[i].key == key) {
			return keys[i].secret;
		}
	}

	/* Unknown keys are treated as secret: the safe direction is to print less, not more. */
	return true;
}

const char *cmd_key_str(enum cmd_key key)
{
	size_t i;

	for (i = 0; i < N_KEYS; i++) {
		if (keys[i].key == key) {
			return keys[i].name;
		}
	}

	return "?";
}

static bool is_space(char c)
{
	return c == ' ' || c == '\t';
}

/*
 * Strict decimal. No sign, no prefix, no trailing junk, no overflow — a console that accepted "12x"
 * as 12 would let a typo configure a port number nobody intended.
 */
static bool parse_u32(const char *s, uint32_t *out)
{
	uint32_t v = 0;
	size_t digits = 0;

	if (s == NULL) {
		return false;
	}

	while (*s != '\0') {
		uint32_t d;

		if (*s < '0' || *s > '9') {
			return false;
		}

		d = (uint32_t)(*s - '0');

		if (v > (0xFFFFFFFFU - d) / 10U) {
			return false;	/* would overflow */
		}

		v = (v * 10U) + d;
		digits++;
		s++;
	}

	if (digits == 0U) {
		return false;
	}

	*out = v;

	return true;
}

/** Advance past a token, NUL-terminate it, and return the start of whatever follows. */
static char *split(char *p, char **token)
{
	while (is_space(*p)) {
		p++;
	}

	*token = p;

	while (*p != '\0' && !is_space(*p)) {
		p++;
	}

	if (*p != '\0') {
		*p = '\0';
		p++;
	}

	return p;
}

static bool parse_gain(char *rest, struct cmd *out)
{
	char *which;
	char *value;

	rest = split(rest, &which);
	(void)split(rest, &value);

	if (strcmp(which, "green") == 0) {
		out->lamp = LAMP_GREEN;
	} else if (strcmp(which, "yellow") == 0) {
		out->lamp = LAMP_YELLOW;
	} else if (strcmp(which, "red") == 0) {
		out->lamp = LAMP_RED;
	} else {
		return false;
	}

	/*
	 * Reuse the brightness parser: gain and brightness occupy the same 0-255 space and the same
	 * meaning (a perceptual scale, not a duty cycle — D89), so they should refuse the same inputs.
	 */
	{
		uint8_t level;

		if (!lamp_brightness_parse(value, &level)) {
			return false;
		}

		out->num = level;
		out->num_valid = true;
	}

	return true;
}

bool cmd_parse(char *line, struct cmd *out)
{
	char *verb;
	char *rest;
	size_t len;

	memset(out, 0, sizeof(*out));

	if (line == NULL) {
		out->kind = CMD_NONE;
		return true;
	}

	/* Strip trailing CR/LF and nothing else: a trailing space could belong to a passphrase. */
	len = strlen(line);
	while (len > 0U && (line[len - 1U] == '\r' || line[len - 1U] == '\n')) {
		line[--len] = '\0';
	}

	rest = split(line, &verb);

	if (verb[0] == '\0') {
		out->kind = CMD_NONE;
		return true;
	}

	if (strcmp(verb, "help") == 0 || strcmp(verb, "?") == 0) {
		out->kind = CMD_HELP;
		return true;
	}

	if (strcmp(verb, "show") == 0) {
		out->kind = CMD_SHOW;
		return true;
	}

	if (strcmp(verb, "save") == 0) {
		out->kind = CMD_SAVE;
		return true;
	}

	if (strcmp(verb, "clear") == 0) {
		out->kind = CMD_CLEAR;
		return true;
	}

	if (strcmp(verb, "reboot") == 0) {
		out->kind = CMD_REBOOT;
		return true;
	}

	if (strcmp(verb, "state") == 0) {
		char *value;

		(void)split(rest, &value);
		out->kind = CMD_STATE;

		/*
		 * The bare-word entry point, sharing its table with the JSON one so the two carriers
		 * cannot disagree about the vocabulary (ADR-0018).
		 */
		return wigwag_state_parse_word(value, &out->state);
	}

	if (strcmp(verb, "brightness") == 0) {
		char *value;
		uint8_t level;

		(void)split(rest, &value);
		out->kind = CMD_BRIGHTNESS;

		if (!lamp_brightness_parse(value, &level)) {
			return false;
		}

		out->num = level;
		out->num_valid = true;

		return true;
	}

	if (strcmp(verb, "test") == 0) {
		char *what;

		(void)split(rest, &what);

		/* Only one thing is testable so far, and it is spelled out rather than implied. */
		if (strcmp(what, "wifi") != 0) {
			out->kind = CMD_TEST_WIFI;
			return false;
		}

		out->kind = CMD_TEST_WIFI;

		return true;
	}

	if (strcmp(verb, "host") == 0) {
		char *value;

		(void)split(rest, &value);
		out->kind = CMD_HOST;

		if (strcmp(value, "on") == 0) {
			out->num = 1;
		} else if (strcmp(value, "off") == 0) {
			out->num = 0;
		} else {
			return false;
		}

		out->num_valid = true;

		return true;
	}

	if (strcmp(verb, "echo") == 0) {
		char *value;

		(void)split(rest, &value);
		out->kind = CMD_ECHO;

		if (strcmp(value, "on") == 0) {
			out->num = 1;
		} else if (strcmp(value, "off") == 0) {
			out->num = 0;
		} else {
			return false;
		}

		out->num_valid = true;

		return true;
	}

	if (strcmp(verb, "gain") == 0) {
		out->kind = CMD_GAIN;
		return parse_gain(rest, out);
	}

	if (strcmp(verb, "set") == 0) {
		const struct key_name *k;
		char *name;

		rest = split(rest, &name);
		k = key_lookup(name);

		out->kind = CMD_SET;

		if (k == NULL) {
			out->key = CMD_KEY_NONE;
			return false;
		}

		out->key = k->key;

		/*
		 * The value is the rest of the line verbatim, after one run of separating whitespace.
		 * Wi-Fi passphrases may contain spaces, so tokenising here would silently truncate a
		 * legal passphrase at its first space — and the failure would look like a wrong password.
		 */
		while (is_space(*rest)) {
			rest++;
		}

		out->value = rest;

		if (k->numeric) {
			if (!parse_u32(rest, &out->num)) {
				return false;
			}
			out->num_valid = true;
		}

		/*
		 * An empty value is legal and meaningful: `set pass` with nothing after it is how you
		 * configure an open network, and `set user` with nothing clears a broker username.
		 */
		return true;
	}

	out->kind = CMD_UNKNOWN;

	return false;
}
