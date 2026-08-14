# CONTEXT — domain vocabulary

The words below are the project's ubiquitous language. Code, commit messages, journal
entries, MQTT payloads, and conversation should all use these exact terms. If a better term
emerges, rename it *everywhere* and note the rename in `JOURNAL.md`.

The project is **wigwag** — after the railroad grade-crossing signal with a swinging red lamp.
Named vendor-neutrally on purpose: Claude Code is the only producer today, but others may be
added as producers later without renaming anything.

## States

There are exactly four states. They are named in code and on the wire in **uppercase**.

| State | Question it answers | Lamp | Animation |
|---|---|---|---|
| `IDLE` | it's done and ready for you | green | steady, dim |
| `BUSY` | it's working; no action needed | yellow | breathing ~0.8 Hz |
| `WAIT` | **it's blocked on you** | red | steady → slow blink after 30 s |
| `ERROR` | the turn died (API error, rate limit) | red + yellow | fast alternate |

`UNKNOWN` is deliberately *not* a state. It is a **link condition** — see below.

## Terms

- **lamp** — one of the three physical light positions (green, yellow, red). A lamp is one
  10 mm diffused LED plus its series resistor and low-side FET. Never call a lamp an "LED";
  the lamp is the whole channel.
- **state** — one of the four values above. Always the *aggregate* across all sessions unless
  explicitly qualified as a *session state*.
- **session state** — the state of one individual session, keyed by `session_id`.
- **aggregation** — reducing many session states to one displayed state by priority:
  `ERROR > WAIT > BUSY > IDLE`. The most urgent live session wins.
- **TTL / expiry** — a session state older than 15 min is dropped, so a crashed session cannot
  wedge the light. Expiry is not a state change; the session simply stops voting.
- **heartbeat** — a repeated `BUSY` report (from tool-use hooks) whose purpose is refreshing the
  TTL, not changing the state.
- **link condition** — whether the device currently trusts what it is displaying.
  `LINKED` (broker reachable) or `UNLINKED` (> 10 s without the broker). `UNLINKED` overrides
  the lamps with the amber flicker.
- **fail-visible** — the governing principle: when the device cannot know the state, it must
  *look* wrong rather than display a stale state confidently. See ADR-0007.
- **producer** — anything that reports state. Today: Claude Code hooks, and the CLI push API.
- **hook client** — `host/hooks/wg-notify`, the sub-10 ms script Claude Code executes.
- **daemon** — `wigwagd`. Owns the session table and is the only MQTT publisher of state.
- **push API** — the generic path (`wigwag set …`) letting non-Claude producers drive the light.
- **footprint budget** — the 8 KB SRAM ceiling on the target part. A gated requirement measured
  with `west build -t ram_report`, not an aspiration. See ADR-0008.

## Wire protocol

| Topic | Direction | Payload | Notes |
|---|---|---|---|
| `wigwag/state` | host → device | `{"state":"WAIT","reason":"permission_prompt","sessions":2}` | retained, QoS 1 |
| `wigwag/brightness` | host → device | `0`–`255` | retained |
| `wigwag/button` | device → host | `{"event":"press","ms":120}` | |
| `wigwag/online` | device → host | `1` / `0` | `0` published as MQTT Last Will |

`reason` is free-form diagnostic text (usually the hook that caused the change). Never switch
behavior on `reason`; only on `state`.

## Hardware names

- **host MCU** — PIC32CM PL10. Target part `PIC32CM6408PL10028` (SSOP-28, 64 KB flash / 8 KB
  SRAM); `-I/SP` SPDIP-28 is the socketable bring-up variant. Chosen as the smallest
  Zephyr-supported Microchip part (ADR-0001, ADR-0008).
- **dev board** — `EV10P22A`, PL10 Curiosity Nano. Carries `PIC32CM6408PL10048` with *identical*
  64 KB/8 KB memory, so footprint measurements transfer directly to the target.
- **module** — the RNWF02 Wi-Fi module (`RNWF02PC-I/100`). It owns Wi-Fi, TCP/IP, TLS, and the
  MQTT client; the host MCU has no network stack. Driven by ASCII AT commands over UART.
- **AT client** — `firmware/src/rnwf_at.c`, the host-side driver for the module.
- **keepout** — the RF exclusion zone around the module's PCB antenna: ≥10 mm to plastic,
  ≥31.75 mm to metal, module at the board edge with ground-plane edges aligned. Binding on both
  the PCB layout and the enclosure (ADR-0002).
- **lamp rail** — the 5 V USB rail that feeds the LED anodes. Distinct from the **3.3 V rail**
  that feeds the MCU and module. Green LEDs cannot run from 3.3 V (ADR-0009).
