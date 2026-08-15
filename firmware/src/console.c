/*
 * The console adapter. See console.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "console.h"

#include "cmd.h"
#include "lamp_pwm.h"
#include "lineedit.h"
#include "settings_store.h"
#include "transport.h"

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#include <string.h>

BUILD_ASSERT(LINEEDIT_BUF_SZ >= CMD_LINE_MAX,
	     "the line buffer must hold the longest command cmd.c accepts");

#define CONSOLE_NODE DT_CHOSEN(zephyr_console)

/*
 * One line's worth plus slack. Smaller than the module UART's 256-byte ring (D77) because nothing
 * here arrives unsolicited: bytes come only when a person types or the host writes a command, and a
 * command is at most CMD_LINE_MAX.
 */
#define RX_RING_SZ 128

static const struct device *uart_dev;
static struct wigwag_settings *cfg;
static struct transport *tport;
static struct lineedit editor;

static struct {
	uint8_t buf[RX_RING_SZ];
	volatile uint16_t head;	/* written by the ISR */
	volatile uint16_t tail;	/* read by the poll loop */
	volatile uint32_t overruns;
} rx;

static void uart_isr(const struct device *dev, void *user)
{
	ARG_UNUSED(user);

	uart_irq_update(dev);

	while (uart_irq_rx_ready(dev) > 0) {
		uint8_t byte;
		uint16_t next;

		if (uart_fifo_read(dev, &byte, 1) != 1) {
			break;
		}

		next = (uint16_t)((rx.head + 1U) % RX_RING_SZ);

		if (next == rx.tail) {
			/*
			 * Full. Drop the byte and count it: the line editor will then see a line that
			 * is missing a character somewhere in its middle, which is exactly the silent
			 * corruption LINEEDIT_TOO_LONG exists to avoid — so it is reported.
			 */
			rx.overruns++;
			continue;
		}

		rx.buf[rx.head] = byte;
		rx.head = next;
	}
}

/* Echo sink for lineedit. Straight to the UART rather than printk: one byte at a time, no formatting. */
static void echo_out(void *user, const char *data, size_t len)
{
	size_t i;

	ARG_UNUSED(user);

	for (i = 0; i < len; i++) {
		uart_poll_out(uart_dev, (unsigned char)data[i]);
	}
}

static void print_help(void)
{
	printk("commands:\n"
	       "  show                     settings, secrets masked\n"
	       "  set <key> <value>        transport ssid pass sec broker port client user mqttpass\n"
	       "  set transport usb|wifi   which side owns the lamps; reboot to apply\n"
	       "  save                     persist; Wi-Fi changes apply on reboot\n"
	       "  clear                    forget stored settings\n"
	       "  reboot\n"
	       "  state IDLE|BUSY|WAIT|ERROR\n"
	       "  brightness <0-255>       applies now\n"
	       "  gain green|yellow|red <0-255>   per-lamp calibration, applies now\n"
	       "  echo on|off              off for a host program driving this port\n"
	       "  host on|off              host liveness; repeat within 10 s to stay trusted\n");
}

static void print_one(enum cmd_key key)
{
	const char *v = settings_get_str(cfg, key);

	if (cmd_key_is_secret(key)) {
		/*
		 * Never the value. Whether it is *set* is the useful fact when diagnosing a failed
		 * association, and printing the passphrase would hand it to anyone with a cable — the
		 * conventions say secrets are never echoed into logs, and a console is a log.
		 */
		printk("  %-9s %s\n", cmd_key_str(key), (v != NULL && v[0] != '\0') ? "<set>" : "<unset>");
		return;
	}

	if (v != NULL) {
		/* "<unset>" rather than an empty column: a blank value reads as a broken `show`. */
		printk("  %-9s %s\n", cmd_key_str(key), (v[0] != '\0') ? v : "<unset>");
	}
}

static void print_show(void)
{
	printk("settings:\n");
	/* First, because it decides what this device is (ADR-0022). Everything else is detail. */
	print_one(CMD_KEY_TRANSPORT);
	print_one(CMD_KEY_SSID);
	print_one(CMD_KEY_PASS);
	printk("  %-9s %u\n", cmd_key_str(CMD_KEY_SEC), cfg->sec);
	print_one(CMD_KEY_BROKER);
	printk("  %-9s %u\n", cmd_key_str(CMD_KEY_PORT), cfg->port);
	print_one(CMD_KEY_CLIENT);
	print_one(CMD_KEY_USER);
	print_one(CMD_KEY_MQTTPASS);
	printk("  brightness %u\n", cfg->brightness);
	printk("  gain      %u/%u/%u (green/yellow/red)\n", cfg->gain[LAMP_GREEN],
	       cfg->gain[LAMP_YELLOW], cfg->gain[LAMP_RED]);
}

static void exec(struct cmd *c, bool ok)
{
	if (!ok) {
		/* Say which part was rejected. "error" alone on a three-lamp device is useless. */
		switch (c->kind) {
		case CMD_SET:
			/*
			 * Distinguish the two failures. `set port abc` is a bad *value* for a key that
			 * exists, and saying "unknown key" sends the reader looking for a typo in the
			 * wrong half of the line.
			 */
			if (c->key == CMD_KEY_NONE) {
				printk("bad set: unknown key; try help\n");
			} else {
				printk("bad set: %s value not accepted\n", cmd_key_str(c->key));
			}
			break;
		case CMD_STATE:
			printk("bad state: expected IDLE, BUSY, WAIT or ERROR\n");
			break;
		case CMD_BRIGHTNESS:
			printk("bad brightness: expected 0-255\n");
			break;
		case CMD_GAIN:
			printk("bad gain: expected green|yellow|red and 0-255\n");
			break;
		default:
			printk("unknown command; try help\n");
			break;
		}
		return;
	}

	switch (c->kind) {
	case CMD_NONE:
		break;

	case CMD_HELP:
		print_help();
		break;

	case CMD_SHOW:
		print_show();
		break;

	case CMD_SET:
		if (!settings_apply(cfg, c->key, c->value, c->num)) {
			printk("bad set: %s value does not fit\n", cmd_key_str(c->key));
			break;
		}
		/* Staged, not stored — `save` is a separate act so a half-typed network is recoverable. */
		printk("set %s (not saved)\n", cmd_key_str(c->key));

		/*
		 * The one configuration mistake this design makes easy: setting up a network on a device
		 * whose transport is still the wire (the default, D120), then wondering why the lamps
		 * ignore it. Said here, at the moment it happens, rather than left for the user to deduce
		 * from `show`.
		 */
		if (c->key == CMD_KEY_SSID && cfg->transport == WIGWAG_TRANSPORT_USB) {
			printk("note: transport is usb, so this network will not be used;\n"
			       "      `set transport wifi` and save to switch\n");
		}
		break;

	case CMD_SAVE: {
		int ret = settings_save(cfg);

		if (ret == 0) {
			printk("saved; reboot to apply Wi-Fi changes\n");
		} else {
			printk("save failed (%d)\n", ret);
		}
		break;
	}

	case CMD_CLEAR: {
		int ret = settings_clear(cfg);

		printk("%s; reboot to apply\n", (ret == 0) ? "cleared" : "clear failed");
		break;
	}

	case CMD_REBOOT:
		printk("rebooting\n");
		sys_reboot(SYS_REBOOT_COLD);
		break;

	case CMD_STATE:
		/*
		 * Applied straight to the lamps. Note this does *not* yet mark the link trusted, so on a
		 * bench with no module the fail-visible pattern still wins and you will not see the state
		 * you just set — correct per ADR-0007, and the piece that changes when D104's transport
		 * selection treats console traffic as the host heartbeat.
		 */
		lamp_pwm_set_state(c->state);
		printk("state %s\n", wigwag_state_str(c->state));
		break;

	case CMD_BRIGHTNESS:
		cfg->brightness = (uint8_t)c->num;
		lamp_pwm_set_brightness(cfg->brightness);
		printk("brightness %u (not saved)\n", cfg->brightness);
		break;

	case CMD_GAIN:
		cfg->gain[c->lamp] = (uint8_t)c->num;
		lamp_pwm_set_gain(c->lamp, cfg->gain[c->lamp]);
		printk("gain %u (not saved)\n", cfg->gain[c->lamp]);
		break;

	case CMD_ECHO:
		lineedit_set_echo(&editor, c->num != 0U);
		printk("echo %s\n", (c->num != 0U) ? "on" : "off");
		break;

	case CMD_HOST:
		if (c->num == 0U && tport != NULL) {
			/*
			 * An orderly goodbye. Saying goodbye is itself the host speaking, so console_poll()
			 * has already recorded a heartbeat and cancelled any previous goodbye; applying this
			 * afterwards is what makes the specific meaning win over the generic one.
			 */
			transport_note_host_bye(tport, (uint32_t)k_uptime_get());
		}
		printk("host %s\n", (c->num != 0U) ? "on" : "off");
		return;

	case CMD_UNKNOWN:
	default:
		printk("unknown command; try help\n");
		break;
	}
}

int console_init(struct wigwag_settings *s, struct transport *t)
{
	static const struct lineedit_io io = { .out = echo_out };

	cfg = s;
	tport = t;
	uart_dev = DEVICE_DT_GET(CONSOLE_NODE);

	if (!device_is_ready(uart_dev)) {
		printk("wigwag: no console uart, commands unavailable\n");
		return -ENODEV;
	}

	lineedit_init(&editor, &io);

	uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL);
	uart_irq_rx_enable(uart_dev);

	printk("wigwag: console ready, try help\n");

	return 0;
}

void console_poll(void)
{
	static uint32_t reported_overruns;

	if (uart_dev == NULL) {
		return;
	}

	while (rx.tail != rx.head) {
		char c = (char)rx.buf[rx.tail];
		enum lineedit_event ev;

		rx.tail = (uint16_t)((rx.tail + 1U) % RX_RING_SZ);

		ev = lineedit_feed(&editor, c);

		if (ev == LINEEDIT_TOO_LONG) {
			printk("line too long, ignored\n");
			continue;
		}

		if (ev == LINEEDIT_LINE) {
			struct cmd parsed;
			char *line = lineedit_line(&editor);
			bool ok = cmd_parse(line, &parsed);

			/*
			 * Only `host` and `state` count — see cmd_is_host_activity(). Every recognised
			 * command used to, which meant a person configuring Wi-Fi over this very wire
			 * claimed the transport and then lost it ten seconds later, flickering amber on a
			 * device that was working fine. A rejected line never counts either: noise on a
			 * wire is not a host.
			 */
			if (ok && cmd_is_host_activity(&parsed) && tport != NULL) {
				transport_note_host(tport, (uint32_t)k_uptime_get());
			}

			exec(&parsed, ok);
		}
	}

	if (rx.overruns != reported_overruns) {
		/*
		 * Only on change, and only as a count. A dropped byte means whatever was typed is not
		 * what arrived, so it must be visible rather than left to look like a typo.
		 */
		reported_overruns = rx.overruns;
		printk("console: %u bytes dropped\n", reported_overruns);
	}
}
