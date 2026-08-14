# ADR-0013 — The broker address is configured during provisioning, not auto-discovered

- **Status:** Accepted
- **Date:** 2026-08-14

Resolves D60, left open by ADR-0012: how does the *broker* configuration get commissioned,
as opposed to the Wi-Fi credentials?

## Context

ADR-0012 settled Wi-Fi commissioning (SoftAP provisioning via the module's own service) but
explicitly left broker configuration unsolved, because the module's provisioning service
knows about Wi-Fi and nothing else. The obvious wish is that the device could simply *find*
the broker, removing a configuration step entirely.

There is a real standard for this. DNS-SD/mDNS defines service browsing, and MQTT has
registered service names — `_mqtt._tcp` (1883) and `_secure-mqtt._tcp` (8883). Home
Assistant and other tools do browse for brokers that way.

The problem is that **neither end of this system speaks it**:

| Layer | mDNS / DNS-SD |
|---|---|
| **RNWF02** | Not supported. Network features are consistently listed across the datasheet and sell sheet as TCP, UDP, DHCP, ARP, HTTP, MQTT, IPv4/IPv6, TLS 1.2, DNS and SNTP. No mDNS, no Zeroconf, no link-local naming. |
| **mosquitto** | Does not advertise. No mDNS or avahi symbols in the installed binary, nothing in its usage output, and a live `dns-sd -B _mqtt._tcp local` browse on the development LAN returned no results. |

Worth distinguishing, because the terms collide: Microchip's **Harmony 3 TCP/IP Library**
*does* provide `TCPIP_MDNS_ServiceRegister` and Zeroconf link-local support — but that is the
host-side stack for parts like PIC32MZ that run their own IP stack. It is not RNWF02 module
firmware, and it is not reachable over the AT interface. Likewise **Home Assistant "MQTT
discovery"** is a different mechanism entirely — a device describing *itself* to Home
Assistant over MQTT once connected — not broker discovery.

So mDNS would mean implementing DNS-SD packet construction and response parsing on the host
MCU over a raw UDP socket, in 8 KB (ADR-0008), *and* separately arranging for the broker to
be advertised. All to avoid typing a hostname once.

Meanwhile, a capability found while investigating this changes the arithmetic:
**`AT+CFGCP`**, Configuration Storage/Retrieval, introduced in RNWF02 firmware v3.0 —
*"allows AT command configurations to be either temporarily stored in volatile memory or
archived to non-volatile storage for later retrieval… the commands re-played upon
retrieving."* Because MQTT settings are themselves configured by AT commands, the module can
persist **broker configuration, not just Wi-Fi credentials**. Configuration therefore happens
once in the device's life, not once per boot.

## Decision

**The broker address is entered during provisioning and persisted on the module. It is not
discovered.**

- The v1.1 provisioning exchange (ADR-0012) carries broker fields alongside the Wi-Fi
  credentials: host, port, TLS on/off, username, password, topic prefix. Delivered over the
  `AT+WPROV` provisioning socket, where we control both ends, rather than the module's stock
  Wi-Fi-only web page.
- Configuration is committed to module NVM with **`AT+CFGCP`**, so a reboot replays it and the
  device reconnects without host involvement — the same property retained MQTT messages give
  the *state* (ADR-0003), applied to the *config*.
- **A hostname is the default, not an IP.** The broker field accepts a DNS name, and the
  module's DNS client resolves it. Most home routers register DHCP client hostnames in local
  DNS, so `mymac.lan` or similar frequently works and survives DHCP lease changes — which a
  hardcoded IP does not. An IP remains valid for networks where that fails.
- v1 keeps the broker compile-time via Kconfig, unchanged from ADR-0012.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **mDNS / DNS-SD browsing for `_mqtt._tcp`** | The standard, correct answer, and genuinely zero-configuration when it works. Rejected because **neither end supports it**: RNWF02 has no mDNS, and mosquitto does not advertise (both verified, not assumed). It would need a hand-rolled DNS-SD implementation on an 8 KB MCU plus a separate advertisement mechanism on the host, to save one hostname entry that `AT+CFGCP` makes a once-ever event. Revisit if RNWF02 firmware ever gains mDNS. |
| **A UDP broadcast beacon from `wigwagd`** | Attractive: the host daemon already exists, the module supports UDP, and it needs no mDNS stack. Rejected for v1.1 on two grounds — it is trivially spoofable, so anything on the LAN could redirect the light to a hostile broker; and it makes the device dependent on a *host* being alive to find the broker, when the whole point of retained messages is that the device and host are independent. Reconsider as an optional convenience that merely pre-fills the provisioning form. |
| **DHCP option carrying the broker address** | Clean, and how enterprise gear does this. Needs router configuration the user may not control, and the AT interface exposes no way to read custom DHCP options. |
| **DNS SRV record (`_mqtt._tcp.example.com`)** | Works with ordinary unicast DNS, which the module *does* have. Rejected because the AT DNS client resolves names to addresses; there is no evidence it can query SRV records, and it needs DNS zone control that a home network lacks. |
| **Fixed well-known hostname (e.g. `wigwag-broker.local`)** | Zero configuration and no discovery protocol. `.local` requires mDNS, which the module lacks. A non-`.local` fixed name would need a DNS entry the user must create — configuration by another route, with less flexibility. |
| **Scan a QR code containing broker config** | Nice UX, used by commercial devices. Needs a camera-side app we would have to write, and the device still has to receive the payload over the same provisioning channel — so it is a front-end for the chosen mechanism, not an alternative to it. |
| **Cloud rendezvous** | Device asks a known endpoint where its broker is. Adds an internet dependency and an account to a device whose broker is usually two metres away. |
| **Try a list of candidates (localhost, gateway IP, common hostnames)** | Needs no user input and no protocol. Rejected as guessing: it is slow, it can silently connect to *someone else's* broker on a shared network, and a wrong guess produces exactly the confidently-wrong behaviour ADR-0007 exists to prevent. |

## Consequences

**Accepted costs**
- Commissioning has more fields than Wi-Fi alone, so the provisioning exchange is a custom
  payload over `AT+WPROV` rather than the module's stock page. More work than reusing the
  built-in web form.
- No zero-configuration story. Someone setting up a wigwag must know their broker's address.
  Acceptable for a device whose owner also runs the broker.
- Depends on `AT+CFGCP`, which requires module firmware **v3.0 or newer** — a version
  constraint to check during the Phase 2 spike, and a reason to record the module firmware
  version in the journal at bring-up.
- Relying on router-registered hostnames is convention, not guarantee; behaviour varies by
  router, and an IP is the fallback.

**Benefits**
- No mDNS implementation on an 8 KB device, and no new protocol to secure.
- No spoofable discovery path: the broker address arrives only through a provisioning flow
  the owner deliberately initiated.
- `AT+CFGCP` means configuration survives reboots and power loss, so the device recovers with
  no host involvement — consistent with how retained messages handle state.
- Hostnames tolerate DHCP lease changes, which hardcoded IPs do not.
- Broker and Wi-Fi are commissioned in one exchange rather than two mechanisms.

**Revisit if** RNWF02 firmware gains mDNS (then `_mqtt._tcp` browsing becomes the right
default, with configuration as the fallback), or if wigwag ever needs to be set up by someone
who does not administer the broker.

**Adjacent future work, deliberately out of scope:** publishing Home Assistant MQTT discovery
messages so a wigwag appears automatically as an entity in Home Assistant. That is the device
describing itself to a consumer, and is unrelated to finding a broker — but it is the feature
people usually mean when they say "MQTT auto-discovery", so it is worth naming to keep the two
apart.
