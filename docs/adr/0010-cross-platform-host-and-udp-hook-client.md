# ADR-0010 — Host software is cross-platform; the hook client is bash over loopback UDP

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

Development is on macOS, but the host software should run anywhere — Linux, and Windows.
That collided with two choices from the original plan.

**The transport.** The plan said an AF_UNIX datagram socket. Windows has no AF_UNIX
*datagram* support (only stream, since Windows 10), so that alone is disqualifying.

**The hook client.** The plan said POSIX `sh` plus `nc -U`. Several problems:

- `nc` is not present in Git Bash on Windows, and its flags differ wildly between BSD
  netcat, GNU netcat and `ncat` even on Unix.
- `session_id` must be extracted from stdin JSON — and there is **no `CLAUDE_SESSION_ID`
  environment variable**, only `CLAUDE_PROJECT_DIR`, `CLAUDE_PLUGIN_ROOT`,
  `CLAUDE_PLUGIN_DATA`, `CLAUDE_EFFORT`, `CLAUDE_CODE_REMOTE` and
  `CLAUDE_CODE_BRIDGE_SESSION_ID`. So the client has to parse.
- The obvious portable answer, "write it in Python", costs 30–50 ms of interpreter
  start-up on a script that runs on **every tool call**.
- PowerShell is worse: 100–300 ms start-up.

The binding constraint is Rule 3 — never break Claude Code. The client must always exit
0, write nothing to stdout (on `UserPromptSubmit` stdout is injected into the model's
context; on `SessionStart` it is shown to the user), work with the daemon down, and stay
cheap.

Usefully, Claude Code runs hooks under **bash on all three platforms** — Git Bash on
Windows by default, PowerShell only as a fallback.

## Decision

**Loopback UDP as the hook→daemon transport, and a hook client written in shell that
uses bash's built-in `/dev/udp`.** No `jq`, `sed`, `nc`, Python or Node anywhere in the
hook path.

```sh
( printf '%s\n' "$msg" > "/dev/udp/$host/$port" ) 2>/dev/null
exit 0
```

`session_id` is extracted with bash parameter expansion rather than a JSON parser:

```sh
session_id="${line#*\"session_id\":\"}"
session_id="${session_id%%\"*}"
```

Wire format is a single space-separated line so `printf` can produce it:
`SET <STATE> <session_id> [reason]`, `DROP <session_id>`, `PING`.

**Measured: 2.9 ms median, 3.7 ms p95** over 30 runs, versus a ~10 ms budget. A test in
`host/tests/test_integration.py` asserts the median stays under 50 ms, as a regression
guard against someone reintroducing an interpreter.

Supporting decisions:

- **UDP's fire-and-forget semantics make Rule 3 structural rather than careful.**
  `sendto` on a connectionless socket cannot block and cannot fail because nothing is
  listening. The hook physically cannot hang or error out when the daemon is down — that
  is a property of the transport, not of defensive coding.
- The subshell around the redirect matters: a bare `2>/dev/null` does **not** suppress
  the shell's own redirection-failure message, which would surface to the user on
  `SessionEnd`. Found by testing, not by reading.
- The listener binds loopback only. Nothing off-machine should be able to drive the light.
- `hooks/wg-notify.ps1` exists for Windows installs genuinely lacking Git Bash, with the
  same contract. It is documented as the slower fallback.
- Hook config uses **exec form** (`command` + `args`) so `${CLAUDE_PROJECT_DIR}`
  substitutes without quoting problems on any platform.
- Python is used for the daemon and the CLI, where start-up cost is irrelevant. `uv`
  manages the environment; `paho-mqtt` is imported lazily so the pure logic and the
  entire test suite run with no third-party package installed.
- Runtime and config paths are per-platform in one module (`paths.py`, `config.py`):
  `%APPDATA%`/`%LOCALAPPDATA%` on Windows, `XDG_*` elsewhere, with a uid-scoped temp
  directory as the macOS runtime fallback since macOS has no `XDG_RUNTIME_DIR`.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **AF_UNIX datagram socket** (original plan) | No Windows support for datagram Unix sockets, and bash's `/dev/udp` cannot address them either — so it would rule out the fast client too. |
| **`sh` + `nc -U` / `nc -u`** (original plan) | `nc` is absent from Git Bash, and its flags are incompatible across BSD netcat, GNU netcat and `ncat`. An external binary for something bash does natively. |
| **Python hook client** | One implementation everywhere and trivially readable, but 30–50 ms of interpreter start-up on every tool call. Rejected on cost for a script whose entire job is one datagram. |
| **PowerShell as the primary Windows client** | 100–300 ms start-up — an order of magnitude worse than bash, and unnecessary since Claude Code uses Git Bash by default. Kept only as a fallback. |
| **Compiled client (Go/Rust)** | ~3 ms, no runtime dependency, genuinely portable — the strongest alternative. Rejected because bash's `/dev/udp` already hits the same latency with **zero** build step, and adding a compiler toolchain to a repo already juggling Zephyr, KiCad and OpenSCAD is real cost. Revisit if bash ever proves insufficient. |
| **TCP instead of UDP** | Reliable delivery, which sounds better. But `connect()` can block or fail when the daemon is down, so the hook would need timeouts and error handling to honour Rule 3 — reintroducing by hand exactly what UDP gives free. Loss on loopback is not a real concern, and heartbeats make any single loss self-correcting. |
| **Write to a file/FIFO the daemon watches** | No sockets at all and trivially portable. Rejected: FIFOs behave badly on Windows, per-session files need cleanup, and file watching adds latency and platform-specific machinery (FSEvents/inotify/ReadDirectoryChangesW). |
| **`jq` to parse stdin** | Correct JSON parsing rather than string surgery. Rejected as an external dependency in the hook path that is frequently absent, for a field we can extract reliably from Claude Code's compact single-line JSON. If the field is missing we degrade to `session_id=unknown` rather than failing. |

## Consequences

**Accepted costs**
- `/dev/udp` is a bash/ksh feature, not POSIX, despite the `#!/bin/sh` shebang. It works
  because Claude Code runs hooks under bash on every platform — but that is a dependency
  on the harness's behaviour, and it is worth re-checking if hook execution changes.
- `session_id` extraction is string manipulation, not parsing. It relies on Claude Code
  emitting compact single-line JSON. Mitigated by degrading to `unknown` instead of
  failing, and by an integration test that feeds it real hook JSON plus four kinds of junk.
- UDP can in principle drop a datagram. Accepted: heartbeats mean any loss self-corrects
  within one tool call, and the TTL bounds the worst case.
- Two hook client implementations to keep in step (shell and PowerShell).
- **Windows is untested.** The design is portable by construction, but nothing has been
  run there. That is stated rather than implied.

**Benefits**
- 2.9 ms median in the hook path, with no interpreter and no external binary.
- Rule 3's hardest guarantee — never hang, never fail when the daemon is down — comes
  from the transport rather than from defensive code.
- One transport works on macOS, Linux and Windows.
- The CLI and the hook client speak the identical protocol, so producers are
  interchangeable and the daemon cannot tell them apart.
- The whole test suite, including the real hook client over real UDP, runs with no
  broker and no third-party dependency.

**Revisit if** hook execution stops using bash on some platform, if `/dev/udp` proves
unavailable somewhere that matters, or if datagram loss ever shows up in practice — a
small compiled client is the pre-analysed answer.
