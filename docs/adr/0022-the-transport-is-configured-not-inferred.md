# ADR-0022 — The transport is configured, not inferred

- **Status:** Accepted
- **Date:** 2026-08-15
- **Supersedes:** **ADR-0021** entirely, and the remainder of **ADR-0018**'s transport-selection
  decision. ADR-0018's decision to fit the `MCP2221A` and carry the console over it still stands.

## Context

This is the third shape transport selection has taken in two days, and the first two were wrong in the
same way.

**ADR-0018** chose the transport from evidence: whichever side was talking took the device.
**ADR-0021** noticed that falling back from USB to Wi-Fi silently substituted a *different machine's*
work, and fixed it by latching — once a host had spoken, the device was its until reset.

Both were wrong, and the second compounded the first: an automatic takeover that is also irreversible.
The consequence in practice is that plugging the device into a computer — for power, or to configure
it, or because a daemon on that machine happens to be pointed at a serial port — silently repurposes
the lamps, with no way back short of a power cycle.

The mistake in both was treating the choice as something to *detect*. **ADR-0013 had already settled
this exact shape for the broker address**: "the broker address is configured, not auto-discovered."
Transport selection should have followed that precedent from the start, and did not.

The underlying reason auto-selection cannot work is worth stating plainly, because it is not obvious:
**the two transports do not carry the same information.** A daemon aggregates the sessions of *its own
machine* (D30), so the serial publisher and the MQTT publisher are potentially different computers
reporting different work. There is no correct way to infer which one the user meant, because both are
telling the truth about different things.

## Decision

**A stored setting decides which transport owns the lamps.** `set transport usb|wifi`, persisted like
any other setting, shown first by `show` because it decides what the device *is*. Nothing the outside
world does can change it: no amount of traffic on the console takes the lamps from a wireless device,
and no Wi-Fi link takes them from a wired one.

**The default is `usb`** (D120). Wi-Fi cannot work until an SSID and passphrase have been set, so
anyone using it is already configuring the device and can set the transport in the same sitting. USB
needs nothing — plug a fresh device into a computer, start the daemon, and it works. The default should
be the transport that works out of the box.

**`transport.c` now decides only trust, not selection.** Which transport is active is known from the
first instruction. Whether it may be *believed* still depends on evidence: a `host on` within
`TRANSPORT_HOST_TTL_MS` (10 s) for the wire, and `link.c`'s verdict for Wi-Fi. There is no latch, no
release window, and no handover, because there is nothing to hand over to.

**A configured transport that is not working goes fail-visible and stays there.** That is the point:
amber means "I cannot tell you about the thing you asked about", which is honest, where quietly
answering with the other transport's data would not be (Rule 4, ADR-0007).

**A wired device never starts the RNWF02** (D118). Not merely to save the retries — an end-to-end
session measured 947 AT timeouts doing exactly that — but because a module that is running and
subscribed would deliver MQTT states to the lamps, which is the substitution this ADR exists to
prevent.

**The one easy mistake is called out where it happens.** Setting an SSID on a device whose transport is
`usb` prints a note saying the network will not be used and what to do about it, and the boot banner
repeats it. A silent footgun would otherwise be the cost of a `usb` default.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **Evidence-based selection** (ADR-0018) | Superseded. Plugging a cable in for power or configuration silently repurposed the device, and the fallback answered with a different machine's work. |
| **Evidence-based, but latched** (ADR-0021) | Superseded. Fixed the substitution and made the takeover irreversible, which is worse: an accidental claim then needed a power cycle to undo. Its one virtue — that escaping was easy because the cable carries the power — was solving a problem that should not have existed. |
| **Wi-Fi has priority; USB fills in when Wi-Fi is down** | Zero configuration, and plugging in never steals a working setup. Rejected because it makes "is Wi-Fi working right now" part of who owns the display, so a wired unit beside a reachable broker would be ignored — and because it still substitutes one machine's work for another's, just less often. |
| **`host on` marks liveness; a separate `take` verb transfers ownership** | Keeps zero-config takeover for whoever wants it. Weaker than it looks: a daemon pointed at a serial port always wants the device, so it would always send `take`. Automatic again, with a step. |
| **Infer from configuration rather than storing a setting** — e.g. "no SSID means wired" | Tempting, and it needs no new setting. Rejected because it re-introduces inference through the back door: setting an SSID would then silently change the transport, which is the same class of surprise in a new place. |
| **Default to `wifi`** | Matches the product's headline description. Rejected because a fresh device could then do nothing at all until configured, while the wired path needs no configuration whatsoever — see D120. |
| **A build-time-only choice, no runtime setting** | Simplest possible. Rejected because it makes one image per variant and prevents anyone converting a unit without a toolchain, which is exactly what the console exists to avoid (ADR-0019). The Kconfig value remains as the *default*. |

## Consequences

**Accepted costs**
- One more setting to know about, and one more thing to get wrong. Mitigated by showing it first in
  `show`, listing it in `help`, and warning at the moment an SSID is set on a wired device.
- Converting a device between transports needs `set transport …`, `save` and a reboot. Deliberate: the
  thing that decides what a device is should not change while it is running.
- A wired device ignores a perfectly good Wi-Fi network, and a wireless one ignores a perfectly good
  daemon on the wire. Both are the intended behaviour and both will look like a fault to somebody at
  some point, which is why the boot banner states the configured transport every time.
- Two ADRs superseded in a day. The reasoning in all three is kept rather than rewritten, because the
  path — infer, then latch, then configure — is the useful part of the record.

**Benefits**
- Plugging in a cable changes nothing. That is the property that was actually wanted.
- No silent substitution is possible in either direction, so the lamps can only ever be right about the
  thing they were configured to report, or visibly unsure.
- A fresh device is useful over USB with no configuration at all.
- `transport.c` is materially simpler: no latch, no release window, no `usb_claimed_ms`, no handover
  counting, and 235 host checks instead of 484 for a smaller and clearer contract.
- The Wi-Fi and wired paths no longer interact, so neither can regress the other.

**Revisit if** a single machine ever needs to drive the device over both paths — at which point the
honest fix is host identity in the protocol (noted in ADR-0021) rather than a cleverer selection rule.
