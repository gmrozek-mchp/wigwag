# wigwag

A physical stoplight for your desk that tells you, at a glance, whether your AI coding session
needs you.

> A *wigwag* is the classic railroad grade-crossing signal — a swinging red lamp.

| Lamp | Meaning | Behavior |
|---|---|---|
| 🟢 green | idle / ready for input | steady, dim |
| 🟡 yellow | thinking / working | breathing ~0.8 Hz |
| 🔴 red | **waiting on you** (permission, input) | steady, then slow blink after 30 s |
| 🔴🟡 red/yellow wigwag | link lost — state unknown | alternating at 1 Hz, one at a time |

**Plug it in over USB and it works** — no network, no broker, no configuration. Set it to Wi-Fi
instead and it can sit anywhere on the desk or a shelf. Microchip silicon (PIC32CM PL10 +
RNWF02) running Zephyr RTOS, in a 3D-printed enclosure.

## How it works

```
Claude Code hooks → wg-notify → wigwagd ──USB serial────────────────────────► PIC32CM PL10 → lamps
                                        └─MQTT─► mosquitto ──Wi-Fi─► RNWF02 ─┘
```

Claude Code fires hooks on every meaningful event. A tiny shell client forwards them to a local
daemon, which tracks each session separately and aggregates them by urgency
(`ERROR > WAIT > BUSY > IDLE`, with a TTL so a crashed session can't wedge the light).

**One transport carries the state, and the device decides which by a stored setting** —
`set transport usb|wifi` — never by anything either side detects (ADR-0022). The default is
`usb`, because a wired device needs no configuration at all: plug it into the machine you code
on, run the daemon, and the lamps are live. Over Wi-Fi the daemon publishes a **retained** MQTT
message instead, so the light shows the correct state the instant it powers on or reconnects,
with no host involvement.

The two carriers deliberately don't fall back to each other. A daemon reports *its own machine's*
sessions, so answering with the other transport's data would answer a different question — a
configured transport that isn't working wigwags instead.

Whichever carrier is in use, anything can drive the light: CI failures, long builds, PR review
requests, cron jobs. See `wigwag set --help`.

Works identically whether you run Claude Code in the VS Code extension or a terminal — hooks
are a CLI-level feature and the extension bundles the CLI.

## Getting started

The host software works today, with no hardware. macOS, Linux and Windows.

```sh
# 1. install uv:  brew install uv  |  curl -LsSf https://astral.sh/uv/install.sh | sh
#                 |  winget install astral-sh.uv
cd host
uv sync                          # one build step
uv run pytest                    # 110 tests, no broker needed

uv run wigwagd --dry-run -v      # terminal 1: logs what it would publish
uv run wigwag set WAIT           # terminal 2: drive it by hand
uv run wigwag status
```

With a device on the end of a USB cable, that's the whole setup — no broker involved:

```sh
uv sync --extra serial                     # adds pyserial
WIGWAG_SERIAL_PORT=auto uv run wigwagd -v  # or /dev/cu.usbmodem…, or COM5
```

`auto` finds the MCP2221A bridge by its factory USB identity and refuses to guess if it sees
none or several.

Then wire up the Claude Code hooks so your sessions drive it automatically — and add a broker
if you want the wireless option. **[host/README.md](host/README.md) is the full runbook** —
the wired transport, broker install and service setup for all three platforms, connecting to a
remote broker, running the daemon at login, and troubleshooting.

> One trap worth knowing before you start on Wi-Fi: a default `mosquitto` install accepts
> connections **only from the same machine**, which works for testing the host software
> and silently breaks the device. See
> [the local-only trap](host/README.md#-the-local-only-trap). The wired transport has no
> broker and so no such trap.

For the firmware side — Zephyr workspace, SDK, building and flashing a PL10 Curiosity Nano, the
console reference and the 8 KB footprint numbers — see **[firmware/README.md](firmware/README.md)**.

## Configuring a device

There is no phone app and no config file on the device. Everything is set over the same serial
console the device prints diagnostics on (ADR-0019), and stored in flash so it survives a reboot:

```
set transport wifi
set ssid MyNetwork
set pass correct horse battery staple
set broker mqtt.example.lan
test wifi        # try it without committing: names the step that failed
save
reboot
```

`test wifi` is the part worth knowing about. It runs the real connect script on a still-wired
device and names the step it reaches, so a wrong passphrase (*associate and get an IP*), a wrong
broker (*resolve, connect and CONNACK*) and a miswired module (*module responding*) stop looking
like the same failure.

## Two things it deliberately gets right

**It never lies.** If the device loses its configured transport for more than 10 seconds — the
broker over Wi-Fi, or the daemon's heartbeat over the wire — it stops showing the last known state
and wigwags red/yellow, which no working state resembles. It doesn't quietly switch to the other
transport either, since that would be a different machine's work. A status light that is
confidently stale is worse than one that admits ignorance.

**It never breaks the tool it observes.** The hook client always exits 0, writes nothing to
stdout, works with the daemon down, and runs in under 10 ms.

## An 8 KB experiment

The firmware deliberately targets the *smallest* Zephyr-supported Microchip part —
`PIC32CM6408PL10028`, 28 pins, 64 KB flash, **8 KB SRAM** — to find out how small a part can
usefully run Zephyr. Footprint is a gated requirement measured with `west build -t ram_report`
at every milestone, not an aspiration. See [ADR-0008](docs/adr/).

Where it stands: **5 267 B of 8 KB (64.3 %)** and 35 516 B of flash, for three lamps, a button,
the AT client, link supervision, a watchdog, a flash driver, settings in NVS and an interactive
console. The answer so far is that the budget is dominated by kernel stacks and tunables rather
than by application code.

The same die is available as **SPDIP-28 through-hole**, so the whole prototype can be
breadboarded in a socket with no SMT at all.

## Layout

| Path | Contents |
|---|---|
| `host/` | daemon, CLI, Claude Code hook client, deployment units |
| `firmware/` | Zephyr application, board overlay, out-of-tree flash driver, RNWF02 AT simulator, host unit tests |
| `docs/adr/` | architecture decision records |
| `docs/PLAN.md` | the implementation plan and its numbered decision register |
| `JOURNAL.md` | append-only development log |
| `enclosure/` | parametric OpenSCAD model — Phase 4, not started |

The KiCad project lands in `hardware/` in Phase 3.

## Status

**Host software and firmware both run on real hardware — a PL10 Curiosity Nano — driven by real
Claude Code sessions over both transports.** Phase 1 (host) and Phase 2 (firmware) are complete bar
one item: bring-up against the real RNWF02 module, which is waiting on an `EV72E72A` add-on board.
The Wi-Fi path so far talks to `firmware/sim/fake_rnwf02.py` — a real AT server bridged to a real
broker over a real UART, but not the module itself (ADR-0015).

Verified: 110 host tests, 11 144 firmware checks, and hook → daemon → lamps end to end on both the
wire and (simulated) Wi-Fi. Next up is Phase 3, the PCB.

See [JOURNAL.md](JOURNAL.md) for where things actually stand, [docs/PLAN.md](docs/PLAN.md) for the
plan and its decision register, and [docs/adr/](docs/adr/) for the 22 decisions made so far and why.

## Documentation conventions

- **[JOURNAL.md](JOURNAL.md)** — append-only chronological devlog, newest first: what changed,
  why, what failed, what's next.
- **[docs/adr/](docs/adr/)** — one durable decision per numbered file. Accepted ADRs are
  superseded, never edited to change a decision.
- **[docs/PLAN.md](docs/PLAN.md)** — the plan, plus a numbered decision register (`D01`…) that the
  journal and the ADRs both cite.
- **[CONTEXT.md](CONTEXT.md)** — domain vocabulary. Use these exact terms.
- **[CLAUDE.md](CLAUDE.md)** — standing instructions for AI-assisted sessions.
