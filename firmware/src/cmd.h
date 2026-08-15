/*
 * Console command vocabulary — pure parsing, no Zephyr, no I/O.
 *
 * One line reader serves two callers that look different and are not: the daemon sending `state BUSY`
 * over USB (D104) and a human typing `set ssid MyNet` into a terminal. Both are lines arriving on the
 * console UART, so the interactive console is not a second mechanism bolted onto the transport — it is
 * the transport with more verbs.
 *
 * Deliberately **not** Zephyr's shell subsystem. Measured from subsys/shell/Kconfig, even
 * SHELL_MINIMAL wants ~2.6 KB — a 2 KB thread stack, a 128-byte command buffer, a 128-byte history
 * and a 64-byte backend ring — against 3624 bytes of free SRAM on this part. That is 72 % of
 * everything left for line editing and tab completion that nothing here needs (Rule 5). This file
 * plus its adapter costs a bounded line buffer and no thread.
 *
 * Conversions are not duplicated: `state` goes through wigwag_state_parse() and `brightness` through
 * lamp_brightness_parse(), which already exist and are already tested. ADR-0018 requires
 * wigwag_state_parse() to stay the single place a string becomes a state, whatever the carrier.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CMD_H
#define CMD_H

#include "lamp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Longest line accepted. Sized for `set pass ` plus a 63-character WPA passphrase, plus slack. */
#define CMD_LINE_MAX 96

enum cmd_kind {
	CMD_NONE = 0,	/* blank line: not an error, just nothing to do */
	CMD_UNKNOWN,	/* unrecognised verb — reported, never guessed at */
	CMD_HELP,
	CMD_SHOW,	/* print settings, secrets masked */
	CMD_SET,	/* stage one setting; needs `save` to persist */
	CMD_SAVE,
	CMD_CLEAR,	/* forget all stored settings */
	CMD_REBOOT,
	CMD_STATE,	/* the transport verb (D104) */
	CMD_BRIGHTNESS,	/* applies immediately, like the retained MQTT topic */
	CMD_GAIN,	/* per-lamp calibration, immediate — the point is to judge it by eye */

	/**
	 * `echo on|off`, in `num`.
	 *
	 * Exists because the console and the transport share one wire (ADR-0018). Echo is essential
	 * for a human, who cannot otherwise see what they type, and pure noise for a host program
	 * driving the same port — so it is a runtime choice rather than a build-time one.
	 */
	CMD_ECHO,

	/**
	 * `host on|off`, in `num` — the serial analogue of `wigwag/host_online`.
	 *
	 * Over MQTT the daemon publishes that topic retained and registers `0` as its Last Will, so the
	 * broker holds the value and reports the death. A serial line has neither, so the wired path
	 * needs the host to say periodically that it is still there (D111). `off` is the orderly
	 * goodbye.
	 */
	CMD_HOST,

	/**
	 * `test wifi` — try the stored Wi-Fi and broker settings *without* committing to them.
	 *
	 * Exists because the alternative is `set transport wifi`, `save`, `reboot`, and then staring at a
	 * device that will not say why it failed. This runs the connect script on a wired device, reports
	 * which step it reached, and leaves the transport setting and the lamps alone.
	 */
	CMD_TEST_WIFI,
};

/**
 * Settable keys.
 *
 * Only these, and only spelled this way. A console that accepted approximate key names would let a
 * typo silently configure nothing, and the device gives no feedback beyond three lamps.
 */
enum cmd_key {
	CMD_KEY_NONE = 0,
	CMD_KEY_SSID,
	CMD_KEY_PASS,		/* Wi-Fi passphrase — never echoed back (see cmd_key_is_secret) */
	CMD_KEY_SEC,
	CMD_KEY_BROKER,
	CMD_KEY_PORT,
	CMD_KEY_CLIENT,
	CMD_KEY_USER,
	CMD_KEY_MQTTPASS,	/* broker password — also never echoed */

	/**
	 * `set transport usb|wifi` — which side owns the lamps (ADR-0022).
	 *
	 * A word rather than a number, because `set transport 1` is the kind of configuration nobody
	 * can read back six months later.
	 */
	CMD_KEY_TRANSPORT,
};

struct cmd {
	enum cmd_kind kind;

	enum cmd_key key;		/* CMD_SET */

	/**
	 * Borrowed from the caller's line buffer, NUL-terminated in place. Valid only until the next
	 * line is read. For CMD_SET this is the **remainder of the line verbatim**, because Wi-Fi
	 * passphrases may contain spaces.
	 */
	const char *value;

	uint32_t num;			/* CMD_BRIGHTNESS, CMD_GAIN, and numeric CMD_SET keys */
	bool num_valid;
	enum wigwag_state state;	/* CMD_STATE */
	enum lamp_id lamp;		/* CMD_GAIN */
};

/**
 * Parse one line, in place.
 *
 * `line` is modified: the verb and key are NUL-terminated where they end. Trailing CR and LF are
 * stripped, and so is leading whitespace, but whitespace *inside* a value is preserved — a
 * passphrase of "two words" is a legal passphrase.
 *
 * Returns false only for CMD_UNKNOWN and malformed arguments, with `out->kind` set so the caller can
 * say something useful. A blank line yields CMD_NONE and true.
 */
bool cmd_parse(char *line, struct cmd *out);

/**
 * Does this command mean "a host is driving this device", as opposed to "a person is configuring it"?
 *
 * Only `host` and `state` qualify. That distinction matters more than it looks: the console and the
 * transport share one wire (ADR-0018), so somebody configuring Wi-Fi over USB is typing on the same
 * line a daemon would use. Treating every command as host activity made the wired transport claim the
 * device mid-configuration, and then — ten seconds after the person stopped typing to read something —
 * drop it and show the fail-visible wigwag on a device whose Wi-Fi was perfectly healthy. That
 * is Rule 4 firing when nothing is wrong, which is the one direction it must not fire.
 *
 * `set`, `show`, `save`, `clear`, `gain`, `brightness`, `echo`, `help` and `reboot` are all things a
 * human does at a terminal. A daemon that wants the device sends `host on` every couple of seconds and
 * `state` when the state changes, so nothing real is lost by ignoring the rest.
 *
 * **`host off` is excluded too**, which is why this takes the whole command rather than just the kind.
 * It is the same verb as `host on` but means the opposite, and treating it as a claim was a real bug:
 * a bare `host off` on a device that had never seen a host would latch it to USB (D117) *and*
 * immediately distrust it, leaving the lamps wigwagging and Wi-Fi ignored until a reset. A message saying
 * "there is no host" must not be able to claim the device.
 */
bool cmd_is_host_activity(const struct cmd *c);

/** True for keys whose value must never be printed back. See `show`. */
bool cmd_key_is_secret(enum cmd_key key);

/** Stable lowercase name, for `show` and for error messages. */
const char *cmd_key_str(enum cmd_key key);

#endif /* CMD_H */
