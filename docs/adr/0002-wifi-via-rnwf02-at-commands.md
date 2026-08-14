# ADR-0002 — Wi-Fi is an RNWF02 network co-processor driven by AT commands

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

The device must reach a local MQTT broker over Wi-Fi, on Microchip silicon, from a Zephyr
application running on a Cortex-M0+ with 8 KB of SRAM (ADR-0001).

The obvious path — a Wi-Fi chip driven by Zephyr's native networking stack — collapses on
inspection:

- **Zephyr's only in-tree Microchip Wi-Fi driver is `winc1500`, and it is deprecated.** Its
  Kconfig carries an explicit notice: *"This offloaded wifi driver is deprecated and will be
  removed in a future release. It is currently scheduled to be removed in Zephyr 4.6"*, for lack
  of maintainer support. It also defaults to 2 concurrent sockets. Starting a new design on it
  in 2026 means adopting a component with a published removal date.
- **Zephyr's own IP stack is far too large for 8 KB of SRAM**, so even with a supported MAC-level
  part, the networking layer would dictate a bigger MCU and defeat ADR-0008.

The remaining option is a **network co-processor**: a module that terminates Wi-Fi, TCP/IP, TLS
*and* MQTT itself, leaving the host to speak a thin control protocol.

## Decision

**`RNWF02PC-I/100`** — PCB antenna, with the Trust&Go secure element — connected to the host
MCU by a **two-wire UART** and driven with **ASCII AT commands**.

The module provides, in its own firmware: 802.11 b/g/n, WPA3, TCP, UDP, DHCP, DNS, ARP, HTTP,
TLS 1.2, and an **MQTT client with publish/subscribe exposed as AT commands**. Hardware
accelerators for Wi-Fi and TLS. FCC/ISED/CE/UKCA/MIC/KCC/NCC certified and Wi-Fi Alliance
certified. 3.0–3.6 V VDD. 28-pin SMD, 21.7 × 14.7 × 2.1 mm. 432 in stock.

Consequences for the host firmware: **no Zephyr networking subsystem at all**. `CONFIG_NETWORKING`
stays off. The host needs one UART, a bounded line-assembly buffer, and a state machine.

Host connections: `UART1_TX`/`UART1_RX` (essential), `MCLR` for module reset (essential),
`INTOUT` (optional wake), `UART2_TX` broken out to a test point for the module's own 230400-baud
debug log. `STRAP1`/`STRAP2` pulled **low** to select the UART host interface rather than SPI/I²C.

The module's antenna rules are treated as binding constraints on both PCB and enclosure
(ADR-0009 records the mechanical consequences): module at the board edge, ground-plane edges
aligned, **≥10 mm clearance to plastic in all directions**, metal **≥31.75 mm** from the trace
antenna.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **ATWINC1500 + Zephyr `winc1500` driver** | The only in-tree Microchip Wi-Fi driver, and it is **deprecated with a scheduled removal in Zephyr 4.6** for lack of a maintainer. Also SPI with a binary protocol and a ~25.5 KB flash / 8.5 KB RAM host driver — the RAM figure alone exceeds our entire budget. |
| **WINCS02** (`EV68G27A`) | Newer than RNWF02 and actively promoted, but SPI with a binary protocol, needing a host-side driver we'd have to write and maintain. RNWF02's ASCII/UART interface is debuggable with a serial terminal and needs no driver. |
| **WFI32E01 / PIC32MZ-W1** | Single-chip Wi-Fi MCU; would eliminate the host MCU entirely and collapse the BOM. MIPS — no Zephyr SoC support. Rejected in ADR-0001. |
| **WBZ451 / PIC32CX-BZ2 (BLE)** | Would avoid Wi-Fi provisioning entirely, but no upstream Zephyr SoC support, and BLE range/pairing is a worse fit for a fixed desk appliance than infrastructure Wi-Fi. |
| **RNWF02UE (U.FL external antenna)** | Better real-world range and freedom to place the antenna outside the plastic. Rejected for the desk form factor: a connector, pigtail, antenna and extra assembly step, plus a less finished object, to solve a range problem that does not exist at 2 m. Layout keeps a DNP U.FL escape hatch. |
| **RNWF02PE (no Trust&Go)** | Marginally cheaper without the secure element. Kept the `PC` variant so the deferred TLS work (ADR-0003) has a hardware root of trust available without a board respin. |
| **Zephyr native IP stack on a bigger MCU** | Would make the host "properly" networked, but the stack's footprint forces a larger part and defeats ADR-0008. |

## Consequences

**Accepted costs**
- We write and own an AT-command client (`firmware/src/rnwf_at.c`): bounded ring-buffer line
  assembly, request/response with timeouts, and unsolicited-result-code dispatch. Text parsing
  is fiddlier than a binary protocol and needs careful bounds discipline at 8 KB.
- Throughput is limited by the UART. Irrelevant — the payload is a few dozen bytes per state
  change.
- Wi-Fi credentials must be provisioned. v1 uses compile-time Kconfig from a gitignored
  `credentials.conf`; AP-mode provisioning is a future direction.
- Two processors to keep straight when debugging, mitigated by the module's separate debug UART.

**Benefits**
- No dependency on any deprecated Zephyr component, and no Zephyr networking footprint at all.
- MQTT, TLS and reconnection logic live in vendor-maintained, certified module firmware.
- The entire link is debuggable by hand from a serial terminal before any host code exists.
- Radio certification is inherited from the module.
- Trust&Go gives TLS a hardware root of trust when we get to it.

**Revisit if** the AT client's bounded buffers prove incompatible with the 8 KB budget (unlikely
— the module does the heavy lifting), or if a maintained, in-tree Zephyr driver appears for a
current Microchip Wi-Fi part.
