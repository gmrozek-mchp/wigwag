/*
 * Settings value handling. See settings.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "settings.h"

#include <string.h>

/** Copy if it fits, refuse if it does not. Never a partial copy. */
static bool set_str(char *dst, size_t cap, const char *value)
{
	size_t len;

	if (value == NULL) {
		return false;
	}

	len = strlen(value);

	if (len >= cap) {
		return false;
	}

	memcpy(dst, value, len + 1U);

	return true;
}

bool settings_apply(struct wigwag_settings *s, enum cmd_key key, const char *value, uint32_t num)
{
	switch (key) {
	case CMD_KEY_SSID:
		return set_str(s->ssid, sizeof(s->ssid), value);
	case CMD_KEY_PASS:
		return set_str(s->pass, sizeof(s->pass), value);
	case CMD_KEY_BROKER:
		return set_str(s->broker, sizeof(s->broker), value);
	case CMD_KEY_CLIENT:
		return set_str(s->client, sizeof(s->client), value);
	case CMD_KEY_USER:
		return set_str(s->user, sizeof(s->user), value);
	case CMD_KEY_MQTTPASS:
		return set_str(s->mqttpass, sizeof(s->mqttpass), value);

	case CMD_KEY_PORT:
		/*
		 * Port 0 is not a port. cmd.c already refused non-numeric input; this is the range
		 * check, kept here because it is a property of the setting rather than of the syntax.
		 */
		if (num == 0U || num > 65535U) {
			return false;
		}
		s->port = (uint16_t)num;
		return true;

	case CMD_KEY_SEC:
		/* enum rnwf_sec_type runs 0..6; anything else would be sent to the module verbatim. */
		if (num > 6U) {
			return false;
		}
		s->sec = (uint8_t)num;
		return true;

	case CMD_KEY_NONE:
	default:
		return false;
	}
}

const char *settings_get_str(const struct wigwag_settings *s, enum cmd_key key)
{
	switch (key) {
	case CMD_KEY_SSID:
		return s->ssid;
	case CMD_KEY_PASS:
		return s->pass;
	case CMD_KEY_BROKER:
		return s->broker;
	case CMD_KEY_CLIENT:
		return s->client;
	case CMD_KEY_USER:
		return s->user;
	case CMD_KEY_MQTTPASS:
		return s->mqttpass;
	default:
		/* Numeric keys and unknown ones: the caller formats those itself. */
		return NULL;
	}
}
