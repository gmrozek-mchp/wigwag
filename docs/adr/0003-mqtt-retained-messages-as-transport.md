# ADR-0003 — State travels as retained MQTT messages

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

Something on the Mac must get a state value to a device on the LAN. The traffic is tiny and
bursty: a handful of small messages per minute while a session is active, nothing at all when
it isn't.

Two properties matter more than throughput:

1. **A status light must be correct immediately after it reconnects or reboots.** Power-cycling
   the device, a Wi-Fi blip, or restarting the daemon must not leave the lamps stale or dark
   until the next state change — which might be minutes away.
2. **It must be trivial for other producers to drive it** (ADR-0004's push API): CI, PR bots,
   cron. That argues for a protocol with a well-known client on every platform, not a bespoke one.

The RNWF02 already implements an MQTT client in module firmware (ADR-0002), so MQTT costs no
host-side code at all.

## Decision

**MQTT to a `mosquitto` broker on the Mac. State is published as a `retain=1` message at QoS 1.**

| Topic | Direction | Payload | Notes |
|---|---|---|---|
| `wigwag/state` | host → device | `{"state":"WAIT","reason":"permission_prompt","sessions":2}` | retained, QoS 1 |
| `wigwag/brightness` | host → device | `0`–`255` | retained |
| `wigwag/button` | device → host | `{"event":"press","ms":120}` | |
| `wigwag/online` | device → host | `1` / `0` | `0` set as the device's MQTT **Last Will** |

The retained message is the crux: the broker replays the current state to the device the instant
it subscribes. Recovery from a reboot or a dropped link requires **no** host participation and no
polling. The Last Will gives the host a truthful liveness signal without a heartbeat protocol.

`reason` is diagnostic only. Consumers switch on `state` and never on `reason`.

v1 authentication is username/password on the LAN. TLS is deferred, not dismissed — the module
supports TLS 1.2 and the chosen part carries Trust&Go (ADR-0002), so it needs no hardware change.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **Raw TCP socket to the daemon** | One less service to install, and the module can do it with socket AT commands. But we would hand-roll reconnect, backoff, framing and liveness — and, critically, **lose retained state**: after any reconnect the light would show nothing until the next change. Reimplementing retention over raw TCP is reimplementing MQTT badly. |
| **HTTP polling** | Trivially debuggable with `curl`, but laggy by construction, chatty for a device that is idle most of the time, and it inverts the flow: the device must ask repeatedly rather than be told once. |
| **HTTP server on the device** | Removes the broker, but then the host must discover the device's IP, and a sleeping/reconnecting device becomes unreachable. Pushes the hard problem onto the host. |
| **UDP broadcast** | Appealingly stateless and needs no broker, but unreliable delivery for the *one* message that matters most (the transition to `WAIT`), and again no state recovery on reboot. |
| **MQTT to a cloud broker** | Would work from anywhere, but adds an internet dependency, credentials, and latency to a device sitting two metres from the publisher. Also sends session-activity metadata off the machine for no benefit. |
| **Bluetooth LE GATT** | No broker, low power. Rejected with the radio choice in ADR-0002. |

## Consequences

**Accepted costs**
- A broker must be installed and running (`brew install mosquitto`). This is the one real
  operational dependency, and it is also the single point of failure for the whole path —
  mitigated by ADR-0007's fail-visible behaviour rather than hidden.
- The daemon must coalesce heartbeat bursts (`PreToolUse`/`PostToolUse` fire rapidly) so we
  don't publish dozens of identical retained messages per second.
- Retained messages are sticky: a bad publish persists until overwritten. The daemon must always
  publish a well-formed state, and `wigwag set` must be able to correct one by hand.

**Benefits**
- Correct state immediately on reconnect or power-up, with zero host involvement.
- Truthful liveness via Last Will, no heartbeat protocol to design.
- The generic push API is free — anything that can publish MQTT can drive the light.
- Zero host-side networking code: the module's firmware is the MQTT client.
- Inspectable end to end with `mosquitto_sub -t 'wigwag/#' -v`, which is also the Phase 1
  verification method.

**Revisit if** the broker dependency becomes annoying in practice (a small embedded broker inside
`wigwagd` would remove it while keeping the protocol), or when TLS moves from deferred to
required.
