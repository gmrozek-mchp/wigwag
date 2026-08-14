# ADR-0011 — The broker is configurable, and TLS defaults on for any non-loopback host

- **Status:** Accepted
- **Date:** 2026-08-14

Refines ADR-0003, which chose MQTT with retained messages and deferred TLS. That
deferral assumed a broker on `localhost`. Supporting a remote broker changes the
security picture, so this ADR supersedes the "TLS deferred" position — the transport
decision in ADR-0003 stands unchanged.

## Context

ADR-0003 chose MQTT and assumed `mosquitto` on the same machine. Two reasons to widen that:

1. **A shared or hosted broker is genuinely useful.** One broker can serve several
   machines, so the light reflects whichever machine you are working on; or it can live
   on a VPS or a managed service so the light works from anywhere.
2. **The broker was the single point of failure.** Requiring it to be local means the
   light only works while that one machine is up and configured.

But the moment the broker is not on loopback, the traffic leaves the machine — and that
traffic is MQTT credentials plus a running commentary on when you are at your desk and
what your tools are doing. On loopback, plaintext is fine. Off it, plaintext is a
mistake that is very easy to make by accident: change one line of config, and everything
still appears to work.

## Decision

**Every broker parameter is configurable, and TLS is inferred from the host unless set
explicitly.**

Configuration is layered — defaults, then a TOML file, then environment variables — so
the zero-config case is a working local setup and no file is needed to start:

| Platform | Config path |
|---|---|
| macOS / Linux | `$XDG_CONFIG_HOME/wigwag/config.toml`, else `~/.config/wigwag/config.toml` |
| Windows | `%APPDATA%\wigwag\config.toml` |
| Any | `$WIGWAG_CONFIG` |

**The TLS policy:**

```
tls unset + loopback host      → TLS off, port 1883
tls unset + non-loopback host  → TLS ON,  port 8883
tls set explicitly             → honoured, and warned about if weak
```

Unresolvable or non-numeric hosts are treated as **remote**, so the ambiguous case fails
toward encryption rather than away from it.

Explicit weakening is permitted but never silent. `BrokerConfig.warnings()` is surfaced by
both the daemon at start-up and `wigwag config`:

- plaintext to a remote broker → *"credentials and session activity will cross the network
  in the clear"*
- `insecure_skip_verify` → *"TLS is encrypting but not authenticating the broker, so this
  does not protect against interception"*
- remote broker with no username → flagged

This is Rule 4 (fail-visible) applied to configuration: an insecure setup must look
insecure rather than merely work.

**Secrets stay out of the config file.** `WIGWAG_MQTT_PASSWORD` is the documented path;
the file supports `password` but the example config says plainly that anything written
there is a plaintext secret on disk. `firmware/credentials.conf` and `host/.env` are
already gitignored.

Also settled here: `topic_prefix` is configurable (so two independent lights can share one
broker) and validated to reject MQTT wildcards `+`/`#`, and `client_id` is configurable
because two daemons sharing an id will fight over the connection in a reconnect loop.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **Local broker only** (the ADR-0003 assumption) | Simplest, and TLS never becomes an issue. Rejected: it forces a broker on every machine and makes that machine a single point of failure for the light. |
| **TLS always required** | Unambiguous and maximally safe. Rejected because it makes the common local case need certificates, which is friction with no benefit — loopback traffic does not leave the machine. |
| **TLS never automatic; make the user opt in** | Honest and explicit. Rejected because the failure mode is silent and severe: point at a remote broker, forget the flag, and it keeps working while leaking. Defaults should be safe when someone forgets. |
| **Refuse to run plaintext against a remote broker** | Would guarantee no accidental leak. Rejected as overreach — a broker on a trusted VLAN or inside a WireGuard tunnel is a legitimate setup, and a tool should not veto its operator. Warn loudly, then obey. |
| **Infer TLS from the port (8883 = TLS)** | Conventional and needs no host analysis. Rejected as too clever: someone running TLS on a non-standard port silently gets plaintext. The host is what determines whether traffic leaves the machine. |
| **Environment variables only, no config file** | Fewer moving parts, no file format. Rejected: broker host, port, TLS, CA path, username and topic prefix is too much to keep in a shell profile, and a file documents itself. Env still overrides, which is what CI and secrets need. |
| **JSON or INI config** | Works on every Python version. Rejected in favour of TOML: comments matter for a config file a human edits occasionally, and `tomllib` is stdlib from 3.11, which we already require. |

## Consequences

**Accepted costs**
- Requires Python 3.11+ for `tomllib`. Acceptable in 2026 and declared in
  `pyproject.toml`; `uv` will fetch a suitable interpreter regardless of the system one.
- More configuration surface to document and test. Mitigated by 20 config tests covering
  the TLS inference table, the warnings, and validation.
- The TLS inference could surprise someone who expects plaintext on a LAN broker. It is in
  the example config and in `wigwag config` output, and the fix is one explicit line.
- Certificate handling for a self-signed broker is still manual (`ca_cert`, or
  `insecure_skip_verify` with a warning).

**Benefits**
- Local, LAN, VPS and managed brokers are all supported by configuration alone.
- The insecure case is impossible to reach *silently*.
- Zero-config still works: no file needed for the local setup.
- `topic_prefix` allows independent lights on a shared broker.
- Passwords have a documented home outside the repo and outside the config file.

**Revisit if** device-side TLS lands — the RNWF02 has TLS 1.2 and the chosen module
variant carries Trust&Go (ADR-0002), so mutual TLS with a hardware root of trust is
possible without a board respin, and that would deserve its own ADR.
