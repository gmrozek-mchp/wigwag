/*
 * The settings themselves: what is configurable, and how a value is accepted or refused.
 *
 * Pure — no Zephyr, no NVS, no I/O — so the part with judgement in it (does this value fit? is it
 * refused or truncated?) is host-tested, the same split as lamp.c/lamp_pwm.c and cmd.c/console.c.
 * Persistence lives next door in settings_store.h.
 *
 * **Costs ~300 bytes of RAM, knowingly (Rule 5).** `struct rnwf_at_config` borrows `const char *`
 * that must outlive the AT client, and these used to be string literals in flash costing nothing.
 * NVS hands back copies rather than pointers into the array — there is no "give me the address of
 * this value" API, even though flash is memory-mapped here — so the strings must live somewhere
 * writable. That is the price of not having to rebuild to change a Wi-Fi password (D37/D56).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include "cmd.h"
#include "lamp.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Field sizes come from what the protocols allow, not from guesses: SSID 32 (IEEE 802.11), WPA-2
 * passphrase 63, hostname 64. Each has room for the NUL. The AT specification permits a 256-byte
 * broker password (rnwf_at.h sizes its transmit buffer from that); 64 is the practical limit accepted
 * here, and a longer one is refused by the line editor rather than truncated.
 */
#define SET_SSID_SZ	33
#define SET_PASS_SZ	64
#define SET_HOST_SZ	65
#define SET_CLIENT_SZ	33
#define SET_USER_SZ	33
#define SET_MQTTPASS_SZ	65

struct wigwag_settings {
	char ssid[SET_SSID_SZ];
	char pass[SET_PASS_SZ];
	char broker[SET_HOST_SZ];
	char client[SET_CLIENT_SZ];
	char user[SET_USER_SZ];
	char mqttpass[SET_MQTTPASS_SZ];

	uint16_t port;
	uint8_t sec;

	/** Startup brightness, so a wired unit with no broker still respects a dimmed desk. */
	uint8_t brightness;

	/** Per-lamp calibration (D91), now trimmable by eye on a built unit instead of in devicetree. */
	uint8_t gain[LAMP_COUNT];
};

/**
 * Apply one parsed `set` command.
 *
 * Returns false if the key is unknown or the value does not fit. **Refusal, never truncation**: a
 * silently shortened passphrase is stored, looks plausible, and produces an association failure with
 * nothing to point at — the same reasoning as LINEEDIT_TOO_LONG.
 */
bool settings_apply(struct wigwag_settings *s, enum cmd_key key, const char *value, uint32_t num);

/** The value for `show`, as text. NULL for keys that are not strings. */
const char *settings_get_str(const struct wigwag_settings *s, enum cmd_key key);

#endif /* SETTINGS_H */
