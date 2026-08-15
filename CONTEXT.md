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
| `ERROR` | the turn died (API error, rate limit) | red + yellow | **both steady** — the only state lighting two lamps at once |

`UNKNOWN` is deliberately *not* a state. It is a **link condition** — see below — and it is shown by
the **wigwag**: red and yellow alternating at 1 Hz, one at a time. That is the railroad crossing signal
this device is named after, and it is deliberately unlike all four states above: `ERROR` uses the same
two lamps but holds them both steady, so the two cannot be confused.

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
  the lamps with the red/yellow wigwag.
- **fail-visible** — the governing principle: when the device cannot know the state, it must
  *look* wrong rather than display a stale state confidently. See ADR-0007.
- **producer** — anything that reports state. Today: Claude Code hooks and the CLI push API.
  The daemon cannot tell producers apart; they all speak the same wire protocol.
- **hook client** — `host/hooks/wg-notify`, the ~3 ms shell script Claude Code executes. Uses
  bash's `/dev/udp`; no `jq`, `sed`, `nc`, Python or Node in the hook path (ADR-0010).
- **daemon** — `wigwagd`. Owns the session table and is the only MQTT publisher of state.
- **push API** — the generic path (`wigwag set …`) letting non-Claude producers drive the light.
- **coalescing** — suppressing a publish when the aggregate has not meaningfully changed. Keyed
  on `state` and session count, deliberately **not** on `reason`, since `PreToolUse` and
  `PostToolUse` differ only in reason and would otherwise republish twice per tool call.
- **footprint budget** — the 8 KB SRAM ceiling on the target part. A gated requirement measured
  with `west build -t ram_report`, not an aspiration. See ADR-0008.
- **commissioning** — getting a device its Wi-Fi credentials *and* its broker configuration for
  the first time. Two separate problems: the module's provisioning service handles Wi-Fi, nothing
  yet handles the broker. v1 is compile-time; v1.1 is SoftAP provisioning. See ADR-0012.
- **provisioning mode** — the temporary state entered by long-pressing the button, in which the
  module runs as a Soft-AP and serves its provisioning service. Signalled by all three lamps
  cycling in sequence — a pattern used nowhere else, so it cannot be mistaken for `WAIT` or the
  link-lost wigwag. Not a `state` and not a `link condition`; a distinct operating mode.

## Wire protocol

| Topic | Direction | Payload | Notes |
|---|---|---|---|
| `wigwag/state` | host → device | `{"state":"WAIT","reason":"permission_prompt","sessions":2}` | retained, QoS 1 |
| `wigwag/brightness` | host → device | `0`–`255` | retained |
| `wigwag/button` | device → host | `{"event":"press","ms":120}` | |
| `wigwag/online` | device → host | `1` / `0` | `0` published as MQTT Last Will |

`reason` is free-form diagnostic text (usually the hook that caused the change). Never switch
behavior on `reason`; only on `state`.

The device also publishes nothing about the host; `wigwag/host_online` (`1`/`0`, retained, `0`
as the daemon's Last Will) is the host's own liveness marker.

### Daemon → device, over the wire (USB serial)

The same four states, a different carrier. One transport is active at a time, and **the device decides
which by a stored setting** — `set transport usb|wifi` — not by anything either side detects
(ADR-0022, D119). Lines on the console UART, `\r\n` terminated, the same stream the device prints
diagnostics on: device→host is human-readable output, host→device is commands, so the two directions
never need framing.

| Line | Meaning |
|---|---|
| `state IDLE\|BUSY\|WAIT\|ERROR` | what to display. Bare word, not JSON |
| `host on` | **the daemon is alive. Must be repeated within 10 s** |
| `host off` | orderly goodbye; the device stops trusting us at once |
| `brightness 0`–`255` | as the retained topic |
| `echo off` | stop echoing input, which a program does not want |

**Why `host on` has to repeat, when `wigwag/host_online` does not.** Over MQTT the daemon publishes
that topic once, retained, and registers `0` as its Last Will: the broker holds the value for a late
subscriber and announces the death on the daemon's behalf. A serial line has neither — nothing
retains, and nothing notices a daemon that stops. (The bridge's `USBCFG` pin would not have closed the
gap either, and is not fitted — D116: a computer whose daemon has crashed still enumerates perfectly
happily.) So the wired path is the one place the device demands *periodic* evidence, on the same 10 s
budget D34 sets for the broker.

**Which transport owns the lamps is a device setting**, not something either side negotiates
(`set transport usb|wifi`, ADR-0022). A wired device ignores Wi-Fi entirely and a wireless one ignores
this protocol entirely — the console still works for configuration and diagnostics either way. That is
deliberate: the two carriers do not report the same thing, since a daemon aggregates *its own machine's*
sessions, so substituting one for the other would answer a different question rather than recover. A
configured transport that is not working shows the fail-visible pattern instead.

The daemon sends this from its existing 2 s loop, so five beats fit inside the device's window
(`SerialPublisher`, ADR-0020). Set `WIGWAG_SERIAL_PORT` — or `serial.port` in the config file — and the
daemon uses the wire instead of a broker; `auto` discovers the MCP2221A by its factory USB identity
(04d8:00dd). `host off` is sent on a clean shutdown, which the device honours immediately rather than
waiting out its timeout.

### Hook → daemon protocol

Between the hook client and the daemon, over **loopback UDP** (default `127.0.0.1:9410`) —
not a Unix socket, because AF_UNIX datagrams do not exist on Windows and bash's `/dev/udp`
cannot address them (ADR-0010). One line per datagram, so `printf` can produce it:

```
SET <STATE> <session_id> [reason]
DROP <session_id>
PING
```

Parsing is **total**: malformed input becomes a logged-and-dropped value, never an exception.
The sender is a hook that cannot see our errors and must not be affected by them.

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
