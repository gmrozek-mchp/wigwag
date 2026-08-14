# ADR-0004 — Session aggregation happens on the host, by priority, with a TTL

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

Several Claude Code sessions commonly run at once. Each fires its own hooks, tagged with its own
`session_id`. There is one light and three lamps.

So something must reduce N session states to one displayed state, and it has to answer the
question the light exists to answer: **"is any session waiting on me right now?"** A session
happily working must never mask one that is blocked.

Two failure modes need designing out:

- **Lost updates.** A crashed or `kill -9`'d session never fires `SessionEnd`, so its last
  reported state would otherwise persist forever. If that state was `WAIT`, the light stays red
  permanently and becomes worthless.
- **Flapping.** Tool-use hooks fire in rapid bursts, which would produce a stream of identical
  publishes.

## Decision

**`wigwagd` owns a session table keyed by `session_id`** and computes the displayed state by
**priority**:

```
ERROR > WAIT > BUSY > IDLE
```

The most urgent *live* session wins. Every entry carries a monotonic timestamp, refreshed on
each report. **Entries with no update for 15 minutes expire** and stop voting; expiry is not a
state change, it is a session leaving the electorate. `SessionEnd` removes an entry immediately;
the TTL exists only for sessions that die without saying so.

Heartbeat reports (repeated `BUSY` from `PreToolUse`/`PostToolUse`) refresh the TTL without
changing state, and the daemon coalesces them so identical states are not republished.

The device receives only the aggregate. It has no concept of a session, and needs no memory of
one — which matters at 8 KB (ADR-0001).

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **Aggregate on the device** | Would need a session table, timers and expiry logic in 8 KB of SRAM, plus a richer wire protocol carrying per-session identity. All of it is free on the host. Directly at odds with ADR-0008. |
| **Last-write-wins, no aggregation** | Much simpler, and correct with exactly one session. With two, a busy session's `BUSY` overwrites a blocked session's `WAIT` and the light actively lies about the thing it exists to report. Unacceptable. |
| **One lamp per session** | Honest and uses the hardware, but caps at three sessions and turns a glanceable signal into a display needing interpretation. The question is "does anything need me", not "what is each session doing". |
| **No TTL, rely on `SessionEnd`** | Correct only if sessions always exit cleanly. They don't — crashes, `kill -9`, closed laptops. One dead session stuck in `WAIT` permanently ruins the device. |
| **Very short TTL (~60 s)** | Self-heals faster, but a legitimately long-running tool call (a build, a big test suite) would expire mid-work and drop the light to `IDLE` while Claude is still going — again, lying. 15 min is longer than any plausible single tool call but short enough to recover unattended. |
| **Sum/blend states across sessions** | e.g. brightness proportional to session count. Cute, ambiguous, and unreadable at a glance. |

## Consequences

**Accepted costs**
- The daemon is stateful, so it needs the session table to be correct across its own restarts.
  Losing it is self-healing: the next hook from each live session re-registers it.
- 15 minutes is a guess. If a real tool call ever exceeds it, the light will briefly go `IDLE`
  mid-work. The heartbeat design makes that unlikely, since every tool call refreshes the TTL.
- `sessions` is published in the payload for diagnostics, which tempts consumers to behave
  differently based on it. They should not; only `state` is behavioural.

**Benefits**
- The urgent case can never be masked by a busy one, which is the entire point of the device.
- The device stays stateless and tiny.
- A crashed session cannot wedge the light; the failure is bounded at 15 minutes.
- Aggregation is pure, synchronous logic on the host — trivially unit-testable, and Phase 1
  tests it directly (priority ordering and TTL expiry).

**Revisit if** per-session visibility turns out to be genuinely wanted (a second device, or an
addressable-LED strip, would be the honest way to provide it rather than overloading three lamps).
