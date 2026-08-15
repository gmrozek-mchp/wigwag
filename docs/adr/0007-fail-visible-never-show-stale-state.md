# ADR-0007 — Fail-visible: never display a state we cannot confirm

- **Status:** Accepted
- **Date:** 2026-08-14
- **Note added 2026-08-15:** the decision here stands unchanged, but this document calls the
  fail-visible pattern an *amber flicker*, and **there is no amber lamp** — the device has three
  discrete red, yellow and green lamps, which cannot blend a colour. The pattern is now a red/yellow
  **wigwag** at 1 Hz, the railroad crossing signal this project is named after (D121). The wording
  below is left as written rather than edited, per Rule 2.

## Context

The device's whole value is that it can be trusted at a glance. You look up, see green, and go
make coffee without checking the terminal.

That trust is asymmetric and fragile. A light that is *usually* right and occasionally silently
wrong is worse than no light at all, because it trains you to rely on it and then betrays that
reliance at the worst moment — you see green and walk away while a permission prompt sits
unanswered.

Plenty of ordinary failures produce exactly that: Wi-Fi drops, the broker stops, the daemon is
killed, the Mac sleeps, the retained message goes stale. In every one, the device keeps happily
displaying its last known state, which is now a lie.

## Decision

**When the device cannot confirm the current state, it must look obviously wrong rather than
plausible.**

Concretely:

- The device tracks a **link condition**, separate from state: `LINKED` (broker reachable) or
  `UNLINKED`.
- **More than 10 seconds without the broker → `UNLINKED`**, which overrides the lamps entirely
  with an **amber flicker** — a pattern that appears nowhere in normal operation and cannot be
  mistaken for `IDLE`, `BUSY`, `WAIT` or `ERROR`.
- Recovery is automatic and requires no host action: on reconnect, the broker's retained message
  (ADR-0003) immediately restores the true state.
- `UNKNOWN` is deliberately **not** a state (see `CONTEXT.md`). Conflating "I don't know" with
  the four real states would let it be aggregated, published and reasoned about as if it were
  information.
- The device publishes `wigwag/online = 0` as its MQTT **Last Will**, so the failure is visible
  from the host side too, not just on the lamps.

The same principle governs the host: `wigwag status` reports the link condition, and never
presents a last-known value as if it were current.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **Hold the last known state on link loss** | The obvious, comfortable default, and what most indicators do. Rejected precisely because it is the failure mode the device exists to avoid: a confident green while a session waits. The whole ADR is about refusing this. |
| **Go dark (all lamps off)** | Honest and unmistakable, but indistinguishable from "unplugged", "firmware crashed" or "no session running". `IDLE` is already quiet; darkness carries no information and invites the assumption that nothing is wrong. |
| **Solid amber** | Simple to implement, but a steady lamp reads as a *state*, and amber sits visually between yellow and red. Flicker is deliberately unlike any real animation — it reads as malfunction, which is exactly the message. |
| **Blink the last known state** | Preserves some information while signalling doubt. Rejected as too subtle: at a glance across a room, a blinking green still reads "green", and the point is to stop being trusted. |
| **Shorter timeout (1–2 s)** | Faster honesty, but ordinary Wi-Fi jitter and broker hiccups would flicker the light constantly, which trains you to ignore the warning. 10 s is well beyond normal jitter and still fast enough to catch you before you walk away. |
| **Longer timeout (60 s+)** | Fewer false alarms, but a full minute of confident lying is exactly the window in which someone reads the light and leaves. |
| **Host-side watchdog pinging the device** | Would detect failure from the other end, but cannot help when the *host* is the thing that died — and that is a common case (daemon killed, Mac asleep). The device must be able to distrust its own information locally. |

## Consequences

**Accepted costs**
- A link supervisor in the firmware (`firmware/src/link.c`) plus a flicker animation — small, but
  real code and RAM against the 8 KB budget (ADR-0008). Merging it into the lamp thread is the
  planned economy if stacks get tight.
- Users will occasionally see the flicker for benign reasons (broker restart, router reboot).
  That is the intended trade: a visible false alarm beats an invisible false negative.
- The flicker must be genuinely distinguishable from `ERROR` (fast red/yellow alternate). Both
  are "something is wrong", but they mean different things, so the patterns must be tuned to be
  clearly different in practice — a Phase 5 task, not something to declare done on paper.

**Benefits**
- The light can be trusted, which is the only property that matters.
- Failures are loud and self-announcing instead of silent.
- Recovery is automatic and needs no host involvement, thanks to retained messages.
- It is directly testable: kill the broker, watch for flicker within 10 s; restore it, watch the
  true state return. That is in the Phase 5 verification list.

**Revisit if** the 10 s threshold proves too twitchy or too slow in real use — tune the number,
never the principle.
