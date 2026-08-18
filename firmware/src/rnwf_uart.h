/*
 * Zephyr UART transport for the AT client.
 *
 * This is the board-specific half that rnwf_at.c deliberately does not contain: it fills in
 * struct rnwf_at_io and pumps received bytes into rnwf_at_feed(). The host build uses
 * firmware/sim/at_host.c instead, and the core is identical in both (ADR-0015, D66).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RNWF_UART_H
#define RNWF_UART_H

#include "rnwf_at.h"

/**
 * Bind the module UART (chosen wigwag,module-uart) and wire it to the AT client.
 *
 * Returns 0 on success, or -ENODEV if the device is not ready. The AT client is initialised by the
 * caller; this only supplies its transport.
 */
int rnwf_uart_init(struct rnwf_at *at, const struct rnwf_at_config *cfg,
		   const struct rnwf_at_callbacks *cb);

/**
 * Drain everything received since the last call into the AT client.
 *
 * Call from the same context that calls rnwf_at_tick(); receive happens in an ISR, so this is the
 * point where bytes cross into the state machine.
 */
void rnwf_uart_poll(struct rnwf_at *at);

/** Bytes lost to a full receive ring. Non-zero means the poll loop is not keeping up. */
uint32_t rnwf_uart_overruns(void);

/**
 * Bytes the ISR has received since rnwf_uart_init(), counted before parsing.
 *
 * The bring-up discriminator: zero means nothing is arriving on the RX pin at all (wiring, power,
 * or a module that never booted), while non-zero with no usable response means the bytes are
 * arriving garbled (wrong baud, or the wrong UART on the module).
 */
uint32_t rnwf_uart_rx_bytes(void);

#endif /* RNWF_UART_H */
