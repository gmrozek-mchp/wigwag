#!/usr/bin/env python3
"""Bring-up probe for a real RNWF02: is it alive, at what baud, and are TX/RX the right way round?

The mirror image of fake_rnwf02.py. That one pretends to be the module so the firmware can be
tested without hardware; this one talks to the *real* module from the host so the module can be
tested without the firmware. Point it at a 3.3 V USB-UART adapter (or the RNWF02 Add-on Board's
own MCP2200, which appears as a CDC port) wired to the module's UART1.

    # the usual question: is anything there, and at what baud?
    python3 probe_rnwf02.py --port /dev/cu.usbserial-XXXX

    # passive listen only -- never transmits. Power-cycle the module and watch for +BOOT.
    python3 probe_rnwf02.py --port /dev/cu.usbserial-XXXX --listen 10

    # eliminate the adapter as a suspect: short the adapter's own TX to its own RX first
    python3 probe_rnwf02.py --port /dev/cu.usbserial-XXXX --loopback

Numbers that matter, from the RNWF02 Wi-Fi Module Data Sheet (DS70005544C, Table 2-1) and the
RNWF02 Add-on Board User's Guide (DS50003575C, 3.3.1):

  * UART1 is the host AT interface and its default is **230400 8N1, no flow control**. Not 115200.
  * Pin 14 UART1_TX is a module *output*; pin 19 UART1_RX is a module *input*. On the add-on board
    these are J205 pin 3 and J205 pin 4, labelled TX and RX from the *module's* point of view --
    which is the opposite sense to a mikroBUS host socket, and the usual reason a pair gets swapped.
  * Strap1 (pin 10) and Strap2 (pin 26) both low select UART1 as the host interface. The add-on
    board fits those pulls already (R235, R238/R239 in DS50003575C Figure 5-5).

WHAT A SILENT RESULT DOES NOT TELL YOU: silence at every baud is consistent with a swapped pair,
a module held in reset, a dead rail and a wrong strap all at once. It narrows nothing on its own,
which is why --loopback and --listen exist: prove the adapter, then prove the module transmits,
and only then believe anything about the pair.

SPDX-License-Identifier: Apache-2.0
"""

from __future__ import annotations

import argparse
import re
import sys
import time

# Datasheet default first, then this project's own SERCOM0 speed, then the two the vendor's tools
# use for the module's separate debug UART -- in case the pair is on UART2_TX by mistake.
DEFAULT_BAUDS = [230400, 115200, 460800, 921600]

# A command line is terminated CR LF, and a bare AT is the cheapest thing that must answer
# (rnwf_at_cmds.h: RNWF_AT_PING).
PING = b"AT\r\n"
VERSION = b"AT+GMR\r\n"

# Generous enough for a module mid-boot to finish and answer, short enough that a four-baud sweep
# stays under ten seconds.
REPLY_TIMEOUT = 1.5


class PortLost(Exception):
    """The port stopped answering mid-probe. Says nothing at all about the module."""


def open_port(port: str, baud: int):
    # An ftdi:// URL goes through pyftdi rather than the kernel VCP. Worth preferring on an adapter
    # that is also used for DFU: mixing pyftdi and /dev/cu.* on one FT232R leaves macOS's own FTDI
    # driver attached, after which libusb writes time out until the device is USB-reset.
    if port.startswith("ftdi://"):
        try:
            from pyftdi.serialext import serial_for_url  # type: ignore
        except ImportError:
            sys.exit("probe_rnwf02: an ftdi:// port needs pyftdi.\n"
                     "  uv run --with pyftdi --with pyserial firmware/sim/probe_rnwf02.py ...")
        try:
            return serial_for_url(port, baudrate=baud, timeout=0)
        except Exception as exc:  # noqa: BLE001
            sys.exit(f"probe_rnwf02: cannot open {port} at {baud}: {exc}")

    try:
        import serial  # type: ignore
    except ImportError:
        sys.exit(
            "probe_rnwf02: needs pyserial.\n"
            "  uv run --with pyserial firmware/sim/probe_rnwf02.py ...\n"
            "  or:  host/.venv/bin/python firmware/sim/probe_rnwf02.py ..."
        )

    try:
        return serial.Serial(port, baud, timeout=0, write_timeout=2)
    except Exception as exc:  # noqa: BLE001 -- the message is the whole point
        sys.exit(f"probe_rnwf02: cannot open {port} at {baud}: {exc}")


def drain(ser, seconds: float) -> bytes:
    """Read for a fixed wall-clock window. Never gives up early: an AEC may arrive late."""
    got = bytearray()
    deadline = time.monotonic() + seconds

    while time.monotonic() < deadline:
        # A USB-UART adapter unplugged mid-probe, or a second program holding the same device,
        # surfaces here rather than at open() time. Distinguish it from silence: silence is a
        # finding about the module, this is not a finding at all.
        try:
            chunk = ser.read(512)
        except Exception as exc:  # noqa: BLE001 -- pyserial raises SerialException and OSError both
            raise PortLost(str(exc)) from exc

        if chunk:
            got += chunk
        else:
            time.sleep(0.01)

    return bytes(got)


def show(label: str, data: bytes) -> None:
    if not data:
        print(f"  {label}: (silence)")
        return

    printable = sum(1 for b in data if 0x20 <= b < 0x7F or b in (0x0D, 0x0A))
    text = data.decode("ascii", "replace").replace("\r", "\\r").replace("\n", "\\n")
    if len(text) > 200:
        text = text[:200] + "..."

    print(f"  {label}: {len(data)} bytes, {printable}/{len(data)} plausible ASCII")
    print(f"        {text}")


def firmware_version(data: bytes) -> str | None:
    """Pull X.Y.Z out of a +GMR reply, e.g. +GMR:"3.1.0 1 58a15dc2 [15:01:42 Aug 19 2025]".

    The second field is the anti-rollback security level and the third the AT specification revision
    the firmware was built from -- 58a15dc2 is the one rnwf_at_cmds.h is written against.
    """
    m = re.search(rb'\+GMR:"?\s*(\d+\.\d+\.\d+)', data)
    return m.group(1).decode() if m else None


def looks_like_at(data: bytes) -> bool:
    """OK, ERROR:<n> or any +AEC is proof we are at the right baud and the module is talking."""
    return b"OK" in data or b"ERROR" in data or b"+" in data


def probe_one(port: str, baud: int) -> tuple[bool, bytes]:
    ser = open_port(port, baud)
    try:
        print(f"\n{baud} baud 8N1:")

        # Passive first. A module that has just booted has already said +BOOT and we would rather
        # see it than talk over it.
        quiet = drain(ser, 0.4)
        show("before sending anything", quiet)

        ser.reset_input_buffer()
        ser.write(PING)
        ser.flush()
        reply = drain(ser, REPLY_TIMEOUT)
        show("after AT", reply)

        if not looks_like_at(reply):
            return False, quiet + reply

        ser.reset_input_buffer()
        ser.write(VERSION)
        ser.flush()
        version = drain(ser, REPLY_TIMEOUT)
        show("after AT+GMR", version)

        return True, quiet + reply + version
    finally:
        ser.close()


def verdict(port: str, results: dict[int, bytes], alive: int | None) -> int:
    print("\n" + "=" * 72)

    if alive is not None:
        print(f"ALIVE: the module answered AT at {alive} baud.")
        print("TX and RX are the right way round, the rail is up and the straps select UART1.")
        if alive != 230400:
            print(
                f"\nNote: {alive} is not the datasheet default (230400). Either someone has"
                "\nreconfigured the module, or this is a different part than assumed."
            )
        else:
            print("\nThis matches sercom0's current-speed in the board overlay.")
        return 0

    noise = {b: d for b, d in results.items() if d}
    if noise:
        print("BYTES BUT NO AT RESPONSE at:", ", ".join(str(b) for b in sorted(noise)))
        print(
            "Something is transmitting, so the module's TX does reach this adapter's RX -- the"
            "\npair is not swapped. Most likely a baud that is not in the sweep, or the pair is on"
            "\nUART2_TX (the module's debug log, output only) rather than UART1_TX."
        )
        return 1

    print(f"SILENCE at every baud on {port}. In the order worth checking:")
    print(
        "\n  1. Is the rail up?  On the add-on board the red D204 lights whenever VDD is present."
        "\n     If it is dark, nothing below matters: fix power first (JP200 on J201-2/J201-3 to"
        "\n     take 3.3 V from the host, J201-1/J201-2 to take it from USB-C)."
        "\n  2. Is the adapter itself good?  Short its TX to its own RX and run --loopback."
        "\n     A loopback that fails means the problem was never the module."
        "\n  3. Does the module transmit at all?  Run --listen 10 and power-cycle the module."
        "\n     A +BOOT banner proves module TX -> adapter RX. No banner and the pair is either"
        "\n     swapped or the module is held in reset (MCLR low)."
        "\n  4. Only then swap the two data wires and repeat."
    )
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", required=True, help="serial device, e.g. /dev/cu.usbserial-XXXX")
    ap.add_argument(
        "--bauds",
        type=int,
        nargs="+",
        default=DEFAULT_BAUDS,
        help=f"bauds to sweep (default: {' '.join(str(b) for b in DEFAULT_BAUDS)})",
    )
    ap.add_argument(
        "--listen",
        type=float,
        metavar="SECONDS",
        help="passive listen only, transmitting nothing. Power-cycle the module while it runs.",
    )
    ap.add_argument(
        "--loopback",
        action="store_true",
        help="adapter self-test: with the adapter's TX shorted to its own RX, prove the port works",
    )
    ap.add_argument("--baud", type=int, default=230400, help="baud for --listen/--loopback")
    ap.add_argument(
        "--require-version",
        metavar="X.Y.Z",
        help="fail with status 3 unless the module reports this firmware version (pre-ship gate)",
    )
    args = ap.parse_args()

    if args.loopback:
        ser = open_port(args.port, args.baud)
        try:
            probe = b"wigwag loopback\r\n"
            ser.reset_input_buffer()
            ser.write(probe)
            ser.flush()
            echo = drain(ser, 0.5)
            show("echo", echo)
            if probe.strip() in echo:
                print(f"\nLOOPBACK OK at {args.baud}: the adapter, the port and this baud are fine.")
                return 0
            print(
                f"\nLOOPBACK FAILED at {args.baud}. Either TX is not shorted to RX, or the adapter"
                "\nor the driver is the problem -- not the module."
            )
            return 1
        finally:
            ser.close()

    if args.listen:
        ser = open_port(args.port, args.baud)
        try:
            print(f"listening at {args.baud} for {args.listen:g}s, transmitting nothing.")
            print("Power-cycle or reset the module now.")
            heard = drain(ser, args.listen)
            show("heard", heard)
            if b"+BOOT" in heard:
                print("\n+BOOT seen: this wire really is the module's UART1_TX. Pair is correct.")
                return 0
            if heard:
                print("\nBytes, but no +BOOT. Right wire, probably wrong baud.")
                return 1
            print("\nNothing. Either this is not the module's TX, or it never booted.")
            return 1
        finally:
            ser.close()

    print(f"probing {args.port}: {len(args.bauds)} bauds, 8N1, no flow control")
    results: dict[int, bytes] = {}
    alive: int | None = None
    version: str | None = None

    for baud in args.bauds:
        ok, data = probe_one(args.port, baud)
        results[baud] = data
        if ok:
            alive = baud
            version = firmware_version(data)
            break

    rc = verdict(args.port, results, alive)

    # Pre-ship gate: a module is only fit to ship on the firmware we have qualified (ADR-0025).
    if args.require_version:
        print()
        if version is None:
            print(f"VERSION CHECK FAILED: no +GMR version read, wanted {args.require_version}.")
            return 3
        if version != args.require_version:
            print(f"VERSION CHECK FAILED: module runs {version}, this build ships"
                  f" {args.require_version}.")
            print("  Update it: docs/module-firmware-dfu.md")
            return 3
        print(f"VERSION OK: {version}")

    return rc


if __name__ == "__main__":
    try:
        sys.exit(main())
    except PortLost as exc:
        print(f"\nPORT LOST mid-probe: {exc}")
        print(
            "\nThis is not a verdict on the module. Either the adapter was unplugged, or another"
            "\nprogram holds the device -- miniterm, screen, or a running wigwagd with the serial"
            "\ntransport enabled. Close it and probe again."
        )
        sys.exit(2)
