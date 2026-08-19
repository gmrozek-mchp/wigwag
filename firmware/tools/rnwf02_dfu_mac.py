#!/usr/bin/env python3
"""Run Microchip's RNWF02 DFU on macOS, by replacing its FTDI transport with pyftdi.

Microchip's own utility (rnwf-wilc-winc-utilities.zip, dfu/do_dfu.py) cannot run here. It branches
on Windows and Linux only: on anything else it takes the Linux path, shells out to `modprobe` and
`udevadm`, and looks for the port at /dev/ttyUSB*. It also needs the proprietary D2XX driver via
`ftd2xx`, whose libftd2xx.dylib fights macOS's own FTDI driver for the device.

This shim keeps **all of Microchip's protocol code** -- the DFU entry patterns, the Programming
Executive command frames, erase, write, the image header fixups -- and swaps only the layer beneath
it: pyftdi over libusb, which needs no kernel driver and can do async bit-bang and then plain UART
on the same device in one process. Their code is imported from wherever you unzipped it and is never
copied into this repo.

    # 1. unzip Microchip's utilities somewhere
    # 2. verify only -- enters DFU mode, reads PE version and device ID, resets. Writes nothing:
    uv run --with pyftdi firmware/tools/rnwf02_dfu_mac.py --utils-dir ~/Downloads/rnwf-utilities-v2.0.1/dfu

    # 3. only once verify passes, and only with --yes:
    uv run --with pyftdi firmware/tools/rnwf02_dfu_mac.py --utils-dir ... \
        --write ~/Downloads/RNWF02_module_release_3.2.0/bin/rnwf02_dfu_high.bin high --yes

WIRING (FTDI 3.3 V cable, standard TTL-232R colours -- the pin roles are from dfu.py's own map,
FTDI async bit-bang D0/D1/D3):

    FTDI TXD  (orange, D0 = PGC) --> module PGC
    FTDI RXD  (yellow, D1 = PGD) <-- module PGD
    FTDI CTS  (brown,  D3)       --> module MCLR   (J204 pin 2, "RST")
    FTDI GND  (black)            --- GND           (J204 or J205 pin 8)

**Which pins are PGC/PGD is not settled by the documentation.** DS70005544C Table 2-1 calls pin 10
DFU_RX/Strap1 and pin 26 DFU_TX/Strap2, while the Application Developer's Guide §8.2.1 says the
opposite ("PB0/DFU_Rx (Pin 26) and PB1/DFU_Tx (Pin 10)"). Meanwhile its own 32-bit reference code
de-initialises SERCOM0 -- the *AT command* UART -- bit-bangs the pattern on those pins and then
re-initialises it, which reads as though the PE protocol runs over UART1 (pins 14 and 19). On the
EV72E72A only one of those is reachable without soldering: UART1 is on J205 pins 3 (TX) and 4 (RX),
whereas pin 26 is only test point TP205 and pin 10 has no test point at all.

So try UART1 first: J205 pin 4 <- FTDI TXD, J205 pin 3 -> FTDI RXD. `--verify` is safe to repeat --
if the module does not enter DFU mode nothing is erased, because Microchip's code checks the PE
version and device ID before any erase, and this shim keeps that order.

Before wiring, disconnect **everything else** from those nets or two outputs will fight: the USB-C
cable (the MCP2200 drives UART1 and MCLR) and the Curiosity Nano's PA04/PA05/PB04 jumpers. Leave
only the FTDI and the 3.3 V supply.

SPDX-License-Identifier: Apache-2.0
"""

from __future__ import annotations

import argparse
import importlib.util
import sys
import time
from pathlib import Path

# Vendor: dfu.py sets 9600 baud in async bit-bang mode, where the FTDI emits one sample per bit
# time * 16. 16 * 9600 = 153600 samples/s, i.e. 6.51 us per step of the pattern.
BITBANG_SAMPLES_PER_S = 16 * 9600


def install_ftd2xx_stub():
    """Satisfy `import ftd2xx` at the top of vendor dfu.py without the D2XX driver being present.

    Nothing in the flow below calls it: the transport is replaced wholesale. The stub only has to
    carry the one attribute their code names in `except` clauses, ftd2xx.ftd2xx.DeviceError, and to
    fail loudly if anything ever does reach through it.
    """
    import types

    if "ftd2xx" in sys.modules:
        return

    class DeviceError(Exception):
        pass

    def unreachable(*_args, **_kwargs):
        raise AssertionError("vendor FTDI transport called; this shim should have replaced it")

    inner = types.ModuleType("ftd2xx.ftd2xx")
    inner.DeviceError = DeviceError

    outer = types.ModuleType("ftd2xx")
    outer.ftd2xx = inner
    outer.DeviceError = DeviceError
    outer.openEx = unreachable
    outer.listDevices = unreachable

    sys.modules["ftd2xx"] = outer
    sys.modules["ftd2xx.ftd2xx"] = inner


def load_vendor(utils_dir: Path):
    """Import Microchip's dfu.py and do_dfu.py from an unzipped utilities directory."""
    dfu_py = utils_dir / "dfu.py"
    do_dfu_py = utils_dir / "do_dfu.py"

    for path in (dfu_py, do_dfu_py):
        if not path.is_file():
            sys.exit(f"rnwf02_dfu_mac: {path} not found. Point --utils-dir at the unzipped\n"
                     f"  rnwf-wilc-winc-utilities .../dfu directory.")

    install_ftd2xx_stub()
    sys.path.insert(0, str(utils_dir))
    mods = {}
    for name, path in (("dfu", dfu_py), ("do_dfu", do_dfu_py)):
        spec = importlib.util.spec_from_file_location(name, path)
        mod = importlib.util.module_from_spec(spec)
        sys.modules[name] = mod
        try:
            spec.loader.exec_module(mod)
        except ImportError as exc:
            # ftd2xx is imported at the top of dfu.py and is exactly what we are replacing.
            sys.exit(f"rnwf02_dfu_mac: {name}.py needs {exc.name}, which this shim exists to avoid."
                     f"\n  Install a stub or use --utils-dir from a release whose dfu.py imports"
                     f" ftd2xx lazily.")
        mods[name] = mod

    return mods["dfu"], mods["do_dfu"]


class PyFtdiTransport:
    """Stands in for Microchip's FTDI class, same method names, pyftdi underneath.

    Only the four operations their DFU flow uses: drive the three pins as GPIO, clock out a pattern,
    hand the pins back to the UART, and close.
    """

    def __init__(self, url: str, pins, debug: bool = False):
        from pyftdi.gpio import GpioAsyncController

        self.url = url
        self.debug = debug
        self.serial_number = url
        self._pin_tx, self._pin_rx, self._pin_cts = pins
        self._mask = self._pin_tx | self._pin_rx | self._pin_cts
        self._gpio = GpioAsyncController()
        self._open = False

    def debug_print(self, msg):
        if self.debug:
            print(msg)

    def set_gpio_mode(self):
        self._gpio.configure(self.url, direction=self._mask, frequency=BITBANG_SAMPLES_PER_S)
        self._open = True
        # pyftdi defaults to 5 s. Bit-bang writes block until the chip has clocked the samples out,
        # and an intermittent USB hiccup here surfaces as FtdiError("Operation timed out") -- which
        # aborts a run mid-pattern and, without the reset in release(), leaves the chip latched.
        self._gpio.ftdi._usb_write_timeout = 20000
        self._gpio.ftdi._usb_read_timeout = 20000
        self.debug_print(f"bit-bang on {self.url}, mask 0x{self._mask:02x}, "
                         f"{BITBANG_SAMPLES_PER_S} samples/s")

    def send_pattern(self, pattern):
        """Clock out one of Microchip's three-line patterns, one byte per bit time."""
        data = bytearray()
        for i in range(len(pattern["mclr"])):
            mask = 0
            if pattern["mclr"][i] == "1":
                mask |= self._pin_cts
            if pattern["pgc"][i] == "1":
                mask |= self._pin_tx
            if pattern["pgd"][i] == "1":
                mask |= self._pin_rx
            data.append(mask)

        self.debug_print(f"pattern: {len(data)} samples")
        self._gpio.write(bytes(data))
        time.sleep(0.5)  # vendor DFU.PE_INIT_DELAY

    def set_uart_mode(self):
        """Leave MCLR high, then release the pins so the same device can be opened as a UART.

        MCLR is only *driven* high here; once the FTDI returns to UART mode its CTS pin is an input
        again and MCLR is held by the add-on board's 10k pull-up (DS50003575C Figure 5-4, R231).

        The bit mode is reset **explicitly**, mirroring the vendor's setBitMode(..., 0), rather than
        left to close(). Learned the hard way: a chip left latched in async bit-bang still opens
        happily as a serial port, and then every byte written goes out as pin levels instead of UART
        frames -- so the module hears nothing and the failure looks like a wiring fault. Reads are
        equally misleading, returning a flood of pin samples that no baud rate can explain.
        """
        from pyftdi.ftdi import Ftdi

        self._gpio.write(bytes([self._pin_cts]))
        time.sleep(0.05)
        self._gpio.ftdi.set_bitmode(0, Ftdi.BitMode.RESET)
        self.close()

    def close(self):
        if self._open:
            self._gpio.close()
            self._open = False

    def release(self):
        """Reset the bit mode and close, swallowing errors. Safe in a finally, on any failure path.

        A chip abandoned in bit-bang mode makes every subsequent attempt fail in ways that look like
        hardware faults, so recovery must not itself depend on the failure being clean.
        """
        from pyftdi.ftdi import Ftdi

        try:
            self._gpio.ftdi.set_bitmode(0, Ftdi.BitMode.RESET)
        except Exception:  # noqa: BLE001 -- best effort, we are already on an error path
            pass
        try:
            self.close()
        except Exception:  # noqa: BLE001
            pass

    def usb_recover(self):
        """Full USB device reset, then clear the bit mode. The cure for wedged writes.

        Symptom this exists for: every bit-bang write failing with
        FtdiError("UsbError: [Errno 60] Operation timed out"), including short ones, where the same
        code worked minutes earlier. Observed after the *kernel* VCP driver had been used on the same
        FT232R (opening /dev/cu.usbserial-* with pyserial) -- macOS keeps its FTDI driver attached and
        libusb cannot then drive the chip properly. A USB reset plus flushing pyftdi's device cache
        re-arbitrates it. Prefer not to mix pyftdi and /dev/cu.* on one adapter in the first place.
        """
        from pyftdi.ftdi import Ftdi
        from pyftdi.usbtools import UsbTools

        try:
            UsbTools.flush_cache()
            ftdi = Ftdi()
            try:
                ftdi.open_from_url(self.url)
                ftdi.reset(usb_reset=True)
                time.sleep(1.0)
                ftdi.set_bitmode(0, Ftdi.BitMode.RESET)
            finally:
                ftdi.close()
            UsbTools.flush_cache()
            time.sleep(0.5)
        except Exception:  # noqa: BLE001 -- best effort recovery
            pass

    def force_uart_mode_quiet(self):
        try:
            self.force_uart_mode(quiet=True)
        except Exception:  # noqa: BLE001
            pass

    def force_uart_mode(self, quiet: bool = False):
        """Recover a chip left in bit-bang mode by an aborted run. Safe to call on a healthy one."""
        from pyftdi.ftdi import Ftdi

        ftdi = Ftdi()
        try:
            ftdi.open_from_url(self.url)
            ftdi.set_bitmode(0, Ftdi.BitMode.RESET)
            if not quiet:
                print(f"{self.url}: bit mode reset, back to UART")
        finally:
            ftdi.close()


def test_mclr(dfu_mod, url: str, debug: bool) -> int:
    """Is the FTDI's CTS line actually reaching MCLR? Ask the module, which is the only witness.

    Drive MCLR low, release it, then listen on UART1 for the +BOOT banner. A module that reboots on
    command proves the third wire; silence means the pattern's MCLR edges are going nowhere, which is
    indistinguishable from every other DFU failure if you only look at the PE response.
    """
    from pyftdi.serialext import serial_for_url

    pins = (dfu_mod.FTDI.PIN_TX, dfu_mod.FTDI.PIN_RX, dfu_mod.FTDI.PIN_CTS)
    tx, rx, cts = pins
    ftdi = PyFtdiTransport(url, pins, debug)

    idle = tx | rx | cts          # everything high: module running, UART lines idle
    in_reset = tx | rx            # MCLR low only

    ftdi.set_gpio_mode()
    print("holding MCLR low ...")
    ftdi._gpio.write(bytes([idle]) * 1536)          # 10 ms settle
    ftdi._gpio.write(bytes([in_reset]) * 15360)     # 100 ms in reset
    ftdi._gpio.write(bytes([idle]) * 1536)          # release
    ftdi.set_uart_mode()

    uart = serial_for_url(url, baudrate=dfu_mod.DFU.UART_BAUD, timeout=0)
    try:
        print("listening 3 s for +BOOT ...")
        end = time.monotonic() + 3.0
        got = bytearray()
        while time.monotonic() < end:
            chunk = uart.read(4096)
            if chunk:
                got += chunk
            else:
                time.sleep(0.02)
    finally:
        uart.close()

    text = bytes(got).decode("ascii", "replace")
    print(f"heard {len(got)} bytes: {text.strip()[:200]!r}")

    if "+BOOT" in text:
        print("\nMCLR IS WIRED: the module rebooted on command. The third wire is good, so a failed\n"
              "DFU entry is about the pattern on PGC/PGD, not about reset.")
        return 0

    print("\nNo +BOOT. Either the FTDI's CTS (brown, D3) does not reach MCLR / J204 pin 2, or this\n"
          "adapter does not break CTS out at all -- many FT232R boards expose only TX/RX/VCC/GND.\n"
          "Without a driven MCLR the DFU pattern cannot reset the module into DFU mode.")
    return 1


def connect(dfu_mod, do_dfu_mod, url: str, debug: bool, attempts: int = 5):
    """Enter DFU mode, retrying: observed flaky, needing two or three goes at the same settings.

    Microchip's own code loops too. Each attempt is harmless -- the pattern only resets the module --
    and nothing is erased until the caller acts on a verified return.
    """
    last = None
    for attempt in range(1, attempts + 1):
        try:
            return connect_once(dfu_mod, do_dfu_mod, url, debug)
        except (DfuEntryError, Exception) as exc:  # noqa: B014 -- explicit: USB errors retry too
            last = exc
            label = exc if isinstance(exc, DfuEntryError) else f"{type(exc).__name__}: {exc}"
            print(f"  attempt {attempt}/{attempts}: {label}")
            recovery = PyFtdiTransport(url, (0x01, 0x02, 0x08))
            recovery.force_uart_mode_quiet()
            recovery.usb_recover()

    sys.exit(f"FAILED after {attempts} attempts: {last}\n"
             "  Nothing was erased. If this persists, unplug and replug the FTDI (a chip left in\n"
             "  bit-bang mode by an aborted run makes every attempt fail), then retry.")


class DfuEntryError(Exception):
    """DFU mode was not reached. Nothing has been erased."""


def connect_once(dfu_mod, do_dfu_mod, url: str, debug: bool):
    """Enter DFU mode and verify it, then return a vendor DFU object wired to our transport.

    Mirrors the order in Microchip's DFU.__init__ -- reset pattern, test pattern, UART, then confirm
    the PE version and device ID -- because that order is what makes a failed entry harmless.
    """
    from pyftdi.serialext import serial_for_url

    pins = (dfu_mod.FTDI.PIN_TX, dfu_mod.FTDI.PIN_RX, dfu_mod.FTDI.PIN_CTS)
    ftdi = PyFtdiTransport(url, pins, debug)

    print(f"Entering DFU mode via {url} ...")
    ftdi.set_gpio_mode()
    ftdi.send_pattern(dfu_mod.DFU.RESET_PATTERN)
    ftdi.send_pattern(dfu_mod.DFU.TEST_PATTERN)
    ftdi.set_uart_mode()

    uart = serial_for_url(url, baudrate=dfu_mod.DFU.UART_BAUD, timeout=dfu_mod.DFU.UART_TIMEOUT)

    # Build the vendor object without running its platform-specific __init__, then let all of its
    # PE methods work unchanged against our uart.
    dfu = object.__new__(dfu_mod.DFU)
    dfu.chip = do_dfu_mod.RIO0
    dfu.debug = debug
    dfu.os = "darwin"
    dfu.user_ftdi = ""
    dfu.uart = uart
    dfu.ftdi = ftdi

    pe_version = dfu.get_pe_version()
    if pe_version != do_dfu_mod.RIO0.PE_VERSION:
        uart.close()
        ftdi.release()
        raise DfuEntryError(f"PE version {pe_version!r}, expected {do_dfu_mod.RIO0.PE_VERSION}")

    # Index 4 is deliberately skipped, as in vendor DFU.__init__: the guide writes this ID as
    # 0x29C7x053, so that digit is a wildcard (a real part answered 29c70053).
    device_id = dfu.get_device_id()
    expected = do_dfu_mod.RIO0.DEVICE_ID
    if device_id[1:4] != expected[1:4] or device_id[5:8] != expected[5:8]:
        uart.close()
        ftdi.release()
        raise DfuEntryError(f"device ID {device_id}, expected {expected}")

    print(f"In DFU mode. PE version {pe_version}, device ID {device_id}.")
    return dfu


def reset_module(dfu_mod, ftdi: PyFtdiTransport):
    """Pulse MCLR to leave DFU mode, the way vendor DFU.mclr_reset() does."""
    print("Sending MCLR reset ...")
    ftdi.set_gpio_mode()
    ftdi.send_pattern(dfu_mod.DFU.RESET_PATTERN)
    ftdi.set_uart_mode()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--utils-dir", required=True, type=Path,
                    help="unzipped rnwf-wilc-winc-utilities .../dfu directory")
    ap.add_argument("--url", default="ftdi:///1",
                    help="pyftdi device URL (default ftdi:///1; use ftdi:///? to list)")
    ap.add_argument("--write", nargs=2, metavar=("BIN", "SECTION"),
                    help="erase and write BIN at SECTION (low, high, file-system) or 0xADDRESS")
    ap.add_argument("--erase", metavar="SECTION", help="erase a whole section")
    ap.add_argument("--yes", action="store_true", help="required for --write and --erase")
    ap.add_argument("--reset-ftdi", action="store_true",
                    help="only take the FTDI out of bit-bang mode and exit (recovery)")
    ap.add_argument("--test-mclr", action="store_true",
                    help="pulse MCLR and listen for +BOOT, to prove the reset wire. Writes nothing.")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if args.reset_ftdi:
        PyFtdiTransport(args.url, (0x01, 0x02, 0x08)).force_uart_mode()
        return 0

    dfu_mod, do_dfu_mod = load_vendor(args.utils_dir.expanduser())
    chip = do_dfu_mod.RIO0

    if args.test_mclr:
        return test_mclr(dfu_mod, args.url, args.verbose)

    destructive = bool(args.write or args.erase)
    if destructive and not args.yes:
        print("Refusing to erase or write without --yes.\n"
              "\nBefore you pass it, know that each release raises the anti-rollback security"
              "\nlevel -- 2.0.0 shipped at 0, 3.1.0 moved it to 1, 3.2.0 to 4 -- so going back to an"
              "\nearlier version afterwards may be permanently blocked."
              "\n\nRun without --write/--erase first: that verifies DFU entry and touches nothing.")
        return 2

    dfu = connect(dfu_mod, do_dfu_mod, args.url, args.verbose)

    try:
        if args.erase:
            if args.erase not in chip.ADDRESS_MAP:
                sys.exit(f"unknown section {args.erase!r}; one of {list(chip.ADDRESS_MAP)}")
            low, high = chip.ADDRESS_MAP[args.erase]
            pages = (high - low) // chip.PAGE_SIZE
            print(f"Erasing {args.erase}: {hex(low)}-{hex(high)}, {pages} pages ...")
            dfu.pe_erase(low, pages)

        if args.write:
            bin_path, section = Path(args.write[0]).expanduser(), args.write[1]

            if section.startswith("0x"):
                address = int(section, 0)
            elif section in chip.ADDRESS_MAP:
                address = chip.ADDRESS_MAP[section][0]
            else:
                sys.exit(f"unknown section {section!r}; one of {list(chip.ADDRESS_MAP)}")

            data = bin_path.read_bytes()

            if address < chip.ADDRESS_MAP["low"][0]:
                sys.exit("write starts below the DFU flash boundary")
            if address + len(data) > chip.ADDRESS_MAP["file-system"][1]:
                sys.exit("write ends above the DFU flash boundary")

            # Vendor behaviour: a single-slot firmware image gets its FW_IMG_SRC_ADDR fixed up for
            # the slot it is being written to.
            if do_dfu_mod.Image.is_firmware_image(chip, address, data):
                data = bytes(do_dfu_mod.Image(data, None, address).byte_stream)

            pages = (len(data) // chip.PAGE_SIZE) + (len(data) % chip.PAGE_SIZE != 0)
            print(f"Erasing {hex(address)}-{hex(address + len(data))} ({pages} pages) ...")
            dfu.pe_erase(address, pages)
            print(f"Writing {bin_path.name} ({len(data)} bytes) at {hex(address)} ...")
            dfu.pe_write(address, data)
            print("Write complete.")
    finally:
        dfu.uart.close()
        reset_module(dfu_mod, dfu.ftdi)
        dfu.ftdi.close()

    print("Done. Re-read the version with:")
    print("  firmware/sim/probe_rnwf02.py --port /dev/cu.usbmodemXXXX --require-version 3.2.0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
