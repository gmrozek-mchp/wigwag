/*
 * Host unit tests for the line editor.
 *
 * The point of testing this rather than eyeballing it on hardware is that the interesting cases are
 * all about *not* corrupting a passphrase: CRLF must not produce two lines, an arrow key must not
 * insert "[A", and an over-long line must be refused rather than truncated into a credential that
 * looks right and is not.
 *
 *   make -C firmware/tests lineedit
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../src/lineedit.h"

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

/* Captures everything echoed, so echo behaviour is asserted rather than assumed. */
static char echoed[512];
static size_t echoed_len;

static void capture(void *user, const char *data, size_t len)
{
	(void)user;

	if (echoed_len + len < sizeof(echoed)) {
		memcpy(echoed + echoed_len, data, len);
		echoed_len += len;
		echoed[echoed_len] = '\0';
	}
}

static struct lineedit le;

static void reset(void)
{
	static const struct lineedit_io io = { .out = capture };

	lineedit_init(&le, &io);
	echoed_len = 0;
	echoed[0] = '\0';
}

/** Feed a string; return the last event produced. */
static enum lineedit_event feed(const char *s)
{
	enum lineedit_event ev = LINEEDIT_NONE;

	while (*s != '\0') {
		ev = lineedit_feed(&le, *s++);
	}

	return ev;
}

static void test_simple_line(void)
{
	reset();
	CHECK(feed("show") == LINEEDIT_NONE, "no event before the terminator");
	CHECK(lineedit_feed(&le, '\r') == LINEEDIT_LINE, "CR completes the line");
	CHECK(strcmp(lineedit_line(&le), "show") == 0, "got \"%s\"", lineedit_line(&le));
}

static void test_lf_and_crlf(void)
{
	/* A script piping a file sends bare LF. */
	reset();
	feed("save");
	CHECK(lineedit_feed(&le, '\n') == LINEEDIT_LINE, "bare LF completes a line");
	CHECK(strcmp(lineedit_line(&le), "save") == 0, "LF line content");

	/* A terminal usually sends CRLF, and that must be ONE line, not two. */
	reset();
	CHECK(feed("save\r") == LINEEDIT_LINE, "CR of a CRLF completes the line");
	CHECK(lineedit_feed(&le, '\n') == LINEEDIT_NONE, "the LF of CRLF produces no second line");

	/* LFCR, which some terminals emit, is symmetric. */
	reset();
	CHECK(feed("save\n") == LINEEDIT_LINE, "LF completes");
	CHECK(lineedit_feed(&le, '\r') == LINEEDIT_NONE, "trailing CR of LFCR is swallowed");

	/* But two deliberate Enters are two lines. */
	reset();
	CHECK(feed("a\r") == LINEEDIT_LINE, "first line");
	CHECK(feed("b\r") == LINEEDIT_LINE, "second line");
	CHECK(strcmp(lineedit_line(&le), "b") == 0, "second line content");
}

static void test_empty_line_is_a_line(void)
{
	/* Leaning on Enter must advance the terminal and yield an empty line, not an error. */
	reset();
	CHECK(lineedit_feed(&le, '\r') == LINEEDIT_LINE, "bare CR is a line");
	CHECK(lineedit_line(&le)[0] == '\0', "and it is empty");
	CHECK(strcmp(echoed, "\r\n") == 0, "CRLF echoed so the cursor moves");
}

static void test_backspace(void)
{
	reset();
	feed("shwo");
	lineedit_feed(&le, 0x08);
	lineedit_feed(&le, 0x08);
	feed("ow\r");
	CHECK(strcmp(lineedit_line(&le), "show") == 0, "backspace corrected to \"%s\"",
	      lineedit_line(&le));

	/* DEL (0x7F) is what many terminals actually send for the Backspace key. */
	reset();
	feed("shx");
	lineedit_feed(&le, 0x7F);
	feed("ow\r");
	CHECK(strcmp(lineedit_line(&le), "show") == 0, "DEL also erases, got \"%s\"",
	      lineedit_line(&le));

	/* The erase must be visible: back up, blank, back up. */
	reset();
	feed("x");
	echoed_len = 0;
	echoed[0] = '\0';
	lineedit_feed(&le, 0x08);
	CHECK(strcmp(echoed, "\b \b") == 0, "erase sequence echoed, got %zu bytes", echoed_len);

	/* Backspacing an empty line must do nothing at all, not walk the cursor backwards. */
	reset();
	echoed_len = 0;
	lineedit_feed(&le, 0x08);
	CHECK(echoed_len == 0U, "no echo when there is nothing to erase");
	CHECK(lineedit_feed(&le, '\r') == LINEEDIT_LINE && lineedit_line(&le)[0] == '\0',
	      "still an empty line");
}

static void test_arrow_keys_are_inert(void)
{
	/*
	 * The case that would silently corrupt a passphrase. Up arrow is ESC [ A; without filtering,
	 * '[' and 'A' are both printable and land in the buffer.
	 */
	reset();
	feed("se");
	feed("\x1b[A");		/* Up */
	feed("\x1b[B");		/* Down */
	feed("\x1b[C");		/* Right */
	feed("\x1b[D");		/* Left */
	feed("t\r");
	CHECK(strcmp(lineedit_line(&le), "set") == 0, "arrows dropped entirely, got \"%s\"",
	      lineedit_line(&le));

	/* Sequences with numeric parameters, e.g. Home as ESC [ 1 ~, must also be swallowed whole. */
	reset();
	feed("ab");
	feed("\x1b[1~");
	feed("c\r");
	CHECK(strcmp(lineedit_line(&le), "abc") == 0, "parameterised sequence dropped, got \"%s\"",
	      lineedit_line(&le));

	/* A lone ESC followed by a normal character must not eat the character forever. */
	reset();
	feed("\x1b");
	feed("Zok\r");
	CHECK(strcmp(lineedit_line(&le), "ok") == 0,
	      "lone ESC swallows only the next byte, got \"%s\"", lineedit_line(&le));
}

static void test_ctrl_c_cancels(void)
{
	reset();
	feed("set pass oops");
	lineedit_feed(&le, 0x03);
	CHECK(strstr(echoed, "^C") != NULL, "^C is shown");
	feed("show\r");
	CHECK(strcmp(lineedit_line(&le), "show") == 0, "line abandoned, got \"%s\"",
	      lineedit_line(&le));
}

static void test_control_bytes_are_dropped(void)
{
	reset();
	lineedit_feed(&le, '\t');
	lineedit_feed(&le, 0x00);
	lineedit_feed(&le, 0x1F);
	lineedit_feed(&le, (char)0x80);
	lineedit_feed(&le, (char)0xFF);
	feed("ok\r");
	CHECK(strcmp(lineedit_line(&le), "ok") == 0, "control and high bytes dropped, got \"%s\"",
	      lineedit_line(&le));
}

static void test_overflow_is_refused_not_truncated(void)
{
	size_t i;

	/*
	 * The important refusal. A truncated passphrase would be stored, look plausible, and produce
	 * an association failure with no explanation.
	 */
	reset();
	for (i = 0; i < LINEEDIT_BUF_SZ + 50U; i++) {
		lineedit_feed(&le, 'x');
	}
	CHECK(lineedit_feed(&le, '\r') == LINEEDIT_TOO_LONG, "over-long line reported as too long");
	CHECK(lineedit_line(&le)[0] == '\0', "and not handed back at all");

	/* Exactly the largest line that fits must still work. */
	reset();
	for (i = 0; i < LINEEDIT_BUF_SZ - 1U; i++) {
		lineedit_feed(&le, 'y');
	}
	CHECK(lineedit_feed(&le, '\r') == LINEEDIT_LINE, "a full-width line is accepted");
	CHECK(strlen(lineedit_line(&le)) == LINEEDIT_BUF_SZ - 1U, "and is complete (%zu)",
	      strlen(lineedit_line(&le)));

	/* Backspacing must not launder an overflow into an apparently-valid short line. */
	reset();
	for (i = 0; i < LINEEDIT_BUF_SZ + 10U; i++) {
		lineedit_feed(&le, 'z');
	}
	for (i = 0; i < 20U; i++) {
		lineedit_feed(&le, 0x08);
	}
	CHECK(lineedit_feed(&le, '\r') == LINEEDIT_TOO_LONG,
	      "overflow survives backspacing, because the middle of the line is gone");

	/* And the editor recovers cleanly for the next line. */
	CHECK(feed("show\r") == LINEEDIT_LINE && strcmp(lineedit_line(&le), "show") == 0,
	      "usable again after an overflow");
}

static void test_echo_can_be_silenced(void)
{
	reset();
	feed("abc");
	CHECK(strcmp(echoed, "abc") == 0, "echo on by default for humans, got \"%s\"", echoed);

	/* The daemon silences it: the console and the transport share one wire (ADR-0018). */
	reset();
	lineedit_set_echo(&le, false);
	feed("state BUSY\r");
	CHECK(echoed_len == 0U, "nothing echoed when silenced, got %zu bytes", echoed_len);
	CHECK(strcmp(lineedit_line(&le), "state BUSY") == 0, "but the line still arrives");
}

static void test_no_io_callback_is_survivable(void)
{
	/* A caller that wires no output must not crash the device on the first keystroke. */
	lineedit_init(&le, NULL);
	CHECK(feed("show\r") == LINEEDIT_LINE, "works with no output sink");
	CHECK(strcmp(lineedit_line(&le), "show") == 0, "and still parses");
}

int main(void)
{
	printf("lineedit host tests\n");

	test_simple_line();
	test_lf_and_crlf();
	test_empty_line_is_a_line();
	test_backspace();
	test_arrow_keys_are_inert();
	test_ctrl_c_cancels();
	test_control_bytes_are_dropped();
	test_overflow_is_refused_not_truncated();
	test_echo_can_be_silenced();
	test_no_io_callback_is_survivable();

	printf("%d checks, %d failures\n", checks, failures);
	return (failures == 0) ? 0 : 1;
}
