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

/*
 * The script step type is opaque here: the tables live in rnwf_at.c and nothing outside chooses or
 * builds a step. Only the pointer needs to be storable.
 */
struct at_step_public;

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
 * false the moment that stops being true. ADR-0007 hangs off this — false must drive the
 * flicker rather than leave a stale lamp lit.
 */
struct rnwf_at_callbacks {
	void (*on_message)(void *user, const char *topic, const char *payload);
	void (*on_link)(void *user, bool linked);

	/**
	 * The address the module was given, the moment +WSTAAIP arrives. Optional.
	 *
	 * A callback rather than a stored field on purpose: an IPv6 address needs 40 bytes to hold
	 * without truncating, and truncating one would be a diagnostic that lies (Rule 4). Reported as
	 * it happens instead, for the cost of one pointer.
	 */
	void (*on_ip)(void *user, const char *ip);

	/**
	 * One scan result, as the module's raw +WSCNIND info field. NULL means the scan is finished.
	 *
	 * Raw rather than parsed because this is a human-facing diagnostic and the specification's own
	 * field order -- RSSI, security, channel, BSSID, SSID -- is more informative than anything this
	 * layer would invent from it. Optional.
	 */
	void (*on_scan)(void *user, const char *info);

	/**
	 * Every asynchronous event the module reports, verbatim, before this client acts on it. Optional.
	 *
	 * Exists because the events we do not model were being discarded into a counter that nothing
	 * printed, which cost a bring-up session: with +WSTALU invisible there was no way to tell an
	 * association that failed from one that succeeded and then lost DHCP, and +WSTAERR's code -- the
	 * module's own verdict -- was thrown away with it. A diagnostic that hides the device's testimony
	 * is not a diagnostic.
	 */
	void (*on_event)(void *user, const char *line);

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

	/*
	 * The host's own liveness marker (CONTEXT.md): retained, with 0 published as the daemon's
	 * Last Will. Subscribed so a dead host or broker becomes visible; NULL to skip.
	 */
	const char *host_online_topic;

	/** Runtime master brightness, 0-255, retained (CONTEXT.md). NULL to skip. */
	const char *brightness_topic;
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

	/**
	 * The script it was asked to run finished, and it was not the full connect.
	 *
	 * Distinct from READY because READY means "the state on the lamps can be trusted" (ADR-0007), and
	 * a module that is merely associated, or has merely finished a scan, confirms nothing about the
	 * host's state. Bring-up wants to stop at these boundaries; the product never does.
	 */
	RNWF_AT_ST_STOPPED,
};

struct rnwf_at {
	const struct rnwf_at_config *cfg;
	struct rnwf_at_io io;
	struct rnwf_at_callbacks cb;

	enum rnwf_at_state state;

	/*
	 * Which script is running, and how far.
	 *
	 * Parameterised so bring-up can stop at a boundary that means something on its own: associated
	 * but not connected to a broker, or a scan and nothing else. The product always runs the whole
	 * connect script, so the default is exactly the old behaviour.
	 */
	const struct at_step_public *script;
	uint8_t script_len;
	uint8_t stop_after;	/* last step index to run; RNWF_AT_RUN_ALL for the whole script */
	bool ends_ready;	/* completing means READY (a trusted link) rather than STOPPED */

	/* Connect script position and what the current step is waiting for. */
	uint8_t step;

	/*
	 * Has the module confirmed a reboot (+BOOT) since the last rnwf_at_start()?
	 *
	 * Only rnwf_at_step_str() reads it, and only so a failure gets attributed to the right step.
	 * step stays 0 across a reset-phase timeout, so without this a module that never answered at
	 * all was reported as failing at "module responding" — naming a command that was never sent,
	 * on a link that was never established. During bring-up that is the difference between "the
	 * UART is miswired" and "the module refused ATV3" (Rule 4: fail-visible, and do not lie).
	 */
	bool boot_seen;

	bool awaiting_ok;
	const char *awaiting_aec;	/* NULL when the step completes on OK alone */
	uint32_t deadline_ms;

	/* Backoff */
	uint32_t backoff_ms;
	uint32_t retry_at_ms;
	uint32_t attempts;

	/*
	 * Keepalive. While READY the client polls the module with a bare AT and requires an OK,
	 * because a module that dies silently otherwise leaves the client believing it is linked
	 * forever — observed on hardware, and exactly what ADR-0007 forbids (D75).
	 */
	uint32_t next_poll_ms;

	uint32_t now_ms;

	/* One bounded buffer per direction. */
	char tx[RNWF_AT_TX_BUF_SZ];
	char rx[RNWF_AT_RX_LINE_SZ];
	size_t rx_len;
	bool rx_overflow;

	/*
	 * Diagnostics — cheap counters, useful when the only output is a lamp.
	 *
	 * lines_dropped and aecs_ignored are deliberately separate: a recognised event we simply
	 * do not model (+WSTALU carries BSSID and channel, which we never use) is normal, while a
	 * malformed or oversized line is not. Conflating them made a healthy run report
	 * "dropped=3" and look broken.
	 */
	uint32_t lines_dropped;	/* malformed, oversized, or unparseable */
	uint32_t aecs_ignored;	/* well-formed AEC we have no use for */
	uint32_t errors;

	/*
	 * The last failure the module reported, verbatim and bounded (24 B).
	 *
	 * ATV3 answers a rejected command with ERROR:<STATUS_CODE>, and the code is the difference
	 * between "the module does not know that command" (0.3) and "invalid parameter" (0.4) and
	 * "access denied" (0.10) and a subsystem-specific failure such as 8.0 MQTT_ERROR. This used to
	 * be counted and dropped -- the comment at the site even said "the code is diagnostic only" --
	 * which left "the module rejected it" as the whole of the diagnosis.
	 */
	char last_error[24];
	uint32_t timeouts;
	uint32_t messages;
	uint32_t polls;		/* keepalive polls issued while READY */
};

void rnwf_at_init(struct rnwf_at *at, const struct rnwf_at_config *cfg,
		  const struct rnwf_at_io *io, const struct rnwf_at_callbacks *cb);

#define RNWF_AT_RUN_ALL 0xFFU

/** Begin the full connect sequence, ending in READY. Safe to call again; restarts from the reset. */
void rnwf_at_start(struct rnwf_at *at, uint32_t now_ms);

/**
 * Reset the module and stop as soon as it says +BOOT. Sends AT+RST and nothing else.
 *
 * The narrowest liveness check there is, and it proves both directions: the module only reboots if it
 * received the command, and the banner only arrives if the host can hear it.
 */
void rnwf_at_reset_only(struct rnwf_at *at, uint32_t now_ms);

/**
 * Run the connect script only as far as Wi-Fi association, then stop in RNWF_AT_ST_STOPPED.
 *
 * "On the network" is a result worth having by itself: it separates a credentials or radio problem
 * from a broker problem, which are otherwise one indistinguishable failure.
 */
void rnwf_at_start_network_only(struct rnwf_at *at, uint32_t now_ms);

/**
 * Reset the module, then scan for networks (AT+WSCN=1). Results arrive via callbacks.on_scan.
 *
 * Needs no configuration at all, which is what makes it the right instrument when association fails:
 * it answers whether the network is even visible, on a band this module can use, advertising the
 * security the settings claim.
 */
void rnwf_at_scan(struct rnwf_at *at, uint32_t now_ms);

/** Feed received bytes. Total: malformed input is counted and dropped, never fatal. */
void rnwf_at_feed(struct rnwf_at *at, const uint8_t *data, size_t len);

/** Advance timers and the script. Call regularly with a monotonic millisecond clock. */
void rnwf_at_tick(struct rnwf_at *at, uint32_t now_ms);

/**
 * What the client is currently trying to do, in words.
 *
 * The point of diagnostics for a configuration mistake: a wrong passphrase fails at "associate and get
 * an IP", a wrong broker hostname at "resolve, connect and CONNACK", and a module that is not wired
 * correctly at "module responding". Those are three different problems that otherwise look identical.
 */
const char *rnwf_at_step_str(const struct rnwf_at *at);

/**
 * Has the connect script reached the network yet?
 *
 * True while it is still at or before Wi-Fi association, false once an IP has been obtained and the
 * remaining steps are all broker business. Lets a caller tell "the network did not accept us" from
 * "the broker did not answer", which are the two failures users confuse, without hard-coding a step
 * number that a new step would invalidate.
 */
bool rnwf_at_before_network(const struct rnwf_at *at);

/** Publish. Returns 0 on success, -1 if not ready or the command would not fit. */
int rnwf_at_publish(struct rnwf_at *at, const char *topic, const char *payload, bool retain);

static inline bool rnwf_at_is_linked(const struct rnwf_at *at)
{
	return at->state == RNWF_AT_ST_READY;
}

#endif /* RNWF_AT_H */
