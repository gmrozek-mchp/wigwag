# wigwag host software

The computer side: a daemon that aggregates AI session states and publishes them over
MQTT, a CLI, and the hook client Claude Code executes.

```
Claude Code hooks ──► wg-notify ──UDP──► wigwagd ──MQTT──► broker ──► the light
                     (~3 ms, bash)      (aggregate)       (retained)
```

Runs on macOS, Linux and Windows. Everything below is written for all three.

> **Windows caveat, stated up front:** the design is portable by construction and the
> Windows paths are implemented, but nothing has been *run* on Windows yet. Treat those
> instructions as untested.

---

## Contents

1. [Prerequisites](#1-prerequisites)
2. [Install](#2-install)
3. [Start everything (the short version)](#3-start-everything-the-short-version)
4. [The MQTT broker](#4-the-mqtt-broker) — install, run, and the local-only trap
5. [Run the broker as a service](#5-run-the-broker-as-a-service)
6. [Connect to a remote broker](#6-connect-to-a-remote-broker)
7. [Run wigwagd automatically at login](#7-run-wigwagd-automatically-at-login)
8. [Install the Claude Code hooks](#8-install-the-claude-code-hooks)
9. [Configuration reference](#9-configuration-reference)
10. [Verifying and troubleshooting](#10-verifying-and-troubleshooting)

---

## 1. Prerequisites

**[uv](https://docs.astral.sh/uv/)** and nothing else — uv fetches a suitable Python
itself, so you do not need a system Python 3.11+.

| Platform | Install uv |
|---|---|
| macOS | `brew install uv` |
| Linux | `curl -LsSf https://astral.sh/uv/install.sh \| sh` |
| Windows | `winget install astral-sh.uv` |

Verify: `uv --version`

## 2. Install

```sh
git clone <repo> wigwag
cd wigwag/host
uv sync          # creates .venv, installs paho-mqtt + pytest
uv run pytest    # 93 tests, needs no broker and no network
```

### The wired transport (no Wi-Fi, no broker)

If the device is plugged into this machine, skip MQTT entirely:

```sh
uv sync --extra serial                     # adds pyserial
WIGWAG_SERIAL_PORT=auto uv run wigwagd -v  # or /dev/cu.usbmodem…, or COM5
```

`auto` finds an MCP2221A by its factory USB identity (04d8:00dd) and refuses to guess if it sees
none or several. The daemon then sends `state BUSY` lines and a `host on` heartbeat every 2 s, and
the device trusts the wire while that keeps arriving — it has to, because a serial line has no
retained topic and no Last Will to report a dead daemon (D111, `CONTEXT.md`). A clean shutdown sends
`host off`, which the device honours at once.

Run with `-vv` to see both directions, including the device's own diagnostics:

```
serial > state BUSY
serial < wigwag: transport usb TRUSTED (ok)
serial < wigwag: RESET BY WATCHDOG (rcause 10)
```

That log is the only place a host ever sees those, so it is worth turning up when something is odd.

**One thing to know:** the device decides which transport owns its lamps, and it is a stored setting
rather than something this daemon can claim (ADR-0022). A fresh device defaults to the wire, so it works
with no configuration; a device set to `wifi` will ignore everything sent here except configuration
commands. `set transport usb`, `save` and `reboot` on the device's console switches it.

`uv sync` is the only build step. On Windows use PowerShell or Git Bash; the commands
are identical.

## 3. Start everything (the short version)

Three things run: a **broker**, the **daemon**, and (optionally) the **hooks** that feed
it. To see it working right now, with no broker at all:

```sh
cd host
uv run wigwagd --dry-run -v      # terminal 1: logs what it *would* publish
uv run wigwag set BUSY           # terminal 2
uv run wigwag set WAIT --id ci --reason build-blocked
uv run wigwag status
uv run wigwag clear --id ci
```

`--dry-run` needs no broker and no MQTT dependency, so it is the fastest way to confirm
the plumbing before dealing with a broker.

With a broker, the full runbook is:

```sh
# 1. broker  (see §4 — a bare `mosquitto` is local-only and will not reach the device)
mosquitto -c host/deploy/mosquitto-wigwag.conf -v

# 2. daemon
cd host && uv run wigwagd -v

# 3. watch what the device would see
mosquitto_sub -h localhost -t 'wigwag/#' -v
```

Then install the hooks (§8) so your sessions drive it automatically.

## 4. The MQTT broker

### Install

| Platform | Command |
|---|---|
| macOS | `brew install mosquitto` |
| Debian / Ubuntu / Raspberry Pi OS | `sudo apt install mosquitto mosquitto-clients` |
| Fedora / RHEL | `sudo dnf install mosquitto` |
| Arch | `sudo pacman -S mosquitto` |
| Windows | `winget install EclipseFoundation.Mosquitto` (or `choco install mosquitto`, or the installer from [mosquitto.org/download](https://mosquitto.org/download/)) |

`mosquitto_sub` / `mosquitto_pub` come with the package on macOS and Windows; on Debian
they are in `mosquitto-clients`.

### ⚠ The local-only trap

**A default mosquitto 2.x install refuses every connection that is not from the same
machine.** It tells you so at startup:

```
Starting in local only mode. Connections will only be possible from clients
running on this machine. Create a configuration file which defines a listener
to allow remote access.
```

That is fine while you test the host software — and it silently breaks the device, which
connects over Wi-Fi from somewhere else. The symptom is confusing: `wigwagd` works, the
light never connects.

The fix is a config file with a `listener`. One is provided, tested, at
[`deploy/mosquitto-wigwag.conf`](deploy/mosquitto-wigwag.conf):

```conf
listener 1883
allow_anonymous true
```

`listener <port>` with no bind address listens on **all** interfaces, which is what makes
the device able to reach it. Only use `allow_anonymous true` on a network you trust —
anyone on it can then drive your light.

### Run it on demand (nothing left running)

```sh
# macOS / Linux
mosquitto -c host/deploy/mosquitto-wigwag.conf -v

# Windows (PowerShell)
& "C:\Program Files\mosquitto\mosquitto.exe" -c host\deploy\mosquitto-wigwag.conf -v
```

`-v` logs every connection and publish, which is the fastest way to see whether the
device is reaching you. Ctrl-C stops it.

### Add a username and password (recommended)

```sh
# macOS (brew)
mosquitto_passwd -c /opt/homebrew/etc/mosquitto/wigwag.passwd wigwag
# Linux
sudo mosquitto_passwd -c /etc/mosquitto/wigwag.passwd wigwag
# Windows (PowerShell, as admin)
& "C:\Program Files\mosquitto\mosquitto_passwd.exe" -c C:\ProgramData\mosquitto\wigwag.passwd wigwag
```

Then in the broker config, replace the anonymous block with:

```conf
listener 1883
allow_anonymous false
password_file /etc/mosquitto/wigwag.passwd
```

And give the daemon the credentials — keep the password in the environment, not the
config file:

```sh
export WIGWAG_MQTT_USERNAME=wigwag
export WIGWAG_MQTT_PASSWORD='...'
uv run wigwagd -v
```

The device needs the same credentials, in the gitignored `firmware/credentials.conf`.

## 5. Run the broker as a service

Only worth doing once you use wigwag daily — the light only works while a broker is up.

### macOS

```sh
brew services start mosquitto      # starts now, and at login
brew services list                 # check
brew services stop mosquitto
```

`brew services` uses `/opt/homebrew/etc/mosquitto/mosquitto.conf`, so put the `listener`
directives from §4 **in that file** — the service ignores a `-c` you are not passing.

### Linux (systemd)

The package ships a system service, already reading `/etc/mosquitto/mosquitto.conf`:

```sh
sudo cp host/deploy/mosquitto-wigwag.conf /etc/mosquitto/conf.d/wigwag.conf
sudo systemctl enable --now mosquitto
systemctl status mosquitto
sudo journalctl -u mosquitto -f
```

Debian's default config includes `/etc/mosquitto/conf.d/*.conf`, so dropping a file there
is cleaner than editing the main config.

### Windows

The installer registers a Windows service:

```powershell
# copy the listener directives into C:\Program Files\mosquitto\mosquitto.conf first
Start-Service mosquitto
Get-Service mosquitto
Set-Service mosquitto -StartupType Automatic   # start at boot
```

### Don't forget the firewall

The device has to reach port 1883 (or 8883):

```sh
# macOS: System Settings ▸ Network ▸ Firewall ▸ Options ▸ allow mosquitto
# Linux (ufw)
sudo ufw allow 1883/tcp
# Linux (firewalld)
sudo firewall-cmd --add-port=1883/tcp --permanent && sudo firewall-cmd --reload
```

```powershell
# Windows
New-NetFirewallRule -DisplayName "MQTT 1883" -Direction Inbound -LocalPort 1883 -Protocol TCP -Action Allow
```

## 6. Connect to a remote broker

A broker elsewhere — another machine on the LAN, a VPS, or a managed service — means the
light works regardless of which machine you are on, and no broker to run locally.

Point `broker.host` at it. **TLS turns itself on automatically for any non-loopback
host** (ADR-0011), so the secure case needs no extra flags:

```toml
# ~/.config/wigwag/config.toml   (%APPDATA%\wigwag\config.toml on Windows)
[broker]
host = "mqtt.example.com"
port = 8883
username = "wigwag"
# password: use WIGWAG_MQTT_PASSWORD instead of putting it here
```

```sh
export WIGWAG_MQTT_PASSWORD='...'
uv run wigwag config     # confirm host, port and tls=True before starting
uv run wigwagd -v
```

Or entirely by environment, no file:

```sh
export WIGWAG_BROKER_HOST=mqtt.example.com
export WIGWAG_MQTT_USERNAME=wigwag WIGWAG_MQTT_PASSWORD='...'
uv run wigwagd -v
```

**Self-signed certificate?** Point at the CA rather than disabling verification:

```toml
ca_cert = "/path/to/ca.crt"
```

`insecure_skip_verify = true` exists but encrypts *without authenticating the broker*, so
it does not protect against interception — the daemon warns when it is on.

**Plaintext to a remote broker** (a trusted VLAN, or inside a WireGuard tunnel) is allowed
but deliberately noisy:

```toml
host = "192.168.1.50"
tls = false
```
```
WARNING broker 192.168.1.50:1883 is remote but TLS is disabled —
        credentials and session activity will cross the network in the clear
```

**Two lights on one broker?** Give each a distinct topic prefix and client id:

```toml
[broker]
client_id = "wigwagd-laptop"
[topics]
prefix = "wigwag/laptop"
```

**Check reachability before blaming wigwag:**

```sh
mosquitto_sub -h mqtt.example.com -p 8883 -u wigwag -P "$WIGWAG_MQTT_PASSWORD" \
              -t 'wigwag/#' -v --capath /etc/ssl/certs
```

## 7. Run wigwagd automatically at login

Unit files are in [`deploy/`](deploy/). Each needs the two `REPLACE_ME` paths edited. They
invoke `.venv/bin/python -m wigwagd` rather than `uv run`, so the service supervises the
daemon itself instead of a wrapper process — `uv run` is a wrapper, and killing it can
orphan the child.

What has actually been checked, since "it's in the repo" is not the same as "it works":

| File | Verified |
|---|---|
| `deploy/mosquitto-wigwag.conf` | ✅ starts, no local-only warning, LAN publish confirmed |
| `deploy/com.wigwag.wigwagd.plist` | ✅ `plutil -lint` clean; paths and interpreter confirmed |
| `deploy/wigwagd.service` | ⚠️ parses correctly, but **not** run through `systemd-analyze verify` or started on a real Linux box |
| Windows Task Scheduler steps | ⚠️ untested |

### macOS — launchd

```sh
sed -i '' "s|REPLACE_ME|$(cd .. && pwd)|g" deploy/com.wigwag.wigwagd.plist
cp deploy/com.wigwag.wigwagd.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.wigwag.wigwagd.plist
launchctl list | grep wigwag
tail -f /tmp/wigwagd.log
```

### Linux — systemd user unit

```sh
sed -i "s|REPLACE_ME|$(cd .. && pwd)|g" deploy/wigwagd.service
mkdir -p ~/.config/systemd/user && cp deploy/wigwagd.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now wigwagd
journalctl --user -u wigwagd -f
sudo loginctl enable-linger "$USER"   # keep running after logout
```

### Windows — Task Scheduler

```powershell
$py = "C:\path\to\wigwag\host\.venv\Scripts\python.exe"
$action  = New-ScheduledTaskAction -Execute $py -Argument "-m wigwagd" -WorkingDirectory "C:\path\to\wigwag\host"
$trigger = New-ScheduledTaskTrigger -AtLogOn
Register-ScheduledTask -TaskName wigwagd -Action $action -Trigger $trigger -Description "wigwag host daemon"
```

Or drop a shortcut to `.venv\Scripts\pythonw.exe -m wigwagd` into `shell:startup`
(`pythonw` avoids a console window).

## 8. Install the Claude Code hooks

Merge the `hooks` object from [`settings.hooks.json`](settings.hooks.json) into your
`.claude/settings.json`:

| Hook | Matcher | State |
|---|---|---|
| `SessionStart` | `startup\|resume\|clear` | `IDLE` |
| `UserPromptSubmit` | — | `BUSY` |
| `PreToolUse` / `PostToolUse` | `*` | `BUSY` (heartbeat, refreshes the TTL) |
| `Notification` | `permission_prompt\|idle_prompt\|agent_needs_input` | **`WAIT`** |
| `Stop` | — | `IDLE` |
| `StopFailure` | `*` | `ERROR` |
| `SessionEnd` | `*` | drops the session |

Works identically in the VS Code extension and the terminal — hooks are a CLI-level
feature and the extension bundles the CLI.

Confirm they are firing:

```sh
uv run wigwag status     # your live session id should appear
```

**Windows without Git Bash:** Claude Code uses Git Bash by default and the shell client
works as-is. Only if Git Bash is genuinely absent, swap the command for the PowerShell
fallback:

```json
{
  "type": "command",
  "command": "powershell.exe",
  "args": ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
           "${CLAUDE_PROJECT_DIR}/host/hooks/wg-notify.ps1", "SET", "BUSY", "PreToolUse"]
}
```

Prefer the shell version: ~3 ms versus 100–300 ms for PowerShell start-up.

## 9. Configuration reference

Layered: **defaults → TOML file → environment**. The defaults are a working local setup,
so no config file is needed to start.

| Platform | Config file |
|---|---|
| macOS / Linux | `$XDG_CONFIG_HOME/wigwag/config.toml`, else `~/.config/wigwag/config.toml` |
| Windows | `%APPDATA%\wigwag\config.toml` |
| Any | `$WIGWAG_CONFIG` |

Start from [`wigwag.example.toml`](wigwag.example.toml). Inspect what is actually in
effect with `uv run wigwag config`.

| Environment variable | Overrides |
|---|---|
| `WIGWAG_CONFIG` | config file location |
| `WIGWAG_BROKER_HOST` / `_PORT` / `_TLS` / `_CA_CERT` | broker connection |
| `WIGWAG_MQTT_USERNAME` / `WIGWAG_MQTT_PASSWORD` | credentials |
| `WIGWAG_TOPIC_PREFIX` | topic prefix |
| `WIGWAG_LISTEN_PORT` | hook→daemon UDP port (default 9410) |
| `WIGWAG_SESSION_TTL` | session expiry seconds (default 900) |
| `WIGWAG_STATUS_FILE` | status snapshot path |

`WIGWAG_HOST` and `WIGWAG_LISTEN_PORT` are also read by the hook client, so a
non-default port needs both sides to agree.

### Topics

| Topic | Direction | Payload |
|---|---|---|
| `wigwag/state` | host → device | `{"state":"WAIT","reason":"permission_prompt","sessions":2}` — retained, QoS 1 |
| `wigwag/brightness` | host → device | `0`–`255`, retained |
| `wigwag/button` | device → host | `{"event":"press","ms":120}` |
| `wigwag/online` | device → host | `1` / `0`, device Last Will |
| `wigwag/host_online` | host → device | `1` / `0`, daemon Last Will |

## 10. Verifying and troubleshooting

```sh
uv run pytest                              # 110 tests, no broker and no device needed
uv run wigwag config                       # effective configuration
uv run wigwag status                       # displayed state + live sessions
uv run wigwag watch                        # print the aggregate as it changes
mosquitto_sub -h localhost -t 'wigwag/#' -v
```

Confirm retained state works — this is what makes the light correct after a reboot:

```sh
mosquitto_sub -h localhost -t 'wigwag/state' -v -C 1 -W 3
```

A fresh subscriber should immediately receive the current state. If it hangs, retention
is not working and the device will come up blank.

| Symptom | Likely cause |
|---|---|
| `wigwag status` says "no status available" | `wigwagd` is not running |
| Hooks fire but state never changes | daemon listening on a different `WIGWAG_LISTEN_PORT` than the hook client |
| `wigwagd` works, the **device** never connects | the local-only trap — broker has no `listener` (§4) |
| Device connects then drops repeatedly | two clients sharing one `client_id`; set a distinct one |
| `paho-mqtt is not installed` | run `uv sync`, or use `--dry-run` |
| Broker refuses the daemon | `allow_anonymous false` with no credentials — set `WIGWAG_MQTT_USERNAME`/`_PASSWORD` |
| Light shows amber flicker | device cannot reach the broker for >10 s — working as designed (ADR-0007) |
| Nothing after a broker restart | add `persistence true` so retained messages survive |

The hook client is deliberately silent and always exits 0, so it will never tell you it
failed. To debug it, run it by hand:

```sh
echo '{"session_id":"test"}' | sh hooks/wg-notify SET WAIT manual
uv run wigwag status        # should now show session "test" in WAIT
```

## Layout

| Path | Contents |
|---|---|
| `wigwagd/state.py` | states and aggregation — pure, no I/O, clock injected |
| `wigwagd/protocol.py` | wire format; parsing is total and never raises |
| `wigwagd/config.py` | TOML + env, TLS policy |
| `wigwagd/listener.py` | loopback UDP receiver |
| `wigwagd/publisher.py` | MQTT via paho, behind an interface with a null impl |
| `wigwagd/daemon.py` | wiring, coalescing, expiry |
| `wigwagd/cli.py` | the `wigwag` command |
| `hooks/wg-notify` | the hook client — shell + bash `/dev/udp`, ~3 ms |
| `hooks/wg-notify.ps1` | Windows fallback for installs without Git Bash |
| `deploy/` | mosquitto config, launchd plist, systemd unit |
| `tests/` | pytest; no broker or network required |

`paho-mqtt` is imported lazily, so the pure logic and the whole test suite run with no
third-party dependency installed.
