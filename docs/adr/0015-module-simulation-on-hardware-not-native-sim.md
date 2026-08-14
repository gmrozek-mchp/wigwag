# ADR-0015 — The module simulator runs against real hardware, because `native_sim` does not exist on macOS

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

`docs/PLAN.md` step 15 and decision D39 planned to test the RNWF02 AT client with no hardware at
all: build the firmware for Zephyr's `native_sim` board, connect its UART to a host PTY, and put
`firmware/sim/fake_rnwf02.py` on the other end bridging to a real `mosquitto`. That gives a full
connect state machine under test — reset, `AT`, Wi-Fi, MQTT, subscribe, backoff — before the
Wi-Fi module or the PCB exists. It is a good plan and it is why D39 exists.

It is also impossible on this development machine. Zephyr's POSIX-architecture documentation is
explicit: the POSIX architecture *"is known to **not** work on macOS due to fundamental
differences between macOS and other typical Unixes."* `native_sim` is a POSIX-architecture board.
Everything downstream of it — the PTY UART driver, the `unit_testing` platform, `twister` runs on
this host — inherits the limitation. The documented workaround for non-Linux hosts is a Linux VM
or WSL2.

Development is on macOS (arm64), and the constraint arrived at exactly the point where the AT
client work starts. The available substitutes had to be weighed on faithfulness, not just
convenience, because the whole value of the simulator is that passing it means something.

Two facts made the decision easy:

- **The `EV10P22A` PL10 Curiosity Nano is on hand**, carrying `PIC32CM6408PL10048` — the same
  64 KB/8 KB memory as the target part (ADR-0008, D44). The Wi-Fi add-on board is not yet.
- **The firmware speaks only AT over a UART.** The module owns Wi-Fi, TCP/IP, TLS and MQTT
  (ADR-0002), so the only interface the simulator has to imitate is a serial line carrying ASCII.
  A serial line is exactly what real hardware offers most cheaply.

## Decision

**The RNWF02 simulator talks to real PL10 hardware over a real UART.** `fake_rnwf02.py` runs on
the Mac as an AT-command server plus a `paho-mqtt` client bridging to `mosquitto`, connected to
the Curiosity Nano's **SERCOM0** — the same peripheral that drives the module on the PCB (D46) —
through a 3.3 V USB-UART adapter. The Zephyr console stays on `sercom1`, the board's on-board
debugger CDC port, so logs and the AT link never share a wire.

**The AT client is layered so most of it is testable with neither hardware nor Zephyr.**
`firmware/src/rnwf_at.c` holds the line assembler and connect state machine behind an injected
byte transport and clock, with no Zephyr headers, so it compiles and runs under plain `clang` on
macOS for unit tests. Only the transport adapter needs a device.

**`native_sim` remains a supported target, not a used one.** The application stays board-agnostic
so a Linux host or CI runner can build and run it, and D39's original intent survives there. It
is simply not the local development path, and nothing in the workflow may depend on it.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **Linux VM (podman machine / Lima / UTM) running true `native_sim`** | Matches the original plan exactly and is the documented workaround. Rejected as the primary path: it is the highest setup cost of the options, moves the build into a VM with a shared filesystem, and — decisively — buys a *less* faithful test than the hardware already on the desk. `native_sim` exercises a POSIX UART stub; the cnano exercises the real SERCOM driver on the real part. |
| **QEMU Cortex-M board (`mps2/an385`) with uart1 on a host TCP socket** | Runs natively on macOS, needs no hardware, gives a genuine `arm-zephyr-eabi` build and real Zephyr thread/UART paths, and would run unattended in CI. Genuinely good, and the strongest runner-up. Rejected because it still is not this SoC: different UART driver, different clock tree, and `ram_report` numbers that mean nothing for the 8 KB question. With the correct part in hand, emulating a different one is hard to justify. |
| **Run the AT link over the cnano's CDC port and drop the console** | Needs no USB-UART adapter at all. Rejected as the default because it trades away all console output at exactly the moment a connect state machine is being debugged. Kept as the documented fallback for a machine with no adapter. |
| **Wait for the `EV72E72A` add-on board and test only against the real module** | Most faithful of all, eventually. Rejected because it blocks the AT client on shipping, and because a fake module is *better* than the real one for the cases that matter most — forcing `ERROR` responses, timeouts, truncated lines, and unsolicited results at chosen moments. Testing against real hardware is not a substitute for testing failure paths on demand. |
| **Skip simulation; write the AT client and debug it on the real module later** | Rejected outright. Fail-visible behaviour (ADR-0007) and reconnection with backoff are precisely the paths that never get exercised by a happy-path bring-up, and they are the ones that decide whether the light lies. |

## Consequences

**Accepted costs**
- Local end-to-end testing needs the board plugged in, so it cannot run unattended in CI. Only
  the pure-C core tests are hardware-free — which is a reason to keep that core large and the
  adapter thin.
- A 3.3 V USB-UART adapter becomes a required bench item (or the console-off fallback applies).
- One extra transport adapter to maintain versus a single `native_sim` build.
- Enabling SERCOM0 needs its own devicetree work — a pinctrl group and a generic-clock channel,
  the same pattern D49 established for TCC0.

**Benefits**
- The tested configuration is the shipped configuration: real SoC, real SERCOM driver, real
  8 KB part, `ram_report` numbers that count.
- SERCOM0 is exercised as the module UART before the PCB commits to it (D46, D59).
- Failure injection stays fully under our control, which is what ADR-0007 needs.
- The pure-C core means the parser and state machine get fast unit tests on any host, macOS
  included — arguably a better outcome than the `native_sim` plan, which had no such split.

**Revisit if** development moves to Linux (then `native_sim` becomes available for free and
should be added back as the CI runner), or if the AT core's unit tests plus hardware testing prove
insufficient for some timing-dependent bug — in which case the QEMU runner is the pre-analysed
next step.
