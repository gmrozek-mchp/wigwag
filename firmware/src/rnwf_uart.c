/*
 * Zephyr UART transport for the AT client.
 *
 * Receive is interrupt-driven into a bounded ring, drained by rnwf_uart_poll() in thread context.
 * Polled receive was considered and rejected: at the module's 230400 a byte arrives every 43 us
 * (87 us when this was written against 115200), and the SERCOM has no deep FIFO, so a loop that
 * also renders lamps would drop bytes mid-line.
 *
 * Transmit is polled. An AT command is at most 273 bytes (rnwf_at_cmds.h), so the worst case blocks
 * its calling thread for about 12 ms at 230400. Tolerable while main() drives both the lamp and the
 * AT client — the visible effect is a stutter in the breathe during a connect burst — and it is the
 * concrete reason lamp rendering gets its own thread in lamp.c rather than sharing this one.
 *
 * No dynamic allocation; one static ring (Rule 5, ADR-0008).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rnwf_uart.h"

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define MODULE_UART_NODE DT_CHOSEN(wigwag_module_uart)

/*
 * Sized to hold one longest line plus slack, so a burst of asynchronous event codes arriving while
 * the poll loop is busy cannot truncate one. Overruns are counted rather than ignored: silently
 * losing bytes would corrupt a line and look like a module fault.
 */
#define RX_RING_SZ 256

static const struct device *uart_dev;

static struct {
	uint8_t buf[RX_RING_SZ];
	volatile uint16_t head;	/* written by the ISR */
	volatile uint16_t tail;	/* read by the poll loop */
	volatile uint32_t overruns;

	/*
	 * Every byte the ISR has ever seen, counted before any parsing can reject it (4 bytes).
	 *
	 * This is the bring-up question the rest of the diagnostics could not answer: "nothing
	 * works" reads identically whether not one edge arrived on the RX pin or the line is alive
	 * and the baud is wrong. Zero here is a wiring or power fault; non-zero with no recognised
	 * response is a framing or speed fault. Counted in the ISR rather than in the parser so
	 * that even a byte lost to a full ring is still counted as having arrived.
	 */
	volatile uint32_t bytes;
} rx;

static void rx_push(uint8_t byte)
{
	uint16_t next = (uint16_t)((rx.head + 1U) % RX_RING_SZ);

	rx.bytes++;

	if (next == rx.tail) {
		rx.overruns++;
		return;
	}

	rx.buf[rx.head] = byte;
	rx.head = next;
}

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	/* Returns void in this Zephyr API; it latches the pending flags for the checks below. */
	uart_irq_update(dev);

	while (uart_irq_rx_ready(dev) > 0) {
		uint8_t byte;

		if (uart_fifo_read(dev, &byte, 1) != 1) {
			break;
		}
		rx_push(byte);
	}
}

static int uart_write(void *user, const uint8_t *data, size_t len)
{
	size_t i;

	ARG_UNUSED(user);

	if (uart_dev == NULL) {
		return -1;
	}

	for (i = 0; i < len; i++) {
		uart_poll_out(uart_dev, data[i]);
	}

	return (int)len;
}

int rnwf_uart_init(struct rnwf_at *at, const struct rnwf_at_config *cfg,
		   const struct rnwf_at_callbacks *cb)
{
	struct rnwf_at_io io = { .write = uart_write, .user = NULL };

	uart_dev = DEVICE_DT_GET(MODULE_UART_NODE);

	if (!device_is_ready(uart_dev)) {
		printk("wigwag: module UART not ready\n");
		uart_dev = NULL;
		return -ENODEV;
	}

	rx.head = 0;
	rx.tail = 0;
	rx.overruns = 0;
	rx.bytes = 0;

	rnwf_at_init(at, cfg, &io, cb);

	uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL);
	uart_irq_rx_enable(uart_dev);

	return 0;
}

void rnwf_uart_poll(struct rnwf_at *at)
{
	/*
	 * Copy out of the ring in chunks rather than byte at a time, so feed() sees runs of bytes
	 * and the head/tail dance happens once per chunk. head is volatile and only advanced by the
	 * ISR, so a snapshot is enough — anything arriving mid-drain is picked up next call.
	 */
	uint16_t head = rx.head;

	while (rx.tail != head) {
		uint8_t chunk[64];
		size_t n = 0;

		while (rx.tail != head && n < sizeof(chunk)) {
			chunk[n++] = rx.buf[rx.tail];
			rx.tail = (uint16_t)((rx.tail + 1U) % RX_RING_SZ);
		}

		rnwf_at_feed(at, chunk, n);
	}
}

uint32_t rnwf_uart_overruns(void)
{
	return rx.overruns;
}

uint32_t rnwf_uart_rx_bytes(void)
{
	return rx.bytes;
}
