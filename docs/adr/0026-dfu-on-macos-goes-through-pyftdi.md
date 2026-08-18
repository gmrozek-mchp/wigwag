# ADR-0026 — Module DFU on macOS goes through pyftdi, keeping Microchip's protocol code

- **Status:** Accepted
- **Date:** 2026-08-18

## Context

ADR-0025 requires every module to be updated on the bench, and this project is developed on macOS.
Microchip's DFU utility (`rnwf-wilc-winc-utilities.zip`, `dfu/do_dfu.py`) cannot run here, for two
independent reasons:

- **It only knows two operating systems.** `DFU.__init__` branches on `nt` and otherwise assumes
  Linux: it shells out to `modprobe` and `udevadm` and looks for the port at `/dev/ttyUSB*`. On macOS
  it takes the Linux path and finds nothing. Its own README badges list Windows and Linux only.
- **It needs D2XX.** `requirements.txt` pins `ftd2xx==1.3.2`, which wraps FTDI's proprietary
  `libftd2xx` — not present here, and on macOS it competes with Apple's own FTDI driver for the
  device. The tool needs *both* interfaces in one run: bit-bang for DFU entry, then a VCP serial port
  for the Programming Executive protocol.

The work itself is not exotic. DFU entry is a 66- and 276-sample pattern clocked onto three lines
(`PGC`, `PGD`, `MCLR`) in FTDI async bit-bang at 153 600 samples/s, after which the same two pins
become a 230400 8N1 UART carrying 4-byte PE command frames. `libusb` is already installed, and
**pyftdi** does async bit-bang *and* serial over libusb with no kernel driver at all.

What must not be reimplemented is the part where mistakes are expensive: the patterns, the PE command
frames, the erase and write sequencing, the checksums, and the image header fixups.

## Decision

`firmware/tools/rnwf02_dfu_mac.py` **imports Microchip's `dfu.py` and `do_dfu.py` unmodified from
wherever the utilities zip was unpacked, and replaces only the FTDI transport class with a pyftdi
implementation.** Their `DFU` object is constructed directly and handed our transport plus a pyftdi
serial port, so every PE operation, address-map constant and header fixup runs their code.

Vendor code is never copied into this repository; `firmware/tools/fetch-rnwf02-firmware.sh` fetches
and checksums it. `import ftd2xx` at the top of their `dfu.py` is satisfied with a stub module that
raises if anything ever reaches through it.

The shim also preserves their **safety ordering**: PE version and device ID are verified before any
erase, so a failed DFU entry writes nothing.

**On Windows or Linux, use Microchip's tool directly.** This shim is a macOS workaround, not a
replacement.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| Install `libftd2xx` and run the vendor tool as-is | Does not fix the platform branching — it would still `modprobe` and glob `/dev/ttyUSB*`. And D2XX on macOS has to fight Apple's FTDI driver for a device the same run needs as a VCP port. |
| Patch the vendor script in place (port globs, skip `udevadm`) | Puts vendor-licensed code under our maintenance with our edits inside it, and still requires D2XX. Every utilities release would need re-patching. |
| Reimplement the whole DFU flow ourselves | The valuable, error-prone part is the PE protocol and the signed-image header handling. Rewriting it risks bricking a secured part to avoid writing one transport class. |
| Do updates on a Linux box or VM | Works, and remains the fallback, but adds a machine to a one-command bench step and USB passthrough to a VM is its own source of the exact timeouts seen here. |
| Use the on-board MCP2200 over USB-C | Reaches only UART1 and MCLR. DFU entry needs three bit-banged lines; the MCP2200 cannot bit-bang its UART pins. Fine for AT, useless for DFU. |
| Host-assisted DFU from the PIC32CM PL10 | It has the three pins already wired and could bang the pattern, but the PE write frame is up to 4096 bytes against 8 KB of SRAM (Rule 5), and it would mean streaming a 578 KB image through the console. A far larger project than a transport class. |

## Consequences

**Accepted costs**

- One more bench tool to keep working, and it depends on the vendor keeping `dfu.py`'s class shape.
  If a future utilities release restructures `DFU.__init__` or `FTDI`, the shim needs revisiting —
  which is why it imports rather than forks, so breakage is loud and localised.
- pyftdi is a bench dependency, deliberately kept out of `host/pyproject.toml` (it is not a runtime
  dependency of the daemon or the firmware, so it needs no dependency ADR). It is invoked with
  `uv run --with pyftdi`.
- Two macOS-specific failure modes now documented rather than designed away: a chip left latched in
  bit-bang mode after an aborted run, and libusb writes timing out once the kernel VCP driver has
  touched the same adapter. Both are handled by an automatic USB reset and retry.

**Benefits**

- Module updates happen on the development machine, with no VM, no kernel extension, and no
  proprietary driver.
- The risky protocol code stays Microchip's, including their verify-before-erase ordering.
- The transport being ours made two extra bring-up instruments almost free: `--test-mclr`, which
  proves the reset wire by making the module announce itself, and `--reset-ftdi` for recovery.

**Revisit if**

- Microchip ships a DFU utility that supports macOS, or moves off D2XX — then delete this shim.
- The vendor changes `dfu.py`'s structure enough that importing it is more fragile than a clean
  reimplementation against the documented PE protocol.
- Production volume moves updates to OTA (ADR-0025's own revisit condition), which needs none of this.
