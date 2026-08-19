# Updating RNWF02 module firmware

Every wigwag ships with the latest released RNWF02 firmware — **ADR-0025** — so every module passes
through this once, before it is fitted or shipped. It is also the only way to get a module onto the
firmware `firmware/src/rnwf_at_cmds.h` is written against.

**Both modules were updated on 2026-08-18**, from the shipped **2.0.0** to **3.1.0** (by DFU) and then
to **3.2.0** (over the AT UART).

> **Read the traps section before you start.** Neither path is difficult, but several failure modes all
> present as "the module is dead", and they cost most of a session the first time.

---

## Which of the two paths

| | **NVM update** — normal | **DFU** — recovery |
|---|---|---|
| When | the module boots and answers `AT` | it does not |
| Needs | any serial port on the module's UART1, `pyserial` | an **FTDI** adapter, bit-banged pattern on three lines |
| Tool | the SDK's `tools/nvm_update/nvm-update.py` | `firmware/tools/rnwf02_dfu_mac.py` (ADR-0026) |
| Image | `rnwf02_ota.bin` | `rnwf02_dfu_high.bin` |
| Wall clock | ~65 s for 578 KB at 230400 | ~30 s plus DFU entry, which is flaky |

**Use the NVM path unless the module is dead** (D141). It needs no FTDI, no MCLR wire, no bit-bang and
no D2XX driver, and it writes to the *inactive* slot so the running firmware stays intact until the
new image is verified and activated.

## 1. What the update buys

| | shipped 2.0.0 | 3.1.0 | 3.2.0 |
|---|---|---|---|
| AT spec revision | `e41f977cb` (Apr 2024) | `58a15dc2` — what `rnwf_at_cmds.h` cites | **`a1ac4a49`** |
| `AT+CFGCP` (config store) | `ERROR:0.3`, unknown | exists — D62, ADR-0012 | exists |
| KRACK (CVE-2017-13079/13081) | vulnerable | **fixed** | fixed |
| TLS renegotiation (CVE-2009-3555) | vulnerable | **fixed** | fixed |
| WolfSSL | 4.7.0 | 5.7.4 | 5.7.4 |
| RNG reseeding after long uptime | broken | broken | **fixed** |
| Persistent baud rate | no | no | **yes** |
| Anti-rollback security level | 0 | 1 | **4** |

**Every documented CVE was fixed in 3.1.0.** 3.2.0 adds no CVE fixes at all — its security content is
hardening: RNG reseeding after prolonged operation (which matters for a device that runs for weeks),
TLS alert handling, certificate pinning. So 3.2.0 is worth having but was never urgent.

The version string carries three facts at once, so one command says where a module stands:

```
+GMR:"3.2.0 4 a1ac4a49 [13:57:36 Mar 19 2026]"
        ^    ^ ^
        |    | AT specification revision the firmware was built from
        |    anti-rollback security level
        firmware version
```

## 2. The normal path: NVM update over the AT UART

The module must be **running and answering AT**, and the host needs a serial port on its UART1. On the
`EV72E72A` the simplest way is the board's own USB-C, which goes through the MCP2200 to UART1:

1. **Disconnect anything else on UART1** — the Curiosity Nano's PA04/PA05 jumpers, and PB04 if wired.
   Two drivers on the module's RX line is the one way to make this fail confusingly.
2. **`JP200` on J201-1 + J201-2** (USB supply, the factory position).
3. **Plug in USB-C.** A `/dev/cu.usbmodem…` appears — note that each add-on board has its own MCP2200
   serial number, so two boards give two different paths.

Confirm what is there before erasing anything:

```sh
host/.venv/bin/python firmware/sim/probe_rnwf02.py --port /dev/cu.usbmodemXXXX
```

Then update, and verify:

```sh
host/.venv/bin/python <SDK>/tools/nvm_update/nvm-update.py \
    -p /dev/cu.usbmodemXXXX firmware/tools/vendor/fw/RNWF02_module_release_3.2.0/bin/rnwf02_ota.bin

host/.venv/bin/python firmware/sim/probe_rnwf02.py \
    --port /dev/cu.usbmodemXXXX --require-version 3.2.0
```

`--require-version` exits **3** on a mismatch, so it works as a pre-ship gate in a script. That is the
check that enforces ADR-0025; run it on every module before it goes into a build.

Expected: `Erasing 240 sectors`, 4516 chunks of 128 bytes, `Firmware verified`, `Firmware activated`,
`Device reset`, then the new version read back. `VERSION OK: 3.2.0` from our own gate.

## 3. Fetch the firmware, verified

```sh
firmware/tools/fetch-rnwf02-firmware.sh          # download + checksum + unpack
firmware/tools/fetch-rnwf02-firmware.sh --verify # re-check an existing cache
```

Vendor binaries are deliberately **not committed** — the script carries their URLs and SHA-256 instead,
so a future session gets byte-identical inputs or a loud failure. Everything lands in
`firmware/tools/vendor/` (gitignored).

| File | Size | Use |
|---|---|---|
| **`rnwf02_ota.bin`** | 578 048 | **normal updates** — written to a running module over AT |
| `rnwf02_dfu_high.bin` | 578 048 | DFU recovery image, single slot |
| `rnwf02_wholeflash.bin` | 1 982 464 | full image: both slots plus file system |
| `flfs_image.bin` | 16 384 | file system (certificate store) only |

**3.2.0 renamed all of these** (`*.bootable.bin` → `*_wholeflash.bin`, `*_dfu.bootable.bin` →
`*_dfu_high.bin`), so a version bump is never a one-line change. `flfs_image.bin` is byte-identical to
3.1.0's — the certificate store did not change.

Releases now come from the **SDK's GitHub releases**, `MicrochipTech/WINCS02-RNWF02-SDK`. That is *not*
where 3.1.0 came from: the RNWF02 Firmware software-library page still lists 3.1.0 as newest, which is
why it looked current. **Check the SDK, not the product page.**

## 4. The recovery path: DFU

Only for a module that will not boot. Needs an **FT232R** (not a CP2102 or CH340 — DFU entry is a
66-sample pattern bit-banged on three lines, which they cannot do), and the on-board MCP2200 cannot do
it either since it reaches only UART1 and MCLR.

On the `EV72E72A`, DFU and AT share one pin pair. From Table 8-1 of the Application Developer's Guide:

> "RNWF02 Add On Board design has interconnected UART1_Tx to PB1/DFU_Tx and UART1_Rx with PB0_Rx to
> enable both Mission mode and DFU operation over the single UART interface."

So wire to the mikroBUS UART pins — the only pair reachable without soldering, since module pin 26 is
only test point TP205 and pin 10 has no test point at all. Power **through mikroBUS** for DFU
(`JP200` on **J201-2 + J201-3**), USB-C unplugged, and the red **D204** confirms VDD.

| FTDI (TTL-232R colour) | | Add-on board |
|---|---|---|
| TXD, orange — D0, `PGC` | → | **J205 pin 4** (`RX`, module UART1_RX, pin 19) |
| RXD, yellow — D1, `PGD` | ← | **J205 pin 3** (`TX`, module UART1_TX, pin 14) |
| CTS, brown — D3 | → | **J204 pin 2** (`RST`, module MCLR, pin 4) |
| GND, black | — | J204 pin 8 or J205 pin 8 |

Verify entry first — this writes nothing and is safe to repeat:

```sh
uv run --with pyftdi firmware/tools/rnwf02_dfu_mac.py \
    --utils-dir firmware/tools/vendor/utils/rnwf-utilities-v2.0.1/dfu \
    --url "ftdi://ftdi:232:XXXXXXXX/1"
```

Expect `In DFU mode. PE version 1, device ID 29c70053.` (`ftdi:///?` lists adapters.) The fifth
character of the device ID is a wildcard — the guide writes it `0x29C7x053`. Then:

```sh
uv run --with pyftdi firmware/tools/rnwf02_dfu_mac.py \
    --utils-dir firmware/tools/vendor/utils/rnwf-utilities-v2.0.1/dfu \
    --url "ftdi://ftdi:232:XXXXXXXX/1" \
    --write firmware/tools/vendor/fw/RNWF02_module_release_3.2.0/bin/rnwf02_dfu_high.bin high --yes
```

On macOS this shim exists because Microchip's `do_dfu.py` is Windows/Linux only and needs the
proprietary D2XX driver (**ADR-0026**). On Windows or Linux, use their tool directly.

## 5. Traps

Each of these presents as "the module is dead". None of them is.

**DFU entry is flaky.** It failed twice at settings that then worked, and took three attempts on the
second module. Microchip's own code loops for the same reason. The shim retries five times; if all five
fail, that is a real signal — check wiring and power.

**A chip left in bit-bang mode poisons everything after it.** If a DFU run aborts before restoring UART
mode, the FT232R stays latched. It still opens happily as a serial port, and then every byte written
goes out as *pin levels* rather than UART frames, while reads return a flood of pin samples no baud rate
can explain. Recover with `--reset-ftdi`.

**Do not mix pyftdi and `/dev/cu.usbserial-*` on the same adapter.** Once the kernel VCP driver has
touched the device, libusb writes start failing with `Errno 60` — *every* write, including short ones,
where the same code worked minutes earlier. The shim now recovers automatically (USB reset plus a
pyftdi cache flush), but the cleaner habit is `ftdi://` URLs throughout, which `probe_rnwf02.py`
accepts.

**A failed or interrupted write is recoverable.** For DFU, the pattern and Programming Executive live in
boot ROM rather than the flash being erased, so re-enter and write again. For the NVM path, the running
firmware is in the *other* slot until activation, so an interrupted download leaves the module bootable.

**Anti-rollback is real and now steep.** 3.1.0 raised the security level 0 → 1; 3.2.0 raised it to
**4**. Going back is very likely blocked, even though the previous image is physically still there:

```
AT+DI
+DI:15.0,0xFFFFFFB0,0x04030200,0x600F0000,2   <- high slot: 3.2.0, sequence FFFFFFB0, active
+DI:15.1,0xFFFFFFC0,0x01030100,0x60000000,3   <- low  slot: 3.1.0, sequence FFFFFFC0
```

The boot ROM picks the **lowest** sequence number, which is why 3.2.0 boots. Both the DFU and NVM tools
write to the inactive slot and alternate, so the previous version survives as a fallback either way.

## 6. When a newer release appears

`3.2.0` is what was latest on 2026-08-18. When a newer one lands:

1. Bump `VERSION`, `FW_REV`, the URL, the filenames **and** every checksum in
   `firmware/tools/fetch-rnwf02-firmware.sh`, in one commit.
2. Re-verify the AT vocabulary against that release's reference. The SDK ships it as **Markdown**
   (`RNWF02/doc/RNWF02_AT_Command_Reference.md`), so this is a grep rather than a PDF crawl. Watch
   `+WSTAC` and `+MQTTC` parameter IDs, and anything touching presentation format — 3.2.0 added an
   `ATF` command controlling how strings and integers are rendered, which the line parser assumes.
3. Update `--require-version` in the pre-ship gate, and this document's tables.
4. Journal the change with the version and date.

## 7. Sources

| Claim | Where |
|---|---|
| UART1 default 230400 8N1; pin 14 TX out, pin 19 RX in; strap table | Data Sheet **DS70005544C** Table 2-1, §2.2 |
| mikroBUS pinout; JP200 power selection; MCP2200 map; R231 pull-up | Add-on Board UG **DS50003575C** §3.1, §3.3, Fig 5-3/5-4/5-5 |
| DFU pins, pattern, PE protocol, slot and sequence rules | Application Developer's Guide §8, Table 8-1 |
| AT command reference, changelog, errata, firmware releases | `MicrochipTech/WINCS02-RNWF02-SDK` |
| 3.2.0 contents and the absence of CVE fixes | `doc/RNWF02_v3.2.0_Release_Notes.md` in the release zip |
| `+MQTTSUB` is the SUBACK, and a code above 2 is a failure | AT reference AEC section; `wdrv_winc_mqtt.c` in `wireless_apps_rnwf` |
| TX cannot leave PAD0 | SERCOM USART `CTRLA.TXPO`; Microchip **KB-000007252** |
