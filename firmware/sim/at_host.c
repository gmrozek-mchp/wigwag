/*
 * Host runner for the AT client: the POSIX half of ADR-0015.
 *
 * Runs the *real* rnwf_at.c against fake_rnwf02.py over a PTY, so the whole connect state machine
 * can be exercised against a real MQTT broker with no hardware and no Zephyr. This is what
 * native_sim would have provided on Linux.
 *
 *   python3 fake_rnwf02.py --pty --broker localhost     # prints a PTY path
 *   ./build/at_host /dev/ttys0xx
 *
 * The only thing here that is not shared with the firmware is the transport and the clock, which
 * is exactly the seam the core was designed around.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../src/rnwf_at.h"
#include "../src/rnwf_at_cmds.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int uart_fd = -1;

static int host_write(void *user, const uint8_t *data, size_t len)
{
	(void)user;
	return (int)write(uart_fd, data, len);
}

static uint32_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U);
}

static void on_message(void *user, const char *topic, const char *payload)
{
	(void)user;
	printf("[app] message  %s = %s\n", topic, payload);
	fflush(stdout);
}

static void on_link(void *user, bool linked)
{
	(void)user;
	printf("[app] link     %s\n", linked ? "LINKED" : "UNLINKED (fail-visible: red/yellow wigwag)");
	fflush(stdout);
}

static const struct rnwf_at_config cfg = {
	.ssid = "TestAP",
	.passphrase = "secretpass",
	.sec_type = RNWF_SEC_WPA2_PERSONAL,
	.broker_host = "localhost",
	.broker_port = 1883,
	.client_id = "wigwag-sim",
	.username = NULL,
	.password = NULL,
	.keep_alive_s = 60,
	.state_topic = "wigwag/state",
	.online_topic = "wigwag/online",
};

int main(int argc, char **argv)
{
	struct rnwf_at at;
	struct rnwf_at_io io = { .write = host_write, .user = NULL };
	struct rnwf_at_callbacks cb = { .on_message = on_message, .on_link = on_link };
	enum rnwf_at_state last = RNWF_AT_ST_IDLE;
	uint32_t run_for_ms = 0;
	uint32_t started;
	bool announced_online = false;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <pty-or-serial-device> [seconds]\n", argv[0]);
		return 2;
	}

	if (argc > 2) {
		run_for_ms = (uint32_t)(atoi(argv[2]) * 1000);
	}

	uart_fd = open(argv[1], O_RDWR | O_NOCTTY);
	if (uart_fd < 0) {
		fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
		return 1;
	}

	/* Raw mode, or the tty layer will helpfully mangle CR into LF and ruin the framing test. */
	{
		struct termios tio;

		if (tcgetattr(uart_fd, &tio) == 0) {
			cfmakeraw(&tio);
			(void)tcsetattr(uart_fd, TCSANOW, &tio);
		}
	}

	rnwf_at_init(&at, &cfg, &io, &cb);
	started = now_ms();
	rnwf_at_start(&at, started);
	printf("[sim] started, driving the real rnwf_at.c over %s\n", argv[1]);

	for (;;) {
		struct pollfd pfd = { .fd = uart_fd, .events = POLLIN };
		uint8_t buf[256];
		uint32_t t;

		if (poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN) != 0) {
			ssize_t n = read(uart_fd, buf, sizeof(buf));

			if (n > 0) {
				rnwf_at_feed(&at, buf, (size_t)n);
			}
		}

		t = now_ms();
		rnwf_at_tick(&at, t);

		if (at.state != last) {
			static const char *names[] = { "IDLE", "RESETTING", "SCRIPT", "READY",
						       "BACKOFF" };

			printf("[sim] state    %s\n", names[at.state]);
			fflush(stdout);
			last = at.state;
		}

		/* Birth message, once, so the online topic is not only ever set by the Last Will. */
		if (at.state == RNWF_AT_ST_READY && !announced_online) {
			if (rnwf_at_publish(&at, cfg.online_topic, "1", true) == 0) {
				printf("[sim] published %s = 1 (retained)\n", cfg.online_topic);
				announced_online = true;
			}
		}
		if (at.state != RNWF_AT_ST_READY) {
			announced_online = false;
		}

		if (run_for_ms != 0U && (t - started) >= run_for_ms) {
			break;
		}
	}

	printf("[sim] done: messages=%u errors=%u timeouts=%u malformed=%u aecs_ignored=%u\n",
	       at.messages, at.errors, at.timeouts, at.lines_dropped, at.aecs_ignored);
	close(uart_fd);
	return 0;
}
