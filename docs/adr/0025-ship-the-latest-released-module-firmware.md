# ADR-0025 — Every module ships on the latest released RNWF02 firmware, currently 3.1.0

- **Status:** Accepted
- **Date:** 2026-08-18

## Context

The `EV72E72A` arrived running **2.0.0** (`+GMR:"2.0.0 0 e41f977cb [16:31:26 Apr 12 2024]"`), and the
first real module contact showed that what the silicon speaks and what this firmware assumes were
never the same dialect.

`firmware/src/rnwf_at_cmds.h` was written from the *AT Command Specification, Network Controller
3.1.0, Revision 58a15dc2*, because that was the only specification available. The shipped 2.0.0
predates it by sixteen months. That gap is not theoretical:

- **`AT+CFGCP` does not exist on 2.0.0.** `AT+CFGCP=?` returns `ERROR:0.3` — unknown command — where
  every command that *does* exist returns `ERROR:0.5` for a syntax complaint. D62 requires firmware
  ≥ 3.0 for exactly this, and ADR-0012's provisioning plan is built on it.
- **2.0.0 is missing the KRACK fixes** (CVE-2017-13079, CVE-2017-13081) and the TLS renegotiation fix
  (CVE-2009-3555), both listed as fixed in the 3.1.0 release notes, which also move WolfSSL to 5.7.4
  and "strengthen cryptographic elements used in WPA2/WPA3".

A desk lamp is not a high-value target, but it holds Wi-Fi credentials and joins a home network, and
shipping a device with known, already-patched Wi-Fi and TLS vulnerabilities is not defensible when the
fix is a one-time bench step.

The counter-pressure was real and worth recording: **2.0.0 works.** Probing it directly confirmed that
every command our connect script issues is present — `ATV3`, `+WSTAC` IDs 1/2/3/4/5/7/8, `+MQTTC` IDs
1–6/8/9. Nothing in the current firmware needs 3.1.0.

## Decision

**Every RNWF02 passes through `docs/module-firmware-dfu.md` before it is fitted or shipped, and lands
on the latest released firmware.** As of this ADR that is **3.1.0**, revision `58a15dc2` — the
revision `rnwf_at_cmds.h` is written against.

"Latest released", not "3.1.0", is the decision. When Microchip publishes a newer package, the project
follows it: bump `VERSION` and the checksums in `firmware/tools/fetch-rnwf02-firmware.sh` together,
re-check the AT vocabulary against that release's specification, and move the pre-ship gate.

The gate is mechanical, not a habit: `firmware/sim/probe_rnwf02.py --require-version 3.1.0` exits **3**
unless the module agrees, so it can sit in a script and fail a build.

Modules are updated by writing the **single-slot** image to the **low** partition, which leaves the
factory image intact in the high slot as a fallback.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| Stay on the shipped 2.0.0 | Ships known-vulnerable Wi-Fi and TLS (KRACK, CVE-2009-3555), and permanently forecloses ADR-0012's provisioning plan, which needs `+CFGCP`. The saving is one bench step per unit. |
| Rewrite `rnwf_at_cmds.h` against the 2.0.0 specification | Two dialects to maintain, and the *older* one is the one with no `+CFGCP` and no security fixes. It also means every future vocabulary question has to ask "which firmware?" — exactly the ambiguity that cost this session a wrong-baud diagnosis. |
| Pin to 3.1.0 forever | The reason for updating (security fixes) is the reason not to freeze. Pinning would recreate this ADR's problem at the next CVE. |
| Update only modules that need a new feature | Fleet fragmentation: the AT vocabulary would then be per-unit, and a bug reproducible on one desk light and not another. |
| Write the full combined image (`rnwf02.bootable.bin`) | Rewrites both slots plus the file system, so it discards the factory fallback and takes minutes instead of ~30 s, widening the window for the USB write timeouts that are already known to happen. |

## Consequences

**Accepted costs**

- A bench step per module: an FTDI adapter, four jumpers, and a documented procedure with four
  distinct "looks dead but isn't" failure modes.
- **Anti-rollback**: 3.1.0 raises the device security level from 0 to 1 so earlier firmware can be
  rejected. Partially mitigated by writing only the low slot — the factory image survives in the high
  slot and `AT+DI` shows both — but this is close to a one-way door and should be treated as one.
- The vendor DFU utility does not run on macOS, so the project now carries a shim (ADR-0026).
- Following "latest" means periodically re-verifying the AT vocabulary; 3.1.0 already added
  `+WSTAC:9`, and its ATV3 error responses now carry prose (`ERROR:0.5,"Incorrect Number of
  Parameters"`), which `rnwf_at_cmds.h` attributes to verbosity levels 4–5 only.

**Benefits**

- The silicon and `rnwf_at_cmds.h` finally describe the same protocol, so a wire-level surprise is a
  bug rather than a version question.
- `+CFGCP` exists, unblocking D62 and ADR-0012.
- No shipped unit carries a Wi-Fi or TLS vulnerability that was patched before it was built.

**Revisit if**

- A future release changes the AT vocabulary incompatibly, making "latest" a porting project rather
  than a bench step — then this becomes "latest qualified", with the qualified version named here.
- The anti-rollback counter ever blocks a *downgrade we actually need*, which would make updating
  before qualification the wrong order.
- Volume makes per-unit bench work untenable, at which point the OTA path (App Developer's Guide §8.2,
  host-assisted) replaces the FTDI procedure.
