# ADR-0019 — Configuration over the console, with the line editor as a replaceable layer

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

D56 commissioned the device with compile-time Kconfig: credentials live in a gitignored
`credentials.conf` and are baked into the image. That was the right call for a first device, and it
means **changing a Wi-Fi password requires a rebuild and a debug probe**. For something meant to sit
on someone else's desk it is not a real answer, and it also blocks two smaller things:

- **Per-lamp `gain` calibration (D91) is judged by eye**, and it lives in devicetree. A value you must
  rebuild to change cannot be judged by eye at all — you lose the comparison while the toolchain runs.
- **Brightness resets to 255 on every boot.** The retained `wigwag/brightness` topic arrives moments
  after subscribing, so it self-corrects on a networked unit — and does nothing at all for a unit with
  no broker.

ADR-0018 then made a config console nearly free. It decided the console and the USB transport are the
same stream, so a line reader is needed either way: `state BUSY` from the daemon and `set ssid MyNet`
from a human are both lines on the console UART. The interactive console is not a second mechanism
bolted on, it is the transport with more verbs. And ADR-0017's flash driver, built the day before, was
sitting unused with a 4 KB `storage_partition` the board devicetree has reserved since day one.

The open question was what to build it *on*. Measured on this build rather than argued from Kconfig
defaults:

| Option | Cost |
|---|---|
| Zephyr `CONFIG_SHELL` | **does not link** — `region 'RAM' overflowed by 464 bytes`, i.e. ~4 KB, half the part's SRAM. `SHELL_MINIMAL` changes nothing measurable: it trims buffers while the 2 KB thread stack dominates. |
| `funbiscuit/embedded-cli` | Publishes no footprint figures. MIT, single header, static allocation, so it would satisfy Rule 5 — but history is the dominant term and 96-byte lines × a few entries put it at an estimated 600 B–1 KB. |
| Hand-rolled | **112 B** for the editor, measured. |

## Decision

**Hand-roll it, in three layers with narrow interfaces, and make the editing layer the throwaway
one.**

| Layer | File | Role |
|---|---|---|
| character-level editing — echo, backspace, escape filtering, bytes → lines | `lineedit.c` | **designed to be replaced** |
| vocabulary and validation — lines → `struct cmd` | `cmd.c` | holds the decisions; survives a swap |
| effects — apply a `struct cmd` to settings, lamps, state | `console.c` | survives a swap |

`lineedit.c` takes an injected output callback rather than calling `printk`, the same trick
`struct rnwf_at_io` plays for the AT transport. So it has no Zephyr in it, it is host-tested, and
substituting embedded-cli or a shell later means replacing one file that emits "here is a complete
line" events.

**Backspace is kept; history and tab completion are not.** Retyping a 63-character passphrase after
one typo is the actual pain, and it costs five lines. Arrow keys are made *inert* rather than useful,
which is the next most valuable thing: without an escape filter, pressing Up inserts a literal `[A`
into your passphrase.

**Over-long input is refused, not truncated** — `LINEEDIT_TOO_LONG`, and `settings_apply()` refuses
oversized values independently. A silently shortened passphrase is stored, looks plausible, and
produces an association failure with nothing to point at. This is Rule 4 applied to input.

**Settings persist in the storage partition via NVS**, not a hand-rolled record. The hard part is not
storing bytes, it is staying consistent across a power cut mid-write and wearing eight sectors evenly;
a corrupt settings store on a device whose only output is three lamps is a bad failure, and that is
exactly the code where subtle bugs live. NVS takes the flash device explicitly, which suits this
family — `FIXED_PARTITION_DEVICE()` cannot work here at all (ADR-0017, upstream bug 5).

**Stored values win over build-time defaults.** `credentials.conf` keeps its role from D37 as *defaults*
for a bench build or an unconfigured device, and `firmware/CMakeLists.txt` merges it automatically when
present so `west build` needs no extra flags.

**Secrets are never printed back.** `show` reports `<set>` or `<unset>` for the Wi-Fi passphrase and
the broker password. Unknown keys default to secret, so the failure direction is printing less. Typing
a passphrase still echoes it, which is unavoidable and is what the person typing expects.

**`set` stages; `save` commits.** A half-typed network is recoverable, and the reply says
`(not saved)` so the distinction is visible rather than remembered. `brightness` and `gain` apply
*immediately* as well, because the entire point of calibration is to see the change — they persist only
on `save`.

**`echo on|off` exists** so a host program driving the same port is not talking to a stream that
repeats everything back at it.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **Zephyr's shell subsystem** | The obvious answer, and it does not fit: measured, it fails to link, needing ~4 KB against 3 624 free. It could be forced in by cutting `CONFIG_SHELL_STACK_SIZE` to ~768, but shell handlers format with `vsnprintf` and D78 measured that class of work at 860 bytes on main's 1024-byte stack. With no MPU (D80) the overflow would be silent corruption found later by `STACK_SENTINEL`. Trading a corruption risk for line editing is a bad trade. |
| **embedded-cli, vendored** | Genuinely well-suited on paper: MIT, single header, no dependencies, static allocation. Rejected on two counts. Its footprint is unpublished and history-dominated, estimated 4–7× the hand-rolled editor; and it is an *interactive* CLI that wants to own the stream conversationally — echoing, holding a prompt, interpreting escapes — while our primary consumer is a daemon whose `state BUSY` writes would interleave with prompt redraws. It would also replace only the ~60 lines that tokenise, not the parts with judgement in them. Kept as the pre-analysed swap if line editing ever matters more than 500 bytes. |
| **A hand-rolled settings record instead of NVS** | Would cost ~0 RAM for the strings, because flash is memory-mapped here and a blob could be read in place with pointers — genuinely attractive against the 302 bytes NVS forces us to cache. Rejected because it means writing crash-safe flash update by hand, and losing settings to a power cut during `save` is worse than 302 bytes. |
| **Zephyr's `settings` subsystem on top of NVS** | Standard, and more machinery than eight keys need: a handler registry and name-based dispatch on a part where NVS's `uint16_t` ids are already the right granularity. |
| **Supersede ADR-0012's SoftAP provisioning** | Considered and explicitly declined. USB config becomes the *primary* path, but ADR-0012 stays accepted for the phone-only case — a unit that lives on a wall charger and never meets a computer. D58's long-press stays reserved for it. |
| **Make `state` over the console mark the link trusted** | It is what D104's transport selection will eventually do, and doing it now would half-implement that decision. Today a typed `state` sets the lamps and the fail-visible pattern still wins when nothing is trusted — correct per ADR-0007, and noted in `console.c` as the seam. |
| **A `lamp <which> <level>` override for calibration** | Would make calibration usable on an unlinked bench, and is the natural factory-test tool. Deferred: an override means the device deliberately displays something untrue, which needs a bounded lifetime and careful thought against ADR-0007 rather than a hasty addition. `lamp_scale()` applies gain even during the fault pattern, so gain changes are visible today. |

## Consequences

**Accepted costs**
- **RAM +656 bytes measured** (4 568 → 5 224 B, 63.8 % of 8 KB), broken down from `ram_report`:
  settings strings **302 B**, console ring and state **260 B**, line editor **112 B**, NVS bookkeeping
  **49 B**. The strings are the honest price of configurability: they used to be flash literals costing
  nothing, and `struct rnwf_at_config` borrows pointers that must outlive the client while NVS hands
  back copies rather than addresses.
- **Flash +8 248 bytes** (24 080 → 32 328 B, 52.6 %), mostly NVS plus the help and `show` strings.
- Headroom is now 2 968 bytes. That is still comfortable but it is the first change to make the 8 KB
  budget feel like a budget, and it is worth re-reading ADR-0008 before the next subsystem.
- A second interrupt source on the console UART. Receive had to become interrupt-driven for the same
  reason as D77: at 115200 a byte lands every 87 µs and a 10 ms poll would keep only the last byte of
  each interval. Typing survives polling; **pasting a passphrase does not**, and pasting is what people
  do.
- NVS ids are permanent. Reusing one across firmware versions would read a broker hostname as a
  passphrase, so the enum is append-only and says so.

**Benefits**
- Changing a Wi-Fi password no longer needs a rebuild, a probe, or the toolchain.
- Calibration by eye is possible on an assembled unit, which is the only way it was ever going to work.
- Brightness and calibration survive a reboot, including on a unit with no broker.
- The vocabulary is host-tested — 145 checks across `cmd`, `lineedit` and `settings` — so the parts most
  likely to be subtly wrong (a passphrase containing spaces, an arrow key mid-line, a value one byte too
  long) are asserted rather than eyeballed.
- The device can be inspected in the field: `show` states what it thinks it is configured for without
  revealing what it must not.

**Revisit if** line editing becomes worth ~500 bytes (embedded-cli is the pre-analysed swap, and only
`lineedit.c` changes), if the settings set grows enough that Zephyr's `settings` subsystem earns its
overhead, or when D104's transport selection lands — at which point console traffic becomes the host
heartbeat and the seam noted in `console.c` closes.
