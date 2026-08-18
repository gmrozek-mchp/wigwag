# Updating RNWF02 module firmware (DFU)

Every wigwag ships with the latest released RNWF02 firmware — **ADR-0025** — so every module passes
through this procedure once, before it is soldered down or shipped. It is also the only way to get a
module onto the firmware `firmware/src/rnwf_at_cmds.h` is actually written against.

Two modules were updated this way on **2026-08-18**, both from the shipped **2.0.0** to **3.1.0**.

> **Read the traps section before you start.** Nothing here is difficult, but four separate failure
> modes present as "the module is dead" and cost most of a session the first time.

---

## 1. What the update buys

| | shipped 2.0.0 | 3.1.0 |
|---|---|---|
| AT spec revision | `e41f977cb` (Apr 2024) | **`58a15dc2`** — the revision `rnwf_at_cmds.h` cites |
| `AT+CFGCP` (config store) | `ERROR:0.3`, unknown command | exists — needed by D62 and ADR-0012 |
| KRACK (CVE-2017-13079/13081) | vulnerable | fixed |
| TLS renegotiation (CVE-2009-3555) | vulnerable | fixed |
| WolfSSL | older | 5.7.4 |
| Anti-rollback security level | 0 | **1** |

The version string carries all three facts, so one command tells you where a module stands:

```
+GMR:"3.1.0 1 58a15dc2 [15:01:42 Aug 19 2025]"
        ^     ^ ^
        |     | AT specification revision the firmware was built from
        |     anti-rollback security level
        firmware version
```

## 2. Hardware and host requirements

- **An FTDI adapter** — an FT232R is proven. Not a CP2102 or CH340: DFU entry is a 66-sample pattern
  bit-banged on three lines, which those cannot do. The on-board **MCP2200 cannot do it either**; it
  reaches only UART1 and MCLR.
- **`EV72E72A` add-on board**, powered **through the mikroBUS interface** — the App Developer's Guide
  requires host-companion mode for DFU. `JP200` on **J201-2 + J201-3** (not the factory J201-1 + J201-2,
  which takes power from USB-C). The red **D204** confirms VDD.
- **USB-C unplugged.** The MCP2200 drives UART1 and MCLR; leaving it attached puts two drivers on
  those nets.
- **Any Curiosity Nano jumpers off** PA04/PA05/PB04 for the same reason.
- **macOS**: use our shim, `firmware/tools/rnwf02_dfu_mac.py` (**ADR-0026**). Microchip's own
  `do_dfu.py` is Windows/Linux only and needs the proprietary D2XX driver. On Windows or Linux, use
  Microchip's tool directly — the shim exists only because macOS is unsupported.

## 3. Wiring

On the `EV72E72A`, DFU and AT share one pair. From Table 8-1 of the Application Developer's Guide:

> "RNWF02 Add On Board design has interconnected UART1_Tx to PB1/DFU_Tx and UART1_Rx with PB0_Rx to
> enable both Mission mode and DFU operation over the single UART interface."

So wire to the **mikroBUS UART pins**, which is the only pair reachable without soldering — module
pin 26 is only test point TP205 and pin 10 has no test point at all.

| FTDI (TTL-232R colour) | | Add-on board |
|---|---|---|
| TXD, orange — D0, `PGC` | → | **J205 pin 4** (`RX`, module UART1_RX, pin 19) |
| RXD, yellow — D1, `PGD` | ← | **J205 pin 3** (`TX`, module UART1_TX, pin 14) |
| CTS, brown — D3 | → | **J204 pin 2** (`RST`, module MCLR, pin 4) |
| GND, black | — | J204 pin 8 or J205 pin 8 |

The J205 `TX`/`RX` silkscreen is from the *module's* point of view, so `TX` → the FTDI's **RXD**.
Pin roles come from `dfu.py`'s own map (`PIN_TX = 0x01`, `PIN_RX = 0x02`, `PIN_CTS = 0x08`).

MCLR needs no pull-up of yours: `R231` on the board pulls `RESET_N` to VDD with 10k
(DS50003575C Figure 5-4).

## 4. Fetch the firmware, verified

```sh
firmware/tools/fetch-rnwf02-firmware.sh          # download + checksum + unpack
firmware/tools/fetch-rnwf02-firmware.sh --verify # re-check an existing cache
```

Vendor binaries are deliberately **not committed** — the script carries their URLs and SHA-256
instead, so a future session gets byte-identical inputs or a loud failure. Everything lands in
`firmware/tools/vendor/` (gitignored).

Which image, and why the low slot:

| File | Size | Use |
|---|---|---|
| **`rnwf02_dfu.bootable.bin`** | 578 048 | **what we flash** — single-slot signed image, ~30 s |
| `rnwf02.bootable.bin` | 1 982 464 | full image: low slot + high slot + file system, several minutes |
| `rnwf02_ota.bin` | 578 048 | for the OTA path, not this one |
| `flfs_image.bin` | 16 384 | file system (certificate store) only |

The single-slot image is written to the **low** partition (`0x60000000`), which is where Microchip
ships the factory image. The tool rewrites the image header's `FW_IMG_SRC_ADDR` from `0x600F0000` to
`0x60000000` automatically — the shipped image is built for the high slot.

## 5. Do it

**Verify entry first. This writes nothing** and is safe to repeat:

```sh
uv run --with pyftdi firmware/tools/rnwf02_dfu_mac.py \
    --utils-dir firmware/tools/vendor/utils/rnwf-utilities-v2.0.1/dfu \
    --url "ftdi://ftdi:232:XXXXXXXX/1"
```

Expect `In DFU mode. PE version 1, device ID 29c70053.` (`ftdi:///?` lists adapters and their
serials.) The device ID's fifth character is a wildcard — the guide writes it `0x29C7x053` and
Microchip's own comparison skips it.

Then write, and verify the result:

```sh
uv run --with pyftdi firmware/tools/rnwf02_dfu_mac.py \
    --utils-dir firmware/tools/vendor/utils/rnwf-utilities-v2.0.1/dfu \
    --url "ftdi://ftdi:232:XXXXXXXX/1" \
    --write firmware/tools/vendor/fw/RN_release_3.1.0/bin/rnwf02_dfu.bootable.bin low --yes

uv run --with pyftdi --with pyserial firmware/sim/probe_rnwf02.py \
    --port "ftdi://ftdi:232:XXXXXXXX/1" --require-version 3.1.0
```

`--require-version` exits **3** on a mismatch, so it works as a pre-ship gate in a script. This is
the check that enforces ADR-0025; run it on every module before it goes into a build.

Expected output, in order: `Previous source address: 600f0000` → `Modified source address: 60000000`,
`Erase success`, 142 `Write status` lines, `Writing finished`, `Sending MCLR reset`, then
`VERSION OK: 3.1.0`.

## 6. Traps

Each of these presents as "the module is dead". None of them is.

**DFU entry is flaky.** It failed twice at the exact settings that then worked, and on the second
module took three attempts. Microchip's own code loops for the same reason. The shim retries five
times; if all five fail, that is a real signal — check wiring and power.

**A chip left in bit-bang mode poisons everything after it.** If a run aborts before restoring UART
mode, the FT232R stays latched. It still opens happily as a serial port, and then every byte written
goes out as *pin levels* rather than UART frames, while reads return a flood of pin samples that no
baud rate can explain (a giveaway: far more bytes per second than the baud rate allows). Recover with:

```sh
uv run --with pyftdi firmware/tools/rnwf02_dfu_mac.py --utils-dir ... --url ... --reset-ftdi
```

**Do not mix pyftdi and `/dev/cu.usbserial-*` on the same adapter.** After the kernel VCP driver has
touched the device, macOS keeps its FTDI driver attached and libusb writes start failing with
`FtdiError: UsbError: [Errno 60] Operation timed out` — *every* write, including 276-byte ones, where
the same code worked minutes earlier. The shim now recovers automatically (USB device reset plus a
pyftdi cache flush) but the cleaner habit is to use `ftdi://` URLs throughout, which
`probe_rnwf02.py` accepts.

**A failed or interrupted write is recoverable.** The DFU pattern and Programming Executive live in
boot ROM, not in the flash being erased, so re-enter DFU and write again. The boot ROM also verifies
Microchip's signature and invalidates an image it does not trust rather than half-booting it.

**Anti-rollback is real but not absolute.** 3.1.0 raises the security level to 1 so earlier versions
can be rejected. However, writing only the low slot leaves the factory image intact in the high slot:

```
AT+DI
+DI:15.0,0xFFFFFFC0,0x01030100,0x60000000,2   <- low  slot: 3.1.0, sequence FFFFFFC0
+DI:15.1,0xFFFFFFE0,0x00020000,0x600F0000,3   <- high slot: 2.0.0, sequence FFFFFFE0
```

The boot ROM picks the **lowest** sequence number, which is why 3.1.0 boots. Erasing the low
partition is the documented way back to the factory image.

## 7. Checking for something newer

`3.1.0` is what was latest on 2026-08-18, not "latest" forever. Releases are published on the
[EV72E72A product page](https://www.microchip.com/en-us/development-tool/ev72e72a) and the RNWF02
Firmware software-library page. When a newer one appears:

1. Bump `VERSION` **and** the checksums in `firmware/tools/fetch-rnwf02-firmware.sh`, in one commit.
2. Re-verify the AT vocabulary against the new release's `doc/microchip_rnwf02.pdf`, especially
   `+WSTAC` and `+MQTTC` parameter IDs — 3.1.0 already added `+WSTAC:9`, which we do not use.
3. Update the `--require-version` value used by the pre-ship check, and this document's tables.
4. Journal the change, with the version and the date.

## 8. Sources

| Claim | Where |
|---|---|
| UART1 default 230400 8N1; pin 14 TX out, pin 19 RX in; strap table | RNWF02 Data Sheet **DS70005544C** Table 2-1, §2.2 |
| mikroBUS pinout; JP200 power selection; MCP2200 map; R231 pull-up | Add-on Board User's Guide **DS50003575C** §3.1, §3.3, Fig 5-3/5-4/5-5 |
| DFU pins, pattern, PE protocol, slot and sequence rules, Table 8-1 | RNWF02 Application Developer's Guide §8, §8.1, §8.2 |
| 3.1.0 contents, CVEs, security level bump | `RNWF02 v3.1 Release Notes.pdf` in the release package |
| TX cannot leave PAD0 | SERCOM USART `CTRLA.TXPO`; Microchip **KB-000007252** |
