/*
 * Character-level line input: bytes in, complete lines out.
 *
 * **This is the layer designed to be thrown away.** It exists as its own file, with an injected
 * output callback and no Zephyr and no knowledge of the command vocabulary, so that swapping it for
 * something richer — embedded-cli, Zephyr's shell, a full readline — means replacing one file that
 * emits "here is a complete line" events. Everything with decisions in it lives in cmd.c and the
 * console adapter, which do not change.
 *
 * What it deliberately does *not* do: history, tab completion, or cursor movement within a line.
 * Those were measured against the alternatives (JOURNAL.md, 2026-08-14) — Zephyr's shell does not
 * even link on this part, overflowing RAM by 464 bytes — and the one feature worth keeping from an
 * interactive CLI is backspace, because retyping a 63-character passphrase after one typo is the
 * actual pain. Arrow keys are made *inert* rather than useful, which is the next most valuable thing:
 * without the escape filter below, pressing Up inserts a literal "[A" into your passphrase.
 *
 * The output callback rather than printk is the same trick struct rnwf_at_io plays: it keeps this
 * host-testable, so the echo and backspace behaviour is asserted rather than eyeballed on hardware.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LINEEDIT_H
#define LINEEDIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Line buffer size.
 *
 * Sized for the longest thing anyone types: `set pass ` plus a 63-character WPA-2 passphrase is 72,
 * so 96 leaves room without being generous with an 8 KB part's SRAM. The console adapter asserts at
 * compile time that this is at least CMD_LINE_MAX, so the two cannot drift.
 */
#define LINEEDIT_BUF_SZ 96

enum lineedit_event {
	LINEEDIT_NONE = 0,	/* byte consumed; nothing further to do */
	LINEEDIT_LINE,		/* a complete line is ready — see lineedit_line() */

	/**
	 * The line ran past the buffer and has been discarded.
	 *
	 * A distinct event rather than silent truncation, on purpose. Quietly accepting the first 96
	 * characters of a too-long passphrase would store a wrong credential that *looks* right, and
	 * the device would report a failed association forever with no clue why. Rule 4 applies to
	 * input as much as to lamps: say "too long" rather than guess.
	 */
	LINEEDIT_TOO_LONG,
};

/** Byte sink for echo. Never called when echo is off. */
struct lineedit_io {
	void (*out)(void *user, const char *data, size_t len);
	void *user;
};

struct lineedit {
	struct lineedit_io io;

	char buf[LINEEDIT_BUF_SZ];
	size_t len;
	bool overflow;

	/**
	 * Echo is on by default, because the first user is a human in a terminal who cannot otherwise
	 * see what they type. The daemon turns it off (`echo off`) so the wire carries only protocol
	 * — the console and the transport share one stream by ADR-0018, and echo is noise on the
	 * machine half.
	 */
	bool echo;

	uint8_t esc;		/* escape-sequence filter state; see lineedit.c */
	char pending_eol;	/* the CR or LF just seen, so CRLF counts as one line ending */
};

void lineedit_init(struct lineedit *le, const struct lineedit_io *io);

/** Feed one received byte. */
enum lineedit_event lineedit_feed(struct lineedit *le, char c);

/**
 * The completed line, NUL-terminated and **mutable** — cmd_parse() tokenises in place.
 *
 * Valid until the next lineedit_feed() call that returns a line.
 */
char *lineedit_line(struct lineedit *le);

void lineedit_set_echo(struct lineedit *le, bool on);

#endif /* LINEEDIT_H */
