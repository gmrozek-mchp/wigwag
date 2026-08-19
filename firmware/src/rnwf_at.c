/*
 * RNWF02 AT client.
 *
 * Structure: a line assembler feeds a small dispatcher, which either completes the step the
 * connect script is waiting on or handles an asynchronous event code. The connect sequence is a
 * linear script (see connect_script[]) rather than a state per command, because that is what it
 * actually is, and because it makes one spec rule structural instead of remembered:
 *
 *   OK means the command was *accepted*, not that the operation finished.
 *
 * A step therefore has two completion conditions — the OK, and optionally an AEC that arrives
 * later. Nothing advances on OK alone unless the step says so.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rnwf_at.h"
#include "rnwf_at_cmds.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * Timeouts. Generous where the module is doing radio work, tight where it is only parsing.
 * Wi-Fi association plus DHCP is the long pole; the spec lets +WSTAC set a connection timeout, but
 * we still need our own upper bound in case no AEC arrives at all.
 */
#define TMO_SHORT_MS	2000	/* a command the module answers immediately */
/*
 * AT+RST -> +BOOT. **Measured, not guessed**: a real RNWF02PC on firmware 3.1.0 takes
 * 3849/3894/3959/4049/4059 ms over five consecutive `test module` runs, warm, on a good supply.
 * The original 5000 left ~26 % headroom, which is not enough for a step whose spurious failure
 * drops the client into backoff and delays the link for no reason (Rule 4: a false "module is
 * dead" is worse than a slower true one). 10 s costs nothing but the time to notice a genuinely
 * absent module, which is already covered by the backoff cycle.
 */
#define TMO_BOOT_MS	10000	/* AT+RST -> +BOOT; module measures ~4.0 s */
#define TMO_WIFI_MS	30000	/* AT+WSTA=1 -> +WSTAAIP (association + DHCP) */
#define TMO_MQTT_MS	15000	/* AT+MQTTCONN -> +MQTTCONNACK (DNS + TCP + CONNECT) */
/*
 * AT+WSCN=1 -> +WSCNDONE. An active scan visits every channel in the regulatory domain, so this is
 * radio work on the same order as association rather than a command the module merely parses.
 */
#define TMO_SCAN_MS	20000

#define TMO_POLL_MS	2000	/* a bare AT must be answered promptly or the module is gone */

#define BACKOFF_MIN_MS	1000
#define BACKOFF_MAX_MS	30000

/*
 * Keepalive cadence while READY. Long enough to be negligible traffic, short enough that a dead
 * module is noticed well inside D34's 10 s fail-visible budget once the timeout is added.
 */
#define POLL_INTERVAL_MS	5000

/* ---------------------------------------------------------------- transmit */

static void at_send_raw(struct rnwf_at *at, const char *line)
{
	size_t len = strlen(line);

	if (at->io.write == NULL) {
		return;
	}

	(void)at->io.write(at->io.user, (const uint8_t *)line, len);
	(void)at->io.write(at->io.user, (const uint8_t *)RNWF_AT_EOL, 2);
}

/* ------------------------------------------------------- the connect script */

/*
 * A step builds its command into at->tx. Returning 0 means "nothing to do, skip me", which is how
 * optional credentials are handled without branching in the state machine.
 */
#define at_step at_step_public

struct at_step {
	size_t (*build)(struct rnwf_at *at);
	const char *await_aec;	/* NULL: the step is done when OK arrives */
	uint32_t timeout_ms;

	/**
	 * What this step is for, in words, for diagnostics.
	 *
	 * "it failed" is nearly useless to somebody configuring a device: a wrong passphrase, an
	 * unreachable broker and a mistyped hostname all look the same from outside. Naming the step
	 * turns three indistinguishable failures into three different messages, which is the whole
	 * value of `test wifi`.
	 */
	const char *name;
};

static size_t bld(struct rnwf_at *at, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static size_t bld(struct rnwf_at *at, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(at->tx, sizeof(at->tx), fmt, ap);
	va_end(ap);

	/*
	 * Truncation must never be sent: a half-formed AT command could be interpreted as a
	 * different, valid one. Report it as "nothing built" and let the step fail cleanly.
	 */
	if (n < 0 || (size_t)n >= sizeof(at->tx)) {
		at->tx[0] = '\0';
		return 0;
	}

	return (size_t)n;
}


/* ------------------------------------------------------- escaped composition */

/*
 * A tiny append-only builder over at->tx, so a quoted argument can be *escaped* as it is written.
 *
 * bld() cannot do this: it formats with %s, which copies a passphrase or a JSON payload verbatim into
 * a quoted field. The specification is explicit that an ASCII string is enclosed in double quotes and
 * that NUL, tab, backslash, double quote, CR, LF, bell, backspace, vertical tab, form feed and escape
 * are all written as escape sequences (AT Command Reference, "Strings"). Sending them raw produces a
 * command whose quoting the module resolves differently from what we meant — and MQTT payloads are
 * JSON, so `{"event":"press"}` inside a quoted field was malformed on every publish.
 *
 * Escaping can double a string's length, and the TX buffer is sized for the specification's maxima
 * rather than for worst-case escaping, so overflow is expected rather than exceptional. It is handled
 * the same way bld() handles truncation: nothing is sent. A half-formed AT command is worse than a
 * refused one, because it can be a *different*, valid command.
 */
struct bldr {
	struct rnwf_at *at;
	size_t len;
	bool ok;
};

static void b_init(struct bldr *b, struct rnwf_at *at)
{
	b->at = at;
	b->len = 0;
	b->ok = true;
	at->tx[0] = '\0';
}

static void b_ch(struct bldr *b, char c)
{
	if (!b->ok) {
		return;
	}

	/* Leave room for the NUL; at_send_raw() appends CRLF separately. */
	if (b->len + 1U >= sizeof(b->at->tx)) {
		b->ok = false;
		return;
	}

	b->at->tx[b->len++] = c;
}

static void b_raw(struct bldr *b, const char *s)
{
	size_t i;

	for (i = 0; s[i] != '\0'; i++) {
		b_ch(b, s[i]);
	}
}

static void b_int(struct bldr *b, uint32_t v)
{
	char digits[11];
	size_t n = 0;

	if (v == 0U) {
		b_ch(b, '0');
		return;
	}

	while (v > 0U && n < sizeof(digits)) {
		digits[n++] = (char)('0' + (v % 10U));
		v /= 10U;
	}

	while (n > 0U) {
		b_ch(b, digits[--n]);
	}
}

/** Append a quoted, escaped ASCII string argument, including its surrounding quotes. */
static void b_qstr(struct bldr *b, const char *s)
{
	size_t i;

	b_ch(b, '"');

	for (i = 0; s != NULL && s[i] != '\0'; i++) {
		char c = s[i];
		char esc = '\0';

		/* The specification's table, in full. NUL cannot appear in a C string, so it is absent. */
		switch (c) {
		case '\t': esc = 't'; break;
		case '\\': esc = '\\'; break;
		case '"':  esc = '"'; break;
		case '\r': esc = 'r'; break;
		case '\n': esc = 'n'; break;
		case '\a': esc = 'a'; break;
		case '\b': esc = 'b'; break;
		case '\v': esc = 'v'; break;
		case '\f': esc = 'f'; break;
		case 0x1B: esc = 'e'; break;
		default: break;
		}

		if (esc != '\0') {
			b_ch(b, '\\');
			b_ch(b, esc);
		} else {
			b_ch(b, c);
		}
	}

	b_ch(b, '"');
}

/** Finish: returns the length, or 0 if anything did not fit — the "skip me" convention. */
static size_t b_done(struct bldr *b)
{
	if (!b->ok) {
		b->at->tx[0] = '\0';
		return 0;
	}

	b->at->tx[b->len] = '\0';

	return b->len;
}

static size_t step_echo_off(struct rnwf_at *at)
{
	/*
	 * Before verbosity, because until this lands every command we send comes straight back at us
	 * and is counted as a dropped line. See RNWF_AT_ECHO_OFF.
	 */
	return bld(at, "%s", RNWF_AT_ECHO_OFF);
}

static size_t step_verbosity(struct rnwf_at *at)
{
	/* Early, so that every later error is ERROR:<code> and not vendor prose. */
	return bld(at, "%s", RNWF_AT_SET_VERBOSITY);
}

static size_t step_ssid(struct rnwf_at *at)
{
	struct bldr b;

	b_init(&b, at);
	b_raw(&b, RNWF_AT_WSTAC);
	b_raw(&b, "=");
	b_int(&b, RNWF_WSTAC_SSID);
	b_raw(&b, ",");
	b_qstr(&b, at->cfg->ssid);

	return b_done(&b);
}

static size_t step_sec(struct rnwf_at *at)
{
	return bld(at, "%s=%d,%d", RNWF_AT_WSTAC, RNWF_WSTAC_SEC_TYPE, at->cfg->sec_type);
}

static size_t step_cred(struct rnwf_at *at)
{
	if (at->cfg->passphrase == NULL || at->cfg->passphrase[0] == '\0') {
		return 0;	/* open network */
	}

	{
		struct bldr b;

		b_init(&b, at);
		b_raw(&b, RNWF_AT_WSTAC);
		b_raw(&b, "=");
		b_int(&b, RNWF_WSTAC_CREDENTIALS);
		b_raw(&b, ",");
		b_qstr(&b, at->cfg->passphrase);

		return b_done(&b);
	}
}

static size_t step_wifi_up(struct rnwf_at *at)
{
	return bld(at, "%s", RNWF_AT_WSTA_ENABLE);
}

static size_t step_broker_host(struct rnwf_at *at)
{
	struct bldr b;

	b_init(&b, at);
	b_raw(&b, RNWF_AT_MQTTC);
	b_raw(&b, "=");
	b_int(&b, RNWF_MQTTC_BROKER_ADDR);
	b_raw(&b, ",");
	b_qstr(&b, at->cfg->broker_host);

	return b_done(&b);
}

static size_t step_broker_port(struct rnwf_at *at)
{
	return bld(at, "%s=%d,%u", RNWF_AT_MQTTC, RNWF_MQTTC_BROKER_PORT,
		   (unsigned)at->cfg->broker_port);
}

static size_t step_client_id(struct rnwf_at *at)
{
	struct bldr b;

	b_init(&b, at);
	b_raw(&b, RNWF_AT_MQTTC);
	b_raw(&b, "=");
	b_int(&b, RNWF_MQTTC_CLIENT_ID);
	b_raw(&b, ",");
	b_qstr(&b, at->cfg->client_id);

	return b_done(&b);
}

static size_t step_username(struct rnwf_at *at)
{
	if (at->cfg->username == NULL) {
		return 0;
	}

	{
		struct bldr b;

		b_init(&b, at);
		b_raw(&b, RNWF_AT_MQTTC);
		b_raw(&b, "=");
		b_int(&b, RNWF_MQTTC_USERNAME);
		b_raw(&b, ",");
		b_qstr(&b, at->cfg->username);

		return b_done(&b);
	}
}

static size_t step_password(struct rnwf_at *at)
{
	if (at->cfg->password == NULL) {
		return 0;
	}

	{
		struct bldr b;

		b_init(&b, at);
		b_raw(&b, RNWF_AT_MQTTC);
		b_raw(&b, "=");
		b_int(&b, RNWF_MQTTC_PASSWORD);
		b_raw(&b, ",");
		b_qstr(&b, at->cfg->password);

		return b_done(&b);
	}
}

static size_t step_keep_alive(struct rnwf_at *at)
{
	if (at->cfg->keep_alive_s == 0U) {
		return 0;
	}

	return bld(at, "%s=%d,%u", RNWF_AT_MQTTC, RNWF_MQTTC_KEEP_ALIVE,
		   (unsigned)at->cfg->keep_alive_s);
}

static size_t step_lwt(struct rnwf_at *at)
{
	/*
	 * The device's own Last Will: if it drops off the network, the broker publishes 0 to the
	 * online topic on its behalf. Retained, so a subscriber that arrives later still learns the
	 * device is gone (CONTEXT.md, ADR-0003). Must precede AT+MQTTCONN.
	 */
	if (at->cfg->online_topic == NULL) {
		return 0;
	}

	{
		struct bldr b;

		b_init(&b, at);
		b_raw(&b, RNWF_AT_MQTT_LWT);
		b_raw(&b, "=0,1,");
		b_qstr(&b, at->cfg->online_topic);
		b_raw(&b, ",");
		b_qstr(&b, "0");

		return b_done(&b);
	}
}

static size_t step_mqtt_connect(struct rnwf_at *at)
{
	return bld(at, "%s", RNWF_AT_MQTT_CONNECT);
}

/* All three subscriptions differ only by topic. QoS 1: ADR-0003 wants the retained state once. */
static size_t sub_cmd(struct rnwf_at *at, const char *topic)
{
	struct bldr b;

	b_init(&b, at);
	b_raw(&b, RNWF_AT_MQTT_SUB);
	b_raw(&b, "=");
	b_qstr(&b, topic);
	b_raw(&b, ",1");

	return b_done(&b);
}

static size_t step_subscribe(struct rnwf_at *at)
{
	/* QoS 1: ADR-0003 needs the retained state delivered reliably, once. */
	return sub_cmd(at, at->cfg->state_topic);
}

static size_t step_subscribe_host(struct rnwf_at *at)
{
	/*
	 * The host's liveness marker. Retained with 0 as the daemon's Last Will, so subscribing is
	 * all it takes to learn that the host or broker has gone - which the module poll cannot
	 * see, because the module is perfectly healthy in that case (D75).
	 */
	if (at->cfg->host_online_topic == NULL) {
		return 0;
	}

	return sub_cmd(at, at->cfg->host_online_topic);
}

static size_t step_subscribe_brightness(struct rnwf_at *at)
{
	if (at->cfg->brightness_topic == NULL) {
		return 0;
	}

	/* QoS 1, retained on the broker, so the device adopts the desk's setting on every connect. */
	return sub_cmd(at, at->cfg->brightness_topic);
}

static const struct at_step connect_script[] = {
	{ step_echo_off,	NULL,			TMO_SHORT_MS,	"module responding" },
	{ step_verbosity,	NULL,			TMO_SHORT_MS,	"set error verbosity" },
	{ step_ssid,		NULL,			TMO_SHORT_MS,	"set ssid" },
	{ step_sec,		NULL,			TMO_SHORT_MS,	"set security type" },
	{ step_cred,		NULL,			TMO_SHORT_MS,	"set passphrase" },
	{ step_wifi_up,		RNWF_AEC_WSTA_GOT_IP,	TMO_WIFI_MS,	"associate and get an IP" },
	{ step_broker_host,	NULL,			TMO_SHORT_MS,	"set broker host" },
	{ step_broker_port,	NULL,			TMO_SHORT_MS,	"set broker port" },
	{ step_client_id,	NULL,			TMO_SHORT_MS,	"set client id" },
	{ step_username,	NULL,			TMO_SHORT_MS,	"set broker username" },
	{ step_password,	NULL,			TMO_SHORT_MS,	"set broker password" },
	{ step_keep_alive,	NULL,			TMO_SHORT_MS,	"set keep-alive" },
	{ step_lwt,		NULL,			TMO_SHORT_MS,	"register last will" },
	{ step_mqtt_connect,	RNWF_AEC_MQTT_CONNACK,	TMO_MQTT_MS,	"resolve, connect and CONNACK" },
	/*
	 * Each subscribe waits for its own SUBACK (+MQTTSUB). Pipelining them on OK alone gets the
	 * overlapping one refused with ERROR:8.0 at whichever step loses the race (RNWF_AEC_MQTT_SUBACK).
	 */
	{ step_subscribe,	RNWF_AEC_MQTT_SUBACK,	TMO_SHORT_MS,	"subscribe to state" },
	{ step_subscribe_host,	RNWF_AEC_MQTT_SUBACK,	TMO_SHORT_MS,	"subscribe to host_online" },
	{ step_subscribe_brightness, RNWF_AEC_MQTT_SUBACK, TMO_SHORT_MS,	"subscribe to brightness" },
};

#define SCRIPT_LEN ((uint8_t)(sizeof(connect_script) / sizeof(connect_script[0])))

static size_t step_scan(struct rnwf_at *at)
{
	return bld(at, "%s=1", RNWF_AT_SCAN);	/* 1 = active scan */
}

/*
 * The scan script. Shares the engine with the connect script because it is the same shape: send,
 * await OK, advance. Echo and verbosity first for the same reasons as ever, since a scan follows a
 * reset and the module comes back with echo on.
 *
 * The scan step waits for +WSCNDONE rather than for its OK, because OK here means only "scanning
 * started" -- the spec's rule that acceptance is not completion, which is why the engine models an
 * awaited AEC at all. Individual +WSCNIND results arrive in between and are handed to the callback.
 */
static const struct at_step scan_script[] = {
	{ step_echo_off,	NULL,			TMO_SHORT_MS,	"module responding" },
	{ step_verbosity,	NULL,			TMO_SHORT_MS,	"set error verbosity" },
	{ step_scan,		RNWF_AEC_SCAN_DONE,	TMO_SCAN_MS,	"scan for networks" },
};

#define SCAN_SCRIPT_LEN ((uint8_t)(sizeof(scan_script) / sizeof(scan_script[0])))

/*
 * Index of the association step, found in the table rather than remembered.
 *
 * There used to be a bare `step <= 4` in main.c meaning "before the network is up". Inserting
 * step_echo_off at the front of the script shifted association from 4 to 5 and the comparison went
 * quietly stale, so a failed association started advising people to check their broker. Deriving it
 * means the next inserted step cannot do that again.
 */
static uint8_t network_step_index(void)
{
	uint8_t i;

	for (i = 0; i < SCRIPT_LEN; i++) {
		if (connect_script[i].build == step_wifi_up) {
			return i;
		}
	}

	return SCRIPT_LEN;
}

bool rnwf_at_before_network(const struct rnwf_at *at)
{
	/* <=, not <: failing *at* the association step is still failing before the network is up. */
	return at->step <= network_step_index();
}

const char *rnwf_at_step_str(const struct rnwf_at *at)
{
	if (at->state == RNWF_AT_ST_READY) {
		return "connected";
	}
	/*
	 * Still the reset phase whether we are waiting or have already given up on it: until +BOOT
	 * arrives, step 0 has not been attempted and must not be blamed.
	 */
	if (at->state == RNWF_AT_ST_RESETTING || !at->boot_seen) {
		return "resetting the module";
	}
	if (at->script == NULL || at->step >= at->script_len) {
		return "?";
	}

	return at->script[at->step].name;
}

/* ------------------------------------------------------------- transitions */

static void notify_link(struct rnwf_at *at, bool linked)
{
	if (at->cb.on_link != NULL) {
		at->cb.on_link(at->cb.user, linked);
	}
}

/** Record a module-reported failure verbatim, then count it. Bounded copy; never fails. */
static void note_error(struct rnwf_at *at, const char *line)
{
	size_t i;

	for (i = 0; i + 1U < sizeof(at->last_error) && line[i] != '\0'; i++) {
		at->last_error[i] = line[i];
	}
	at->last_error[i] = '\0';

	at->errors++;
}

static void enter_backoff(struct rnwf_at *at)
{
	bool was_linked = (at->state == RNWF_AT_ST_READY);

	at->state = RNWF_AT_ST_BACKOFF;
	at->awaiting_ok = false;
	at->awaiting_aec = NULL;
	at->retry_at_ms = at->now_ms + at->backoff_ms;
	at->attempts++;

	/*
	 * Exponential, capped. Doubling before the next attempt rather than after the failure keeps
	 * the first retry quick, which matters for a transient AP glitch.
	 */
	at->backoff_ms = (at->backoff_ms >= BACKOFF_MAX_MS / 2U)
			 ? BACKOFF_MAX_MS
			 : at->backoff_ms * 2U;

	/*
	 * Rule 4 / ADR-0007: the moment the link is not trusted, say so. The lamps must go to the
	 * fail-visible wigwag rather than keep displaying a state nothing is confirming.
	 */
	if (was_linked) {
		notify_link(at, false);
	}
}

/* Issue the step at at->step, skipping any that build nothing. Enters READY at the end. */
static void run_script_from_current(struct rnwf_at *at)
{
	while (at->step < at->script_len && at->step <= at->stop_after) {
		const struct at_step *s = &at->script[at->step];

		if (s->build(at) == 0U) {
			at->step++;
			continue;	/* optional step, or a command that would not fit */
		}

		at->awaiting_ok = true;
		at->awaiting_aec = s->await_aec;
		at->deadline_ms = at->now_ms + s->timeout_ms;
		at_send_raw(at, at->tx);
		return;
	}

	at->awaiting_ok = false;
	at->awaiting_aec = NULL;

	/*
	 * Either the script ran out or it reached the boundary it was asked to stop at. Only the full
	 * connect may claim READY: that state means the displayed state is trustworthy (ADR-0007), which
	 * being merely associated does not establish.
	 */
	if (!at->ends_ready) {
		at->state = RNWF_AT_ST_STOPPED;
		return;
	}

	/* Script complete: subscribed, so the state we display can be trusted. */
	at->state = RNWF_AT_ST_READY;
	at->backoff_ms = BACKOFF_MIN_MS;
	at->next_poll_ms = at->now_ms + POLL_INTERVAL_MS;
	notify_link(at, true);
}

static void advance_step(struct rnwf_at *at)
{
	at->step++;
	run_script_from_current(at);
}

/* ------------------------------------------------------------------ parsing */

/* Copy field n (0-based, comma separated) out of an AEC argument list, unquoting if quoted. */
static bool field(const char *args, unsigned n, char *out, size_t cap)
{
	const char *p = args;
	unsigned i = 0;
	size_t len = 0;

	while (i < n) {
		p = strchr(p, ',');
		if (p == NULL) {
			return false;
		}
		p++;
		i++;
	}

	if (*p == '"') {
		/*
		 * Quoted: run to the *next* quote, so a comma inside the field is kept. Not the
		 * last quote in the line — that would swallow every following field. Fields that
		 * may themselves contain quotes (the +MQTTSUBRX payload) are never read through
		 * here; see handle_subrx().
		 */
		const char *end = strchr(p + 1, '"');

		if (end == NULL || end <= p) {
			return false;
		}
		len = (size_t)(end - (p + 1));
		if (len >= cap) {
			return false;
		}
		memcpy(out, p + 1, len);
	} else {
		const char *end = strchr(p, ',');

		len = (end != NULL) ? (size_t)(end - p) : strlen(p);
		if (len >= cap) {
			return false;
		}
		memcpy(out, p, len);
	}

	out[len] = '\0';
	return true;
}

/*
 * +MQTTSUBRX:<DUP>,<QOS>,<RETAIN>,<TOPIC_NAME>,<TOPIC_PAYLOAD>
 *
 * The payload is the last field, so rather than splitting on commas we take everything after the
 * fourth one. That matters because our own payloads are JSON containing commas *and* double
 * quotes, and the specification does not say how the module escapes a quote inside a quoted
 * string. Taking the tail verbatim is correct for any escaping scheme that does not rewrite the
 * bytes; if the module turns out to escape them, this is the one place that changes.
 */
static void handle_subrx(struct rnwf_at *at, const char *args)
{
	char topic[64];
	const char *p = args;
	unsigned commas = 0;

	if (!field(args, 3, topic, sizeof(topic))) {
		at->lines_dropped++;
		return;
	}

	while (commas < 4U) {
		p = strchr(p, ',');
		if (p == NULL) {
			at->lines_dropped++;
			return;
		}
		p++;
		commas++;
	}

	/* Strip one layer of surrounding quotes if present, in place-free fashion. */
	{
		const char *start = p;
		size_t len = strlen(start);
		char payload[RNWF_AT_RX_LINE_SZ];

		if (len >= 2U && start[0] == '"' && start[len - 1U] == '"') {
			start++;
			len -= 2U;
		}
		if (len >= sizeof(payload)) {
			at->lines_dropped++;
			return;
		}
		memcpy(payload, start, len);
		payload[len] = '\0';

		at->messages++;
		if (at->cb.on_message != NULL) {
			at->cb.on_message(at->cb.user, topic, payload);
		}
	}
}

/*
 * Decimal parse for AEC integer fields. Local rather than atoi() so the AT path pulls in no
 * conversion routines and a non-numeric field is a detectable failure rather than a silent 0.
 */
static bool parse_uint(const char *s, uint32_t *out)
{
	uint32_t v = 0;

	if (*s == '\0') {
		return false;
	}

	for (; *s != '\0'; s++) {
		if (*s < '0' || *s > '9') {
			return false;
		}
		v = (v * 10U) + (uint32_t)(*s - '0');
	}

	*out = v;
	return true;
}

static bool line_is_aec(const char *line, const char *name, const char **args)
{
	size_t n = strlen(name);

	if (strncmp(line, name, n) != 0) {
		return false;
	}

	/* Must be followed by ':' or end of line, so +MQTTCONN never matches +MQTTCONNACK. */
	if (line[n] == ':') {
		*args = line + n + 1;
		return true;
	}
	if (line[n] == '\0') {
		*args = line + n;
		return true;
	}

	return false;
}

static void handle_line(struct rnwf_at *at, const char *line)
{
	const char *args;

	/* Final result codes for the command in flight. */
	if (at->awaiting_ok && strcmp(line, RNWF_AT_OK) == 0) {
		at->awaiting_ok = false;

		if (at->state == RNWF_AT_ST_READY) {
			/*
			 * Answer to a keepalive poll: the module is alive. Not a script step, so
			 * advancing here would run off the end and re-announce the link.
			 */
			at->next_poll_ms = at->now_ms + POLL_INTERVAL_MS;
			return;
		}

		if (at->awaiting_aec == NULL) {
			advance_step(at);
		}
		/* Otherwise: accepted only. Keep waiting for the AEC, on the same deadline. */
		return;
	}

	if (strncmp(line, RNWF_AT_ERROR, strlen(RNWF_AT_ERROR)) == 0) {
		/* ATV3 makes this ERROR:<STATUS_CODE>[,"<MSG>"]; keep it, it is the whole diagnosis. */
		note_error(at, line);
		enter_backoff(at);
		return;
	}

	if (line[0] != '+') {
		/* Echo, banner text, or something we do not model. Not an error. */
		return;
	}

	/*
	 * Hand every event up before interpreting it. The caller can then show what the module actually
	 * said, which is the difference between "association failed" and knowing whether it associated
	 * at all (+WSTALU) and what the module called the failure (+WSTAERR:<code>).
	 */
	if (at->cb.on_event != NULL) {
		at->cb.on_event(at->cb.user, line);
	}

	/* The AEC the current step is waiting for. */
	if (at->awaiting_aec != NULL && line_is_aec(line, at->awaiting_aec, &args)) {
		if (strcmp(at->awaiting_aec, RNWF_AEC_MQTT_CONNACK) == 0) {
			char reason[8];
			uint32_t code;

			/* +MQTTCONNACK:<CONNACK_FLAGS>,<CONN_REASON_CODE>, 0 = success. */
			if (!field(args, 1, reason, sizeof(reason)) ||
			    !parse_uint(reason, &code) ||
			    code != (uint32_t)RNWF_MQTT_CONN_SUCCESS) {
				note_error(at, line);
				enter_backoff(at);
				return;
			}
		}

		if (strcmp(at->awaiting_aec, RNWF_AEC_MQTT_SUBACK) == 0) {
			uint32_t code;

			/* Granted subscriptions echo the QoS; 128 (or anything higher) is a refusal. */
			if (!parse_uint(args, &code) || code > RNWF_MQTT_SUBACK_MAX_QOS) {
				note_error(at, line);
				enter_backoff(at);
				return;
			}
		}

		/*
		 * A link-local address is not an address we can use.
		 *
		 * +WSTAAIP fires for *every* automatic assignment, and with IPv6 SLAAC enabled the module
		 * announces its fe80:: address the instant the link comes up -- before DHCPv4 has produced
		 * anything. Completing the step on that would declare "the network accepted us" while the
		 * module still has no route to an IPv4 broker, and then the MQTT steps race DHCP: observed
		 * on hardware as +WSTAAIP:1,"FE80::DF6A:DC30:93BF:60AE" followed by inconsistent failures.
		 * Keep waiting instead; the step's own timeout still bounds it.
		 */
		if (strcmp(at->awaiting_aec, RNWF_AEC_WSTA_GOT_IP) == 0) {
			char ip[40];

			if (field(args, 1, ip, sizeof(ip)) &&
			    (strncmp(ip, "FE80", 4) == 0 || strncmp(ip, "fe80", 4) == 0)) {
				at->aecs_ignored++;
				return;
			}
		}

		/*
		 * Report the address the moment it arrives. Bring-up wants to *see* the DHCP lease --
		 * "associated" and "has an address" are different claims, and only the second means the
		 * network accepted us. Reported rather than stored: holding an IPv6 address without
		 * truncating costs 40 bytes, and truncating one would be a diagnostic that lies.
		 */
		if (at->cb.on_ip != NULL && strcmp(at->awaiting_aec, RNWF_AEC_WSTA_GOT_IP) == 0) {
			char ip[40];

			if (field(args, 1, ip, sizeof(ip))) {
				at->cb.on_ip(at->cb.user, ip);
			}
		}

		at->awaiting_aec = NULL;
		advance_step(at);
		return;
	}

	if (line_is_aec(line, RNWF_AEC_BOOT, &args)) {
		if (at->state == RNWF_AT_ST_RESETTING) {
			at->boot_seen = true;
			at->state = RNWF_AT_ST_SCRIPT;
			at->step = 0;
			run_script_from_current(at);
		}
		return;
	}

	if (line_is_aec(line, RNWF_AEC_SCAN_RESULT, &args)) {
		/*
		 * One network. Handed over raw: the spec's own field order (RSSI, security, channel,
		 * BSSID, SSID) says more to a human than anything this layer would rearrange, and a scan
		 * result is never acted on automatically.
		 */
		if (at->cb.on_scan != NULL) {
			at->cb.on_scan(at->cb.user, args);
		} else {
			at->aecs_ignored++;
		}
		return;
	}

	if (line_is_aec(line, RNWF_AEC_MQTT_SUBRX, &args)) {
		handle_subrx(at, args);
		return;
	}

	/*
	 * Losing Wi-Fi or MQTT at any point invalidates what we are displaying, including
	 * mid-script. Restart rather than try to resume from an unknown position.
	 */
	if (line_is_aec(line, RNWF_AEC_WSTA_LINK_DOWN, &args) ||
	    line_is_aec(line, RNWF_AEC_WSTA_ERROR, &args)) {
		enter_backoff(at);
		return;
	}

	/*
	 * "+MQTTCONN:<CONN_STATE>", 0 = not connected. This is how the module reports a broker or
	 * network loss that leaves the module itself healthy, so the keepalive poll would keep
	 * succeeding and never notice (D75). Distinct from +MQTTCONNACK, which line_is_aec keeps
	 * separate by requiring ':' or end-of-line after the name.
	 */
	if (line_is_aec(line, RNWF_AEC_MQTT_CONN_STATE, &args)) {
		uint32_t state;

		if (parse_uint(args, &state) && state == 0U) {
			enter_backoff(at);
		} else {
			at->aecs_ignored++;
		}
		return;
	}

	/*
	 * A late, command-specific failure: "+CMDNAME:ERROR:<code>" (spec's example is
	 * +SOCKBR:ERROR:4). Any AEC carrying ERROR is a failure of whatever we were doing.
	 */
	if (strstr(line, ":" RNWF_AT_ERROR) != NULL) {
		note_error(at, line);
		enter_backoff(at);
		return;
	}

	/* Well-formed, and of no interest to us: +WSTALU, +MQTTSUBLST, read responses, and so on. */
	at->aecs_ignored++;
}

/* --------------------------------------------------------- line  assembler */

/*
 * Delimiting on either CR or LF, and ignoring empty lines, is what makes the specification's AEC
 * framing fall out for free: an AEC is <CR>+NAME:INFO<CR><LF>, so the leading CR simply closes an
 * empty line. Responses are <RESPONSE><CR><LF>. Neither needs a special case.
 */
void rnwf_at_feed(struct rnwf_at *at, const uint8_t *data, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		char c = (char)data[i];

		if (c == '\r' || c == '\n') {
			if (at->rx_overflow) {
				/* Drop the whole oversized line; never parse a fragment. */
				at->rx_overflow = false;
				at->rx_len = 0;
				at->lines_dropped++;
				continue;
			}
			if (at->rx_len == 0U) {
				continue;	/* empty line: the AEC's leading CR, or CRLF */
			}
			at->rx[at->rx_len] = '\0';
			at->rx_len = 0;
			handle_line(at, at->rx);
			continue;
		}

		if (at->rx_len + 1U >= sizeof(at->rx)) {
			at->rx_overflow = true;
			continue;
		}

		at->rx[at->rx_len++] = c;
	}
}

/* -------------------------------------------------------------------- API */

void rnwf_at_init(struct rnwf_at *at, const struct rnwf_at_config *cfg,
		  const struct rnwf_at_io *io, const struct rnwf_at_callbacks *cb)
{
	memset(at, 0, sizeof(*at));
	at->cfg = cfg;
	at->io = *io;
	if (cb != NULL) {
		at->cb = *cb;
	}
	at->state = RNWF_AT_ST_IDLE;
	at->backoff_ms = BACKOFF_MIN_MS;
}

static void start_script(struct rnwf_at *at, uint32_t now_ms, const struct at_step *script,
			 uint8_t len, uint8_t stop_after, bool ends_ready)
{
	at->script = script;
	at->script_len = len;
	at->stop_after = stop_after;
	at->ends_ready = ends_ready;

	at->now_ms = now_ms;
	at->state = RNWF_AT_ST_RESETTING;
	at->step = 0;
	at->boot_seen = false;
	at->last_error[0] = '\0';
	at->awaiting_ok = false;
	at->awaiting_aec = NULL;
	at->rx_len = 0;
	at->rx_overflow = false;
	at->deadline_ms = now_ms + TMO_BOOT_MS;
	at_send_raw(at, RNWF_AT_RESET);
}

void rnwf_at_start(struct rnwf_at *at, uint32_t now_ms)
{
	start_script(at, now_ms, connect_script, SCRIPT_LEN, RNWF_AT_RUN_ALL, true);
}

void rnwf_at_reset_only(struct rnwf_at *at, uint32_t now_ms)
{
	/* An empty script: +BOOT completes the reset phase and there is nothing after it. */
	start_script(at, now_ms, connect_script, 0, RNWF_AT_RUN_ALL, false);
}

void rnwf_at_start_network_only(struct rnwf_at *at, uint32_t now_ms)
{
	start_script(at, now_ms, connect_script, SCRIPT_LEN, network_step_index(), false);
}

void rnwf_at_scan(struct rnwf_at *at, uint32_t now_ms)
{
	start_script(at, now_ms, scan_script, SCAN_SCRIPT_LEN, RNWF_AT_RUN_ALL, false);
}

void rnwf_at_tick(struct rnwf_at *at, uint32_t now_ms)
{
	at->now_ms = now_ms;

	switch (at->state) {
	case RNWF_AT_ST_BACKOFF:
		/* Signed comparison so the wrap at 2^32 ms does not strand the client. */
		if ((int32_t)(now_ms - at->retry_at_ms) >= 0) {
			/*
			 * The same script, not rnwf_at_start(): a retry after a failed diagnostic run must
			 * not quietly turn into a full connect attempt the caller never asked for.
			 */
			start_script(at, now_ms, at->script, at->script_len, at->stop_after,
				     at->ends_ready);
		}
		return;

	case RNWF_AT_ST_RESETTING:
	case RNWF_AT_ST_SCRIPT:
		if ((int32_t)(now_ms - at->deadline_ms) >= 0) {
			at->timeouts++;
			enter_backoff(at);
		}
		return;

	case RNWF_AT_ST_READY:
		if (at->awaiting_ok) {
			if ((int32_t)(now_ms - at->deadline_ms) >= 0) {
				/* The module stopped answering: stop claiming a link. */
				at->timeouts++;
				enter_backoff(at);
			}
		} else if ((int32_t)(now_ms - at->next_poll_ms) >= 0) {
			at->polls++;
			at->awaiting_ok = true;
			at->awaiting_aec = NULL;
			at->deadline_ms = now_ms + TMO_POLL_MS;
			at_send_raw(at, RNWF_AT_PING);
		}
		return;

	case RNWF_AT_ST_IDLE:
	case RNWF_AT_ST_STOPPED:
	default:
		return;
	}
}

int rnwf_at_publish(struct rnwf_at *at, const char *topic, const char *payload, bool retain)
{
	if (at->state != RNWF_AT_ST_READY) {
		return -1;
	}

	/*
	 * AT+MQTTPUB=<DUP>,<QOS>,<RETAIN>,<TOPIC_NAME_ID>,<TOPIC_PAYLOAD>
	 *
	 * The payload is JSON, so it is full of double quotes and must be escaped. Built rather than
	 * formatted for exactly that reason; before this it was sent raw and every publish was malformed.
	 */
	{
		struct bldr b;

		b_init(&b, at);
		b_raw(&b, RNWF_AT_MQTT_PUB);
		b_raw(&b, "=0,0,");
		b_int(&b, retain ? 1U : 0U);
		b_raw(&b, ",");
		b_qstr(&b, topic);
		b_raw(&b, ",");
		b_qstr(&b, payload);

		if (b_done(&b) == 0U) {
			return -1;
		}
	}

	at_send_raw(at, at->tx);
	return 0;
}
