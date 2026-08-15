/*
 * Character-level line input. See lineedit.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lineedit.h"

#include <string.h>

/* Escape-filter states. */
#define ESC_IDLE	0
#define ESC_SEEN	1	/* got 0x1B, expecting '[' or 'O' */
#define ESC_BRACKET	2	/* got the introducer, swallowing until the final byte */

#define CH_BS		0x08
#define CH_DEL		0x7F
#define CH_ESC		0x1B
#define CH_CTRL_C	0x03

static void emit(struct lineedit *le, const char *s, size_t len)
{
	if (le->echo && le->io.out != NULL && len > 0U) {
		le->io.out(le->io.user, s, len);
	}
}

void lineedit_init(struct lineedit *le, const struct lineedit_io *io)
{
	memset(le, 0, sizeof(*le));

	if (io != NULL) {
		le->io = *io;
	}

	le->echo = true;
}

void lineedit_set_echo(struct lineedit *le, bool on)
{
	le->echo = on;
}

char *lineedit_line(struct lineedit *le)
{
	return le->buf;
}

static enum lineedit_event finish(struct lineedit *le)
{
	bool was_overflow = le->overflow;

	emit(le, "\r\n", 2);

	le->buf[le->len] = '\0';
	le->len = 0;
	le->overflow = false;

	if (was_overflow) {
		le->buf[0] = '\0';	/* refuse to hand back a truncated line */
		return LINEEDIT_TOO_LONG;
	}

	return LINEEDIT_LINE;
}

enum lineedit_event lineedit_feed(struct lineedit *le, char c)
{
	unsigned char u = (unsigned char)c;

	/*
	 * Swallow ANSI escape sequences. Arrow keys arrive as ESC '[' 'A'..'D', and without this the
	 * '[' and the letter are both printable and would land in the buffer — so pressing Up while
	 * typing a passphrase would silently corrupt it. Making them inert is most of the value of
	 * handling them at all; making them *work* would mean history, which is not worth the RAM.
	 */
	if (le->esc == ESC_SEEN) {
		le->esc = (u == '[' || u == 'O') ? ESC_BRACKET : ESC_IDLE;
		return LINEEDIT_NONE;
	}

	if (le->esc == ESC_BRACKET) {
		/* Parameter bytes are 0x30-0x3F; anything else terminates the sequence. */
		if (u < 0x30U || u > 0x3FU) {
			le->esc = ESC_IDLE;
		}
		return LINEEDIT_NONE;
	}

	if (u == CH_ESC) {
		le->esc = ESC_SEEN;
		return LINEEDIT_NONE;
	}

	/*
	 * CR and LF both end a line, and CRLF must end exactly one. Terminals send CR, some send
	 * CRLF, and a script piping a file sends LF — all three have to behave.
	 */
	if (c == '\r' || c == '\n') {
		if (le->pending_eol != '\0' && le->pending_eol != c) {
			/* Second half of a CRLF pair: already handled. */
			le->pending_eol = '\0';
			return LINEEDIT_NONE;
		}

		le->pending_eol = c;

		return finish(le);
	}

	le->pending_eol = '\0';

	if (u == CH_BS || u == CH_DEL) {
		if (le->len > 0U) {
			le->len--;
			/* Back up, overwrite with a space, back up again — the portable erase. */
			emit(le, "\b \b", 3);
		}

		/*
		 * Backspacing does not clear an overflow. Once characters have been dropped the line is
		 * no longer what was typed, and letting a few backspaces make it "fit" would hand back
		 * a line missing something from its middle.
		 */
		return LINEEDIT_NONE;
	}

	if (u == CH_CTRL_C) {
		le->len = 0;
		le->overflow = false;
		emit(le, "^C\r\n", 4);
		return LINEEDIT_NONE;
	}

	/* Printable ASCII only. Anything else — tabs, other control bytes, high bytes — is dropped. */
	if (u < 0x20U || u > 0x7EU) {
		return LINEEDIT_NONE;
	}

	if (le->len >= (LINEEDIT_BUF_SZ - 1U)) {
		/* No room. Remember it, stop echoing, and let finish() report the discard. */
		le->overflow = true;
		return LINEEDIT_NONE;
	}

	le->buf[le->len++] = c;
	emit(le, &c, 1);

	return LINEEDIT_NONE;
}
