# ADR-0020 — `pyserial` on every platform for the wired transport, imported lazily

- **Status:** Accepted
- **Date:** 2026-08-15

## Context

D104 and D111 built the device half of the wired transport: the firmware accepts `state BUSY` and
`host on` on its console UART and trusts that path while a host keeps talking. Nothing on the host
side speaks it. The daemon publishes only over MQTT, so the feature is complete on the device and
unusable end to end.

The daemon's dependency policy is deliberate and written down in `host/pyproject.toml`: *"Runtime
dependencies are deliberately minimal … Everything except MQTT publishing is stdlib, so the test suite
and the pure logic run with nothing installed."* `paho-mqtt` is imported lazily inside `MqttPublisher`
to preserve exactly that (D31), and `CLAUDE.md` requires an ADR before adding a runtime dependency.

Python has no serial support in the standard library. `termios` can drive a POSIX tty directly, and on
Windows there is nothing equivalent — `msvcrt` and the `winreg`/`ctypes` route means calling
`CreateFile`, `SetCommState` and `SetCommTimeouts` by hand. ADR-0010 makes the host cross-platform on
purpose, and Windows support is a stated requirement rather than an aspiration.

So the real question is not "a dependency or not" but "one implementation or two".

## Decision

**Use `pyserial` on every platform, imported lazily, as a `[project.optional-dependencies]` extra.**

A single `SerialPublisher` alongside `MqttPublisher`, behind the `Publisher` protocol that already
exists. `import serial` happens inside `start()`, never at module import, so the pure logic, the
protocol parser and the whole test suite continue to run with nothing installed — the property D31
established for paho, applied again rather than eroded.

**The heartbeat becomes part of the transport interface**, as `Publisher.heartbeat()`. `MqttPublisher`
implements it as a no-op and says why: retention plus the Last Will already cover host liveness there.
`SerialPublisher` writes `host on`. That puts the asymmetry documented in `CONTEXT.md` into the type
rather than into a comment in the daemon's main loop.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **`termios` on POSIX, `pyserial` only on Windows** | The one that looks thriftiest, and the rot lands in the worst place. Development is on macOS, so the `termios` branch would be exercised on every run and the Win32 branch almost never — making Windows, the platform we cannot test casually, the one served by the less-travelled code. It also means implementing raw-mode setup, partial writes, timeouts and port reopening twice, in two places that will drift. And **it saves nothing in the dependency graph**: Windows still needs `pyserial`, so the dependency exists either way; POSIX merely takes a different road to the same place. |
| **Hand-rolled `ctypes` bindings on Windows** | Removes the dependency entirely. Rejected without much hesitation: `SetCommState`, DCB structures and overlapped I/O are a genuine amount of fiddly code to own, on the one platform where we cannot casually reproduce a bug report. This is precisely the work `pyserial` has already done and debugged. |
| **A zero-dependency wired variant on POSIX** | The strongest argument for splitting, and worth stating because it is not silly: a USB-only deployment needs no MQTT at all, so with `termios` it could run with *nothing* installed — a genuinely attractive property for the simplest configuration. Rejected because it would hold only on POSIX, would still need `pyserial` on Windows for the same variant, and buys that partial win with a second implementation. Revisit only if "runs with no dependencies at all" becomes a stated goal rather than a nicety. |
| **A hard rather than optional dependency** | Simpler packaging. Rejected because most installations will be MQTT-only and should not be made to install a serial library to publish to a broker. `wigwagd[serial]` keeps the default install as small as it is today. |
| **Shell out to a serial tool** | No dependency at all. Rejected: a per-write subprocess is absurd for a 2-second heartbeat, and there is no portable such tool anyway. |

## Consequences

**Accepted costs**
- A second runtime dependency, though an optional and unusually safe one: `pyserial` is pure Python
  (`termios` on POSIX, `ctypes` to Win32 on Windows), so there is no compiled extension and no build
  step on any platform.
- The daemon grows a transport choice, which is a new way to be misconfigured. Mitigated by making the
  serial port explicit configuration rather than magic: the daemon will not guess a port unless asked.
- `pyserial` was already installed in this repo's development environment for `fake_rnwf02.py` and
  `miniterm`, but it was never a *declared* dependency of the host package. It is now, so the
  distinction between "on my machine" and "in the manifest" is closed rather than left implicit.

**Benefits**
- One serial implementation, exercised identically on every platform, with Windows on the same code
  path as macOS.
- The wired transport becomes usable end to end, which is what D111 was blocked on.
- `Publisher.heartbeat()` makes the MQTT-versus-serial liveness asymmetry explicit at the interface,
  where the next person will see it.
- The lazy import means `pytest` still runs with nothing installed, so the test suite does not acquire
  a hardware-shaped dependency.

**Revisit if** a dependency-free daemon becomes a goal (then the POSIX/`termios` split earns its second
implementation), or if `pyserial` ever stops being pure Python — at which point the calculus above
changes and this should be re-argued rather than inherited.
