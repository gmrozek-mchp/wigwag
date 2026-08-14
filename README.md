# wigwag

A physical stoplight for your desk that tells you, at a glance, whether your AI coding session
needs you.

> A *wigwag* is the classic railroad grade-crossing signal — a swinging red lamp.

| Lamp | Meaning | Behavior |
|---|---|---|
| 🟢 green | idle / ready for input | steady, dim |
| 🟡 yellow | thinking / working | breathing ~0.8 Hz |
| 🔴 red | **waiting on you** (permission, input) | steady, then slow blink after 30 s |
| 🟠 amber flicker | link lost — state unknown | deliberately distinct |

Wi-Fi connected, so it can sit anywhere on the desk or a shelf. Microchip silicon
(PIC32CM PL10 + RNWF02) running Zephyr RTOS, in a 3D-printed enclosure.

## How it works

```
Claude Code hooks → wigwag CLI → wigwagd → mosquitto → Wi-Fi → RNWF02 → PIC32CM PL10 → lamps
```

Claude Code fires hooks on every meaningful event. A tiny shell client forwards them to a local
daemon, which tracks each session separately and aggregates them by urgency
(`ERROR > WAIT > BUSY > IDLE`, with a TTL so a crashed session can't wedge the light). The
daemon publishes a **retained** MQTT message, so the light shows the correct state the instant
it powers on or reconnects — no host involvement needed.

Because the transport is just MQTT, anything can drive the light: CI failures, long builds, PR
review requests, cron jobs. See `wigwag set --help`.

Works identically whether you run Claude Code in the VS Code extension or a terminal — hooks
are a CLI-level feature and the extension bundles the CLI.

## Getting started

The host software works today, with no hardware. macOS, Linux and Windows.

```sh
# 1. install uv:  brew install uv  |  curl -LsSf https://astral.sh/uv/install.sh | sh
#                 |  winget install astral-sh.uv
cd host
uv sync                          # one build step
uv run pytest                    # 93 tests, no broker needed

uv run wigwagd --dry-run -v      # terminal 1: logs what it would publish
uv run wigwag set WAIT           # terminal 2: drive it by hand
uv run wigwag status
```

Then add a broker and wire up the Claude Code hooks so your sessions drive it
automatically. **[host/README.md](host/README.md) is the full runbook** — broker install
and service setup for all three platforms, connecting to a remote broker, running the
daemon at login, and troubleshooting.

> One trap worth knowing before you start: a default `mosquitto` install accepts
> connections **only from the same machine**, which works for testing the host software
> and silently breaks the device. See
> [the local-only trap](host/README.md#-the-local-only-trap).

For the firmware side — Zephyr workspace, SDK, building and flashing a PL10 Curiosity Nano, and
the 8 KB footprint numbers — see **[firmware/README.md](firmware/README.md)**.

## Two things it deliberately gets right

**It never lies.** If the device loses the broker for more than 10 seconds, it stops showing the
last known state and switches to an obviously-wrong amber flicker. A status light that is
confidently stale is worse than one that admits ignorance.

**It never breaks the tool it observes.** The hook client always exits 0, writes nothing to
stdout, works with the daemon down, and runs in under 10 ms.

## An 8 KB experiment

The firmware deliberately targets the *smallest* Zephyr-supported Microchip part —
`PIC32CM6408PL10028`, 28 pins, 64 KB flash, **8 KB SRAM** — to find out how small a part can
usefully run Zephyr. Footprint is a gated requirement measured with `west build -t ram_report`
at every milestone, not an aspiration. See [ADR-0008](docs/adr/).

The same die is available as **SPDIP-28 through-hole**, so the whole prototype can be
breadboarded in a socket with no SMT at all.

## Layout

| Path | Contents |
|---|---|
| `host/` | daemon, CLI, Claude Code hook client |
| `firmware/` | Zephyr application, board overlays, RNWF02 AT simulator |
| `hardware/` | KiCad project, BOM, fab outputs |
| `enclosure/` | parametric OpenSCAD model |
| `docs/adr/` | architecture decision records |
| `docs/PLAN.md` | the implementation plan |
| `JOURNAL.md` | append-only development log |

## Status

Early development — Phase 0 (documentation and workflow). See [JOURNAL.md](JOURNAL.md) for
current state and [docs/adr/](docs/adr/) for decisions made so far and why.

## Documentation conventions

- **[JOURNAL.md](JOURNAL.md)** — append-only chronological devlog, newest first: what changed,
  why, what failed, what's next.
- **[docs/adr/](docs/adr/)** — one durable decision per numbered file.
- **[CONTEXT.md](CONTEXT.md)** — domain vocabulary. Use these exact terms.
- **[CLAUDE.md](CLAUDE.md)** — standing instructions for AI-assisted sessions.
