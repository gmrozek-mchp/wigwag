/*
 * The console adapter: the Zephyr and effects half of the command console.
 *
 * Owns the console UART, feeds bytes to lineedit.c, hands complete lines to cmd.c, and applies the
 * result — to the settings store, to the lamps, or to the displayed state. The three layers are
 * separate on purpose (see lineedit.h): this file and cmd.c hold the decisions, lineedit.c is the
 * piece designed to be replaced if a richer CLI ever earns its RAM.
 *
 * Receive is interrupt-driven into a small ring, for the same reason rnwf_uart.c is (D77): at 115200
 * a byte lands every 87 us and this SERCOM has no deep FIFO, so a 10 ms poll loop would keep only the
 * last byte of each interval. Typing survives polling; **pasting a passphrase does not**, and pasting
 * a 63-character passphrase is exactly what someone will do.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#include "settings.h"
#include "transport.h"

/**
 * Take over receive on the console UART and start accepting commands.
 *
 * @p s and @p t are borrowed and must outlive the console: @p s is the live settings the commands
 * edit, and @p t is told whenever a host speaks, which is what lets the wired path be trusted at all
 * (D104). Returns 0, or -ENODEV if the console UART is missing, in which case the device runs exactly
 * as it did before: unconfigurable, but working.
 */
int console_init(struct wigwag_settings *s, struct transport *t);

/** Drain received bytes and execute any complete lines. Call from the service loop. */
void console_poll(void);

/**
 * Start a Wi-Fi connectivity test, implemented by main.c because it owns the AT client.
 *
 * Returns 0 if a test was started, or a negative errno explaining why not. Must return immediately:
 * the test is advanced by the service loop, so the console stays responsive and the watchdog keeps
 * being fed (ADR-0016) instead of a 30-second command blocking everything.
 */
int wifi_test_start(void);

/**
 * Reset the module and wait for +BOOT: the liveness check that needs no settings.
 *
 * Returns 0 once started, -EALREADY if a test is already running, or -ENODEV if the module UART
 * will not bind. Progress and the verdict go to the console as the service loop advances it.
 */
int module_test_start(void);

#endif /* CONSOLE_H */
