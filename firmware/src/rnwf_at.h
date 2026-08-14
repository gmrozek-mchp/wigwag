/*
 * RNWF02 AT client — public interface.
 *
 * Deliberately free of Zephyr headers so the line assembler, request/response engine and connect
 * state machine can be built and tested with plain clang on any host, including macOS where
 * native_sim does not exist (ADR-0015, D66). The Zephyr UART adapter and the host PTY adapter both
 * sit behind struct rnwf_at_io.
 *
 * No dynamic allocation. One bounded buffer per direction (ADR-0002, ADR-0008, Rule 5).
 *
 * The caller drives it: rnwf_at_feed() with received bytes, rnwf_at_tick() with a monotonic
 * millisecond clock. Nothing here blocks and nothing here reads the clock itself, so tests can
 * step time by hand.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RNWF_AT_H
#define RNWF_AT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Sized from the AT specification's own field maxima (see rnwf_at_cmds.h), not guessed. The
 * longest command emitted is a broker password set: AT+MQTTC=5,"<256>" + CRLF = 273 bytes.
 */
#define RNWF_AT_TX_BUF_SZ	288
#define RNWF_AT_RX_LINE_SZ	256

/** Byte transport. Returns bytes written, or negative on failure. */
struct rnwf_at_io {
	int (*write)(void *user, const uint8_t *data, size_t len);
	void *user;
};

/**
 * Events out of the client. Both are optional.
 *
 * on_message delivers a subscribed MQTT message (+MQTTSUBRX). Pointers are only valid for the
 * duration of the call — copy what you need.
 *
 * on_link reports the link condition in CONTEXT.md's sense: true once subscribed and trusted,
 * false the moment that stops being true. ADR-0007 hangs off this — false must drive the amber
 * flicker rather than leave a stale lamp lit.
 */
struct rnwf_at_callbacks {
	void (*on_message)(void *user, const char *topic, const char *payload);
	void (*on_link)(void *user, bool linked);
	void *user;
};

/**
 * Connection parameters. All strings are borrowed, never copied, and must outlive the client —
 * in the firmware they are Kconfig string literals in flash, which is what keeps this off the
 * RAM budget. NULL username or password skips that configuration step.
 */
struct rnwf_at_config {
	const char *ssid;
	const char *passphrase;
	int sec_type;			/* enum rnwf_sec_type */

	const char *broker_host;	/* hostname by preference, per ADR-0013 */
	uint16_t broker_port;
	const char *client_id;
	const char *username;
	const char *password;
	uint16_t keep_alive_s;

	const char *state_topic;	/* subscribed: host -> device */
	const char *online_topic;	/* device -> host, and the LWT topic */
};

/**
 * Where the client is in its connect script.
 *
 * Exposed because the tests assert on it, and because the firmware wants to show progress on the
 * lamps. RNWF_AT_READY is the only state in which the displayed state can be trusted.
 */
enum rnwf_at_state {
	RNWF_AT_ST_IDLE,	/* constructed, not started */
	RNWF_AT_ST_RESETTING,	/* AT+RST sent, waiting for +BOOT */
	RNWF_AT_ST_SCRIPT,	/* walking the connect script */
	RNWF_AT_ST_READY,	/* subscribed; link is trusted */
	RNWF_AT_ST_BACKOFF,	/* failed; waiting to retry */
};

struct rnwf_at {
	const struct rnwf_at_config *cfg;
	struct rnwf_at_io io;
	struct rnwf_at_callbacks cb;

	enum rnwf_at_state state;

	/* Connect script position and what the current step is waiting for. */
	uint8_t step;
	bool awaiting_ok;
	const char *awaiting_aec;	/* NULL when the step completes on OK alone */
	uint32_t deadline_ms;

	/* Backoff */
	uint32_t backoff_ms;
	uint32_t retry_at_ms;
	uint32_t attempts;

	uint32_t now_ms;

	/* One bounded buffer per direction. */
	char tx[RNWF_AT_TX_BUF_SZ];
	char rx[RNWF_AT_RX_LINE_SZ];
	size_t rx_len;
	bool rx_overflow;

	/* Diagnostics — cheap counters, useful when the only output is a lamp. */
	uint32_t lines_dropped;
	uint32_t errors;
	uint32_t timeouts;
	uint32_t messages;
};

void rnwf_at_init(struct rnwf_at *at, const struct rnwf_at_config *cfg,
		  const struct rnwf_at_io *io, const struct rnwf_at_callbacks *cb);

/** Begin the connect sequence. Safe to call again; restarts from the reset. */
void rnwf_at_start(struct rnwf_at *at, uint32_t now_ms);

/** Feed received bytes. Total: malformed input is counted and dropped, never fatal. */
void rnwf_at_feed(struct rnwf_at *at, const uint8_t *data, size_t len);

/** Advance timers and the script. Call regularly with a monotonic millisecond clock. */
void rnwf_at_tick(struct rnwf_at *at, uint32_t now_ms);

/** Publish. Returns 0 on success, -1 if not ready or the command would not fit. */
int rnwf_at_publish(struct rnwf_at *at, const char *topic, const char *payload, bool retain);

static inline bool rnwf_at_is_linked(const struct rnwf_at *at)
{
	return at->state == RNWF_AT_ST_READY;
}

#endif /* RNWF_AT_H */
