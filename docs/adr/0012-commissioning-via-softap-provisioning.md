# ADR-0012 — Commissioning: compile-time for v1, SoftAP provisioning for v1.1

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

How does a wigwag get its Wi-Fi credentials and broker configuration in the first place?
Until now this was one line in the plan ("credentials via Kconfig for v1") with AP-mode
provisioning listed vaguely under future directions. That is not good enough, because the
answer **constrains the PCB** and therefore has to be settled before Phase 3 layout.

The device is unusually constrained as commissioning targets go:

- **No USB peripheral.** The PL10 peripheral summary (datasheet Table 8-1) lists
  SERCOM0/1, TC0/1/2, TCC0, ADC, AC, CCL, PTC, DMAC, EVSYS, RTC, WDT and friends —
  **there is no USB device controller**. USB-C on the board is power-only (D24).
- **No Bluetooth.** RNWF02 is Wi-Fi only. It has a three-wire PTA interface for
  coexistence with an *external* Bluetooth radio, but no Bluetooth of its own, and the
  PL10 has no radio at all.
- **No display and one button.** Three lamps and a tactile switch is the entire UI.
- **8 KB of SRAM** (ADR-0008), so the host cannot host a web server or a JSON config parser.

Two things need commissioning, and they are easy to conflate:

1. **Wi-Fi credentials** — SSID, passphrase, security mode.
2. **Broker configuration** — host, port, TLS, username, password, topic prefix.

The RNWF02 provisioning machinery handles (1) natively. Nothing handles (2) for us.

What the module actually provides, from the RNWF02 Application Developer's Guide and
Supplemental User Guide:

- **Soft-AP mode** (datasheet: *"Supports STA Mode and Soft AP Functionality"*).
- **A provisioning service** that *"implements or handles all the required AT commands to
  start the module in Access Point mode and open up a TCP tunnel or serve a HTML web page
  to receive the Wi-Fi credentials"* — so both a socket tunnel and a web page are available
  without us writing an HTTP server.
- **`AT+WPROV`, a provisioning socket**: enable hotspot → enable provisioning socket →
  client joins → client connects to the socket → client sends commands → client asks the
  module to join the provisioned AP.
- On success, a callback returns **`[Mode, SSID, Passphrase, Security, Autoenable]`**, which
  *"user application can store securely for auto reconnection on every boot up"*.
- A **Microchip "Wi-Fi Provisioning" mobile app** already speaks this, with a reference
  demo at `wireless_apps_rnwf/apps/wifi_easy_config/`.

## Decision

**Phase in two steps, and design the PCB now so the second step needs no respin.**

### v1 — compile-time credentials

Wi-Fi and broker settings come from Kconfig, sourced from a gitignored
`firmware/credentials.conf`. No commissioning UI at all. This gets a working light in the
shortest path and keeps the 8 KB budget uncontested while the lamp and AT-client code is
still being written.

### v1.1 — SoftAP provisioning, using the module's own service

- **Long-press the button** (~5 s) enters provisioning mode. The button already exists
  (D35), so this costs no hardware.
- The host asks the module to start Soft-AP plus its provisioning service. **The module
  serves the page and parses the credentials; the host only issues AT commands and handles
  the completion callback.** That is what keeps this feasible in 8 KB.
- **Lamp feedback during provisioning:** all three lamps cycle slowly in sequence — a
  pattern that appears in no other state, so provisioning mode is unmistakable and cannot
  be confused with `WAIT` or the amber link-lost flicker (ADR-0007).
- On success, credentials are persisted by the module for auto-reconnect, and the device
  drops back to station mode.

### Broker configuration is a separate problem, and v1.1 must solve it too

The module's provisioning service knows about Wi-Fi, not MQTT. Planned approach: extend
the provisioning exchange with the broker fields, carried over the same provisioning socket
(`AT+WPROV`) rather than the stock web page, since we control both ends. Until then the
broker stays compile-time even in v1.1 — stated explicitly rather than left ambiguous.

### What the PCB must preserve (the part that actually matters now)

| Requirement | Why |
|---|---|
| Host UART (SERCOM0 TX/RX) broken out to a header or test pads | Enables bench commissioning and debugging with a serial adapter, at zero BOM cost |
| Button on a GPIO with interrupt capability | Long-press trigger for provisioning mode |
| Module `UART2_TX` on a test point | The module's own 230400-baud debug log — already planned (ADR-0002) |
| Module `MCLR` under host GPIO control | Provisioning needs a clean module reset — already planned |
| SWD header | Reflashing, which is the v1 commissioning mechanism |

None of these are new parts. They are pin assignments and pads, which is exactly the kind
of thing that is free now and a respin later.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **USB CDC commissioning** | The natural answer for a desk gadget with a USB-C socket — plug it into the laptop and configure it. **Impossible on this MCU: the PL10 has no USB peripheral** (Table 8-1). It would require adding a USB-UART bridge (e.g. MCP2221A), turning USB-C into a data port, adding D+/D− routing and ESD protection, and reversing D24. Rejected as a large hardware cost to avoid a one-off SoftAP flow — but this is *the* decision that had to be made before layout, because retrofitting it is a respin. |
| **Bluetooth / BLE commissioning** | The most pleasant UX, and standard for headless devices. Needs a radio neither part has: RNWF02 is Wi-Fi only (PTA is for coexisting with an external BT radio) and PL10 has none. Would mean a second module such as WBZ451 — more silicon, more antenna work, more Zephyr enablement. |
| **Compile-time only, forever** | Zero UI to build, and honestly adequate for one device on one network. Rejected as the *end state* because every network change means rebuild-and-reflash, which needs a debug probe and the toolchain — unreasonable for a finished object, and it makes the thing ungiftable. |
| **WPS push-button** | Would pair with one button press and no app. Deprecated, has known security weaknesses, is not listed among RNWF02's supported features, and many APs now ship with it disabled. |
| **Hard-coded credentials with a captive portal we write ourselves** | Full control of the flow. Rejected on footprint: an HTTP server and form parsing on the host in 8 KB, when the module already provides both, is exactly the wrong place to spend RAM. |
| **microSD card or a config file on removable media** | Trivially editable on any computer. Needs a card socket, SPI, a filesystem and a physical slot in the enclosure — a lot of hardware and RAM to avoid an AP mode the radio already implements. |
| **NFC tag provisioning** | Very slick, and used by commercial smart-home devices. Needs an NFC front-end IC and antenna, and is a poor fit for a mains-powered desk object you can already reach. |
| **Provision over the MQTT link itself** | Elegant — but circular: it needs Wi-Fi and a broker to already be configured. Viable for *re*configuration later, not for first commissioning. |

## Consequences

**Accepted costs**
- v1 users (i.e. me) must rebuild and reflash to change networks. Acceptable for a bench
  device, and time-boxed by v1.1.
- v1.1 depends on the RNWF02 provisioning service behaving as documented, which is
  unverified — it is a Phase 2 spike, ideally alongside the D49 TCC PWM spike.
- Credentials live in module NVM after provisioning. **Secure storage is not addressed**:
  the module has Trust&Go on the chosen variant (ADR-0002) but wiring provisioned
  credentials into it is out of scope here.
- The broker configuration path is genuinely unsolved and remains compile-time for longer
  than the Wi-Fi credentials do.
- A long-press on the only button is a discoverable-by-documentation gesture, not an
  obvious one. Lamp feedback mitigates it once entered.

**Benefits**
- The PCB is committed with commissioning already considered, so the mechanism can improve
  without a board change — which was the entire point of raising this before Phase 3.
- The module does the hard parts (AP, web page, credential parsing), so the host cost is
  a state machine and a few AT commands, which fits 8 KB.
- The existing button and lamps supply the whole UI; no new hardware.
- A serial adapter on the UART pads gives a bench commissioning path from day one.

**Regulatory note, not a v1 requirement**
The EU **RED Delegated Act** has applied since 1 August 2025 to network-connected radio
equipment: no default passwords, secure credential storage, authenticated firmware updates,
secure communications. Microchip's own guidance is explicit that RNWF02 reference
applications ship with default passwords that an end product must remove, and that access
control, authentication and secure storage are the integrator's responsibility. This is a
personal device, not a product, so none of it is binding — but if wigwag ever became one,
commissioning is where most of that work would land, and "provisioning AP with no
password" would be the first thing to fail. Worth knowing now rather than discovering later.

**Revisit if** the provisioning-service spike fails (fall back to the `AT+WPROV` socket with
a small host-side protocol, or to UART commissioning), or if a future revision adds a USB
bridge for other reasons — at which point USB commissioning becomes nearly free and this
decision should be reconsidered.
