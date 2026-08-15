# USB-serial (MCP2221A) and a serial bootloader — findings

Two related hardware ideas, investigated 2026-08-14, **neither committed yet**. This file exists so
the analysis does not have to be redone: every number below was read from a datasheet, the Zephyr
source tree, or measured on a PL10 Curiosity Nano, on that date.

The two ideas have very different verdicts, so they are kept separate here.

- **USB-serial via MCP2221A** — viable, cheap, and it closes a real hole. Awaiting a scope decision.
- **UF2 bootloader** — impossible on this silicon. The *serial* half of the same Adafruit bootloader
  is viable; see below for what it actually needs.

---

## 1. MCP2221A as a USB-serial bridge

### Why it is cheaper than it sounds

**ADR-0009 already puts a USB-C connector on the board**, for power: `USB-C 5V → MCP1826 → 3.3V`.
The D+/D− pins are unconnected. So this is not "add USB", it is "connect two pins already present".

### The part

`MCP2221A`, USB 2.0 to I²C/UART/SMBus with GPIO. From the datasheet (DS20005565E) and
microchipDIRECT on 2026-08-14:

| | |
|---|---|
| Packages | SOIC-14, TSSOP-14, VQFN-16, PDIP-14 |
| **Recommended** | **`MCP2221A-I/ST`** (TSSOP-14) — **9 888 in stock**, 12-week lead, Active |
| Also in stock | `-I/P` PDIP 2 640 (breadboard bring-up), `-I/ML` VQFN 91, `-I/SL` SOIC 57 |
| Operating voltage | 3.0–5.5 V |
| **Supply current** | **10 mA typ / 12 mA max at 3.0 V**; 46 µA standby |
| Baud | 300 – 460 800 (the non-A `MCP2221` caps at 115 200 — use the A) |
| Buffers | 448 B total: 64 B transmit, 384 B receive |
| External parts | **None for USB** — "integrating the USB termination resistors and the oscillator needed for USB operation". No crystal. |
| Qualification | AEC-Q100, >4 kV HBM ESD |
| Host drivers | CDC class driver on Windows/macOS/Linux — nothing to install |

### Design notes that are easy to get wrong

- **`VUSB` must also go to 3.3 V.** §1.6.2.1: the internal LDO feeds the USB transceiver from `VDD`,
  and when `VDD` is already 3.3 V "the internal USB transceiver LDO cannot provide the required 3.3V
  power. It is necessary to also connect the `VUSB` pin ... to the 3.3V power supply rail." Getting
  this wrong yields marginal USB signalling rather than an obvious failure.
- **10 mA is always-on.** Irrelevant to the 1 A MCP1826, but it roughly doubles the board's quiescent
  draw and matters if a battery variant is ever considered.
- **It consumes PL10's last SERCOM.** The part has exactly two (`SERCOM0`, `SERCOM1`, verified in
  `pic32cm6408pl10048.h`); SERCOM0 is the RNWF02 (D76). After this the product has no I²C, ever.
- **No DTR/RTS.** The pinout exposes `URx`/`UTx` only, so the MCU **cannot** tell when the host opens
  the port. Liveness must stay application-level, exactly like `wigwag/host_online` (D75).

### What it buys beyond the transport

- **A console on the product PCB, which today does not exist.** On a real board the only way to see
  `RESET BY WATCHDOG` (ADR-0016) is SWD. Rule 4 asks the device not to lie; this is the first cheap
  way to let it *explain itself*.
- **Hardware evidence of a live host.** `GP2` can be `USBCFG` — "indicates when the enumeration is
  completed" — and `GP0` can be `SSPND`, asserted when the host suspends. Two more independent inputs
  for `link.c`, better grounded than anything in the MQTT path. Enumeration is not the same as "the
  daemon is running", so it supplements rather than replaces a heartbeat.
- **A wired variant could drop the RNWF02 entirely** — no Wi-Fi credentials, no broker, no MQTT. For
  a light sitting next to the machine running Claude Code, that may be the majority case.

### Open decisions

1. **Scope** — footprint + pins committed in Phase 3 with firmware later, versus designing the wired
   transport now, versus recording only.
2. **Transport role** — peer to MQTT with USB taking precedence, a separate USB-only variant, or
   diagnostics/console only. This one materially changes `link.c` and `CONTEXT.md`.

Not yet decided; both were being discussed when the bootloader question took priority.

---

## 2. Bootloader

### UF2 is impossible here, and the MCP2221A cannot rescue it

UF2 is a USB mass-storage protocol: the MCU must *be* a USB device presenting a fake FAT volume.
**PL10 has no USB peripheral at all** — the pack's component directory contains `sercom.h`, `tc.h`,
`tcc.h`, `adc.h`, `eic.h`, `nvmctrl.h` and so on, and **no `usb.h`**.

The MCP2221A does not help: it is a USB device in its own right with a fixed CDC+HID personality. It
cannot expose the MCU's flash as a drive.

### The serial half of the same bootloader *is* viable

Adafruit's `uf2-samdx1` is a composite device — UF2 mass storage **and** a CDC port speaking the
BOSSA/SAM-BA protocol, in 8 KB on SAMD21, entered by double-tap reset. Split across our constraints:

| Piece | On PL10 |
|---|---|
| UF2 / MSC / FAT12 emulation | ✗ impossible — no USB peripheral. This is also most of the 8 KB. |
| BOSSA/SAM-BA over serial | ✓ viable — and the MCP2221A supplies the CDC port the MCU cannot |
| Host tooling | ✓ **already in Zephyr**: `bossac` is an in-tree runner (`arduino/due`, `arduino/mkrzero`, `others/serpente`, `peregrine/sam4l_wm400_cape`), with `is_extended_samba_protocol()` for the SAMD variant and the image offset derived from `zephyr,code-partition`. `west flash -r bossac` would work with no custom tooling. |
| Entry mechanism | ✓ nearly free — we have a debounced button with a long-press path reserved (D58), `RCAUSE` to distinguish reset causes, and 8 KB of SRAM in which a magic word survives a warm reset. SAMD21 uses double-tap only because it has nothing else. |
| Vector relocation | ✓ **`__VTOR_PRESENT = 1`** in `pic32cm6408pl10048.h`. This was the biggest silent risk: ARMv6-M makes VTOR optional, and without it a relocated application is a non-starter. |

**Decided direction (2026-08-14):** a bootloader is a **developer/maker convenience**, not a product
feature — no signing, no rollback, no anti-brick guarantees beyond `BOOTPROT`. It would be
**bare-metal, derived from the Adafruit bootloader, not a Zephyr application**, so it needs no Zephyr
flash driver — only the raw NVMCTRL sequence. Not scheduled.

### Flash geometry and layout

Measured and read on 2026-08-14:

```
FLASH_SIZE 64 KB   FLASH_PAGE_SIZE 512   FLASH_NB_OF_PAGES 128   (pack header)
PARAM = 0x07060080 -> NVMP 128 pages, PSZ 6 -> 8<<6 = 512 B      (read from silicon)
LOCK  = 0x0000ffff -> 16 regions, all unlocked
Flash is mapped at 0x0C000000, not 0x00000000
```

The board devicetree already declares:

```
0x0000  slot0_partition   "image-0"   60 KB   <- zephyr,code-partition; app is 24 080 B
0xf000  storage_partition "storage"    4 KB   <- unused until ADR-0017
```

A bootloader layout composes cleanly on top, and `bossac` reads the offset from
`zephyr,code-partition` itself:

```
0x0000  bootloader       ~4 KB    BOOTPROT + WLOCKREGION protected
0x1000  slot0 (app)      ~56 KB   Zephyr built with a matching code-partition offset
0xf000  storage            4 KB   untouched
```

`BOOTPROT` and the `LR`/`UR`/`WLOCKREGION` commands exist to write-protect that bottom region, so the
bootloader can be made unable to erase itself.

### What it would cost, with real numbers

From ADR-0017's measurements at 24 MHz: **page erase 10.1 ms, word write ~0.13 ms**, and erase
duration is independent of page count (§26.4.2.3.3), so a `FLMPER32` erases 16 KB in ~10 ms. Writing
a 24 KB image is therefore roughly 6 000 words ≈ 0.8 s plus erase — serial transfer at 460 800 baud
will dominate.

### Prerequisite knowledge, now in hand

The NVMCTRL sequence is subtle in one way that cost a debugging round and is worth carrying into the
bare-metal port: **issuing an *enable* command (`FLWR`, `FLPER`) is itself a command that clears
`INTFLAG.READY`.** Storing to the array before it completes sets `STATUS.PROGE` and the operation
silently does not happen. Measured: `INTFLAG` reads `0x00000000` immediately after writing `FLPER`.
See `firmware/modules/pic32cm-pl-nvmctrl/drivers/flash_pic32cm_pl.c` — `issue_cmd()` — for the full
rule and the other keys (`CMDEX` `0xA5`, `WPCTRL.WPKEY` `0x4E564D`).

---

## Rejected along the way

| Idea | Why not |
|---|---|
| **MCUboot with serial recovery instead of SAM-BA** | Zephyr-native, signed images, `mcumgr` host tooling. Rejected for this part: two slots do not fit in 64 KB, and serial recovery's buffers are hard to justify in 8 KB. Would point at the larger part (D20) — which is a decision to make on its own merits, not as a side effect of wanting a bootloader. |
| **Bit-bang SWD from the MCP2221A's GPIO** | Its 4 GP pins are host-controllable over HID, so in principle the bridge could program the MCU directly. Rejected: pyOCD has no such probe backend, it would be slow, and it makes programming depend on an unsupported hack. SWD pads cost nothing. |
| **A larger MCU with native USB, for real UF2** | Solves the problem properly and would allow drag-and-drop firmware. Rejected as out of scope for now: it reopens ADR-0008 (smallest part that runs Zephyr) and ADR-0001, and the pin/PCB work is already committed around PL10. Note it as the escape hatch if UF2 ever becomes a requirement rather than a nice-to-have. |
| **The boot ROM as a bootloader** | PL10 *has* a boot ROM with an Interactive Mode, but §6.4.6 makes clear it is debugger-facing: IMODE is entered by SWD cold-plug, commands come over the DAP. There is no SAM-BA-style UART monitor in ROM, so there is no free lunch. |
