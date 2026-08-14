# Journal

Append-only development log. **Newest entry first.** Durable decisions live in
[`docs/adr/`](docs/adr/) and are referenced from here as `ADR-NNNN`.

Entries record what was done, why, and — importantly — what was tried and rejected.

---

## 2026-08-14 — Phase 1 host software: daemon, CLI, hook client, 93 tests

Branch `phase1/host-software`.

**Done**
- `host/` is a **uv** project (`pyproject.toml`, `uv.lock`), Python 3.11+, one runtime dep
  (`paho-mqtt`) imported lazily so the pure logic and the whole test suite need nothing installed.
- `wigwagd`: loopback UDP listener → per-session store with TTL → priority aggregation →
  retained MQTT publish. Modules split so the core is I/O-free and clock-injected:
  `state.py`, `protocol.py`, `config.py`, `listener.py`, `publisher.py`, `daemon.py`, `paths.py`.
- `wigwag` CLI: `set`, `clear`, `status`, `watch`, `config` — the generic push API (D36).
- `host/hooks/wg-notify`: the hook client, shell + bash `/dev/udp`. **2.9 ms median, 3.7 ms p95.**
  Plus `wg-notify.ps1` for Windows without Git Bash.
- `host/settings.hooks.json` (hook wiring, exec form), `wigwag.example.toml`, `host/README.md`.
- **93 tests passing**, including 9 integration tests that run the *real* shell client over
  *real* loopback UDP against a live listener.
- ADR-0010 (cross-platform + UDP hook client), ADR-0011 (configurable broker, TLS policy).

**Why**
- Cross-platform and "local or outside MQTT server" were added as requirements, and both
  invalidated parts of the plan — see below.
- Config is layered defaults → TOML → env, so the zero-config case is a working local setup.
  TLS **infers on** for any non-loopback broker: changing one config line to a remote host must
  not silently start shipping credentials and session activity in the clear. Explicit plaintext
  is still allowed but warns loudly (Rule 4 applied to configuration).

**Tried and rejected**
- **AF_UNIX datagram socket** (the original plan's transport). Windows has no AF_UNIX *datagram*
  support, and bash's `/dev/udp` cannot address Unix sockets either — so it would also have
  ruled out the fast hook client. Replaced with loopback UDP.
- **`sh` + `nc`** (the original plan's client). `nc` is absent from Git Bash on Windows, and its
  flags differ across BSD netcat, GNU netcat and `ncat`. Replaced with bash `/dev/udp`, which
  needs no external binary at all.
- **Python hook client.** One implementation everywhere, but 30–50 ms of interpreter start-up on
  a script that runs on every tool call. Rejected on cost.
- **PowerShell as the primary Windows client.** 100–300 ms start-up, and unnecessary since Claude
  Code uses Git Bash by default. Demoted to a documented fallback.
- **Go/Rust compiled client.** Genuinely good — same ~3 ms, no runtime dep. Rejected because
  bash already hits that latency with zero build step, and a compiler toolchain is real cost in a
  repo already carrying Zephyr, KiCad and OpenSCAD. Pre-analysed as the fallback in ADR-0010.
- **A bare `2>/dev/null` on the `/dev/udp` redirect.** Does *not* suppress the shell's own
  redirection-failure message, so a stopped daemon leaked `connect: Operation not permitted` to
  stderr — which `SessionEnd` shows to the user. Fixed by wrapping the redirect in a subshell.
  Found by testing the daemon-down path, not by reading the script.
- **Coalescing on the full aggregate including `reason`.** Looked right, and the test suite caught
  it: `PreToolUse` and `PostToolUse` differ only in reason, so every tool call republished the
  retained message twice — precisely the burst coalescing exists to prevent. Now keyed on
  `state` + session count only (D54).
- **`PING` triggering a publish.** A liveness probe must not be able to cause broker traffic.

**Verified**
- Hook client latency: **2.9 ms median / 3.7 ms p95** over 30 runs. A test asserts median < 50 ms
  as a regression guard against reintroducing an interpreter.
- Rule 3 holds in every case tested: exit 0, empty stdout, empty stderr — with the daemon down,
  with junk on stdin (`""`, `not json`, `{}`, `{"session_id":}`), and with an unknown verb.
- `/bin/sh` on macOS is **bash 3.2.57**, so `/dev/udp` is available where hooks run.
- **No `CLAUDE_SESSION_ID` env var exists** — only `CLAUDE_PROJECT_DIR`, `CLAUDE_PLUGIN_ROOT`,
  `CLAUDE_PLUGIN_DATA`, `CLAUDE_EFFORT`, `CLAUDE_CODE_REMOTE`, `CLAUDE_CODE_BRIDGE_SESSION_ID`.
  So `session_id` must come from stdin JSON; that is why the client parses at all.
- Claude Code runs hooks under **bash on all platforms** (Git Bash on Windows, PowerShell only as
  fallback) — the fact the whole client design rests on.
- Live smoke test with `--dry-run`: two producers (a hook session and a `ci` CLI producer),
  `WAIT` correctly held while CI reported `BUSY`, fell back to `BUSY` when the session hit `Stop`,
  then to `IDLE` when CI was cleared. No duplicate publishes.

**Verified against a real broker**
Installed `mosquitto` via brew *without* registering it as a service (run on demand).

- Full publish path: `IDLE → BUSY → WAIT → IDLE` observed arriving at the broker via
  `mosquitto_sub -t 'wigwag/#' -v`, plus `host_online 1` on connect.
- **Retained messages behave as ADR-0003 requires** — the load-bearing claim of the whole
  transport choice. A *fresh* subscriber (i.e. the device booting after the fact) immediately
  receives `{"state":"WAIT",...}` with no host involvement, and repeatedly, not one-shot.
- **Last Will fires on an unclean death**: SIGKILL → `host_online 0`. Clean SIGTERM also
  publishes `0`.
- **The retained state survives the daemon's death**, which is exactly *why* the device must
  fail-visible on link loss (ADR-0007): the broker keeps serving a state whether or not anything
  is still producing it. Seeing that directly makes ADR-0007 feel less like caution and more like
  a requirement.
- **Hook block merged into `.claude/settings.json`, and this session now drives the light.**
  `wigwag status` shows this session's own id (`0fec7bb8-…`) as a live session in `BUSY` from
  `PreToolUse`. Real hooks → real bash client → real daemon → real broker.

**A test bug worth recording, since it nearly became a false bug report**
The first Last Will test showed `host_online 1` after SIGKILL, which looked like a broken will.
It was a flawed test: `kill -9 $!` killed the `uv run` *wrapper* and orphaned the Python child,
so the MQTT connection stayed open and the will correctly did not fire. `pgrep -fl` exposed the
survivor. Re-run against the real process, both SIGKILL and SIGTERM produce `0`.
Lesson: `uv run` is a wrapper — for signal-handling tests, exec the interpreter directly
(`.venv/bin/python -m wigwagd`) so `$!` is the process under test.

**Open**
- **`WAIT` has not been observed live** — it needs a real permission prompt. Covered by unit and
  integration tests, but not yet seen arriving from an actual `Notification` hook.
- **Windows is untested.** Portable by construction, but unrun. Stated in ADR-0010 rather than
  glossed over.
- Device-side TLS with Trust&Go remains future work.
- `mosquitto` runs on demand, not as a service, so the light only works while it is started.
  Worth revisiting once the device exists and this becomes daily-use rather than a test.

**Cross-platform operator documentation**
Rewrote `host/README.md` as a runbook covering macOS, Linux and Windows: prerequisites,
start-everything, broker install/run/service per platform, remote broker with TLS,
autostart at login, config reference, and a troubleshooting table. Root `README.md` gained a
getting-started section pointing at it. Added `host/deploy/` with a mosquitto config, a
launchd plist and a systemd user unit.

**The trap that would have cost hours later**
`mosquitto` 2.x with no config file **starts in "local only mode" and refuses every
connection not from the same machine.** Verified: a LAN publish to this host's own IP was
refused, and the broker log says so outright — *"Starting in local only mode… Create a
configuration file which defines a listener to allow remote access."*

This is insidious because it does not affect host development at all: `wigwagd` connects over
loopback and everything looks correct. It breaks only when the **device** tries to connect
over Wi-Fi, at which point the symptom is "the light never connects" with a working daemon.
Documented prominently in both READMEs and fixed by `deploy/mosquitto-wigwag.conf`.

Verified the fix rather than assuming it: `listener 1883` + `allow_anonymous true` → LAN
publish OK; `allow_anonymous false` + `password_file` → anonymous refused, authenticated OK;
and `wigwagd` connects to that authenticated broker over the LAN and publishes a retained
message. ADR-0011's plaintext warning fired for real in that last test, which was pleasing.

**Verified in the deploy files** — because shipping a config file is not the same as it working:
- `plutil -lint` clean on the launchd plist; the `sed` one-liner in the README leaves zero
  placeholders and the resulting interpreter path exists and runs.
- The systemd unit parses. Note `configparser` chokes on systemd's `%t` specifier unless
  interpolation is disabled — that was my *test script's* bug, not the unit's.
- Pinned `WIGWAG_STATUS_FILE=%t/wigwag/status.json` in the systemd unit. Without it the
  daemon derives the path from `XDG_RUNTIME_DIR`, and if that were unset it would fall back
  to a temp dir that `PrivateTmp=true` makes private to the service — so `wigwag status`
  would silently find nothing. `%t` is what the CLI computes, so both sides now agree.
- **Not** verified: the systemd unit on real Linux (`systemd-analyze` unavailable on macOS),
  and the Windows Task Scheduler steps. Both marked ⚠️ in a table in `host/README.md` rather
  than implied to work.

**Commissioning captured (ADR-0012) — it was only a passing line in the plan**
Raised as "how will we commission the wigwag — USB, Wi-Fi, Bluetooth, other?" The plan had one
line ("credentials via Kconfig for v1") and a vague future-directions note. It needed a real
decision, because it **constrains the PCB** and therefore had to be settled before Phase 3.

- **USB commissioning is impossible on this MCU.** PL10's peripheral summary (Table 8-1) lists
  SERCOM0/1, TC0/1/2, TCC0, ADC, AC, CCL, PTC, DMAC, EVSYS, RTC, WDT and so on — **no USB
  peripheral**. It would take an MCP2221A bridge, D+/D− routing, ESD, and reversing D24
  (USB-C power-only). Recorded as D57 because this is precisely the kind of thing that is free
  to decide now and a respin to discover later.
- **Bluetooth is out**: RNWF02 is Wi-Fi only (its PTA is for coexisting with an *external* BT
  radio) and PL10 has no radio.
- **RNWF02's provisioning support is much better than I assumed** — this is the find that made
  the decision easy. It has Soft-AP, an `AT+WPROV` **provisioning socket**, and a provisioning
  service that *"implements or handles all the required AT commands to start the module in
  Access Point mode and open up a TCP tunnel or serve a HTML web page to receive the Wi-Fi
  credentials."* Completion hands back `[Mode, SSID, Passphrase, Security, Autoenable]`. There
  is a **Microchip Wi-Fi Provisioning mobile app** and a `wifi_easy_config` reference demo.
  So the module serves the page and parses credentials; the host only sends AT commands — which
  is the only reason this fits in 8 KB.
- **Decision:** v1 compile-time Kconfig (fastest to a working light), v1.1 SoftAP provisioning
  triggered by a long-press on the button we already have, with all three lamps cycling so the
  mode cannot be confused with `WAIT` or the amber flicker.

**The distinction I nearly missed:** commissioning is *two* problems, not one — Wi-Fi credentials
**and** broker configuration. The module's provisioning service only knows about Wi-Fi. The broker
config has no path yet, so it stays compile-time even in v1.1 until the `AT+WPROV` socket is
extended. Logged as **D60, still open**, rather than quietly assumed solved.

**What this buys the PCB:** nothing new on the BOM. Break out the host UART (SERCOM0) to pads,
keep the button on an interrupt-capable GPIO, keep module `MCLR` under host control, keep the
`UART2_TX` debug pad. All pin assignments, all free now.

Noted but not binding: the EU **RED Delegated Act** has applied since 2025-08-01 to
network-connected radio equipment — no default passwords, secure credential storage,
authenticated updates. Microchip's own guidance says its RNWF02 reference apps ship with default
passwords that a product must remove. Irrelevant for a personal device, but commissioning is
where that work would land, and "provisioning AP with no password" would fail first.

**Broker auto-discovery investigated and rejected (ADR-0013), resolving D60**
Asked whether any auto-discovery mechanism exists for MQTT brokers. There is a real standard —
DNS-SD with registered service names `_mqtt._tcp` (1883) and `_secure-mqtt._tcp` (8883) — but
**neither end of this system speaks it**, and I verified both rather than assuming:

- **RNWF02 has no mDNS.** Network features are listed identically in the datasheet and the sell
  sheet: TCP, UDP, DHCP, ARP, HTTP, MQTT, IPv4/IPv6, TLS 1.2, DNS, SNTP. No mDNS, no Zeroconf.
- **mosquitto does not advertise.** Zero mDNS/avahi symbols in the installed binary, nothing in
  its usage output, and a live `dns-sd -B _mqtt._tcp local` browse on this LAN returned nothing.

Two naming collisions worth keeping straight, because both nearly misled me:
- Microchip's **Harmony 3 TCP/IP Library** *does* have `TCPIP_MDNS_ServiceRegister` and Zeroconf
  link-local support — but that is the host-side stack for parts like PIC32MZ that run their own
  IP stack. It is not RNWF02 module firmware and is not reachable over the AT interface.
- **Home Assistant "MQTT discovery"** is a device describing *itself* to HA once connected —
  not broker discovery. It is the thing people usually mean by "MQTT auto-discovery", and it is
  a plausible future feature, but it does not answer this question.

So mDNS would mean hand-rolling DNS-SD packet construction and parsing over a raw UDP socket in
8 KB, *plus* separately advertising the broker — to save typing one hostname.

**The find that settled it: `AT+CFGCP`.** Configuration Storage/Retrieval, added in RNWF02
firmware v3.0 — AT command configurations can be *"archived to non-volatile storage for later
retrieval… the commands re-played upon retrieving."* Since MQTT is configured *by* AT commands,
the module can persist **broker config, not just Wi-Fi**. Configuration becomes a once-ever
event, not once per boot, which removes most of the motivation for discovery. Recorded as D62,
with a note to verify the module ships firmware ≥ v3.0 at Phase 2 bring-up.

Decision: broker address is **entered during provisioning and persisted**, defaulting to a
**hostname rather than an IP** so it survives DHCP lease changes via router-registered local DNS.

Also rejected, with reasons in the ADR: a UDP broadcast beacon from `wigwagd` (trivially
spoofable, and it would make the device depend on a live host to find its broker — undermining
the independence retained messages buy), DHCP options, DNS SRV, fixed `.local` names, QR codes,
cloud rendezvous, and probing a candidate list. That last one is the worst option available: on a
shared network it could silently connect to someone else's broker, producing exactly the
confidently-wrong behaviour ADR-0007 exists to prevent.

**Next**
Phase 2 — starting with the **D49 TCC PWM spike**, which gates the PCB: get `pwm_mchp_tcc_g1`
bound to PL10 via devicetree and prove it with a breathing LED on an `EV10P22A`. Worth pairing it
with a provisioning-service spike on the same hardware, since both are RNWF02/Zephyr unknowns and
both feed the layout — and while there, record the module's firmware version to confirm
`AT+CFGCP` is available (D62).

---

## 2026-08-14 — Project scoped, named, and Phase 0 documentation built

**Done**
- Scaffolded the repo: `README.md`, `CONTEXT.md`, `CLAUDE.md`, `.gitignore`, directory tree for
  `host/`, `firmware/`, `hardware/`, `enclosure/`, `docs/adr/`. `git init` done, **nothing
  committed yet**.
- Built the reusable `journal` skill at `.claude/skills/journal/` — `SKILL.md`, three templates
  (`entry.md`, `adr.md`, `journal-header.md`), and `journal-reminder.sh`. Written with zero
  project specifics so it can be lifted to `~/.claude/skills/` (ADR-0005).
- Wrote ADR-0001 through ADR-0009.
- Mirrored the plan into `docs/PLAN.md` with a decision register (`D01`…`D51`).

**Why**
- Journaling first was an explicit instruction, so Phase 0 precedes any code.
- The name is deliberately vendor-neutral: Claude Code is the only producer today, but the design
  may serve other AI tools later, and nothing in the protocol or firmware is Claude-specific. A
  *wigwag* is the railroad grade-crossing signal with a swinging red lamp.
- Documentation split into journal + ADRs + `CONTEXT.md` + `CLAUDE.md` per ADR-0005. On
  terminology: there is **no** standard term for the chronological log — devlog, worklog and
  engineering journal are used interchangeably. **ADR is** the standard term, but only for the
  decisions subset. Adjacent conventions: "memory bank" (Cline), "handoff".

**Tried and rejected**
- **AVR64DU32 as the MCU.** Ideal on every other axis for a USB-tethered build — native
  crystal-less USB FS, bus-powered from 5 V VBUS via an internal 3.3 V USB VREG, no bridge chip.
  **Zephyr has no 8-bit AVR support**, so it was disqualified the moment Zephyr became a
  requirement. Whole design direction changed.
- **Zephyr's `winc1500` Wi-Fi driver.** The only in-tree Microchip Wi-Fi driver, and its Kconfig
  says it is **deprecated, scheduled for removal in Zephyr 4.6**, for lack of a maintainer. Dead
  end for a new 2026 design → ADR-0002 uses RNWF02 as a network co-processor instead.
- **WFI32E01 / PIC32MZ-W1** (single-chip Wi-Fi MCU, would collapse the BOM): MIPS, no Zephyr SoC
  support. **WBZ451 / PIC32CX-BZ2**: no upstream Zephyr SoC support either.
- **PIC32CM JH01 `PIC32CM5164JH01048`** as the target — first draft's choice. Superseded; see the
  error below. Retained as the 32-pin/16 KB escape hatch (D20).
- **Running the MCU at 5 V using MVIO.** Electrically fine and would delete three FETs, but no net
  BOM saving (the module needs 3.3 V regardless), extra Zephyr enablement work, pinout
  constraints, and `AVDD` is internally tied to `VDD` so VDD would be raw USB 5 V → ADR-0009.

**Verified**
- Zephyr Microchip board support — `boards/microchip/pic32c/` has ten boards including
  `pic32cm_pl10_cnano` and `pic32cm_jh01_cnano`; `soc/microchip/` has `{mec,miv,pic32c,pic64,sam,smartfusion2}`.
- `winc1500` deprecation — read directly from `drivers/wifi/winc1500/Kconfig.winc1500`. It is also
  the *only* Microchip entry in `drivers/wifi/`.
- **PL10 TCC/PWM** — datasheet §23.2: one TCC instance with NPWM, DPWM, dual-slope and critical
  PWM modes. Pinout table §2.3: TCC0 `WO0`–`WO3` reach the 28-pin package.
- **Mainline Zephyr PWM drivers for this silicon** — `drivers/pwm/` contains
  `pwm_mchp_tcc_g1.c`, `pwm_mchp_tc_g1.c` and `Kconfig.mchp`.
- **PL10 memory and packages** — `PIC32CM6408PL10` = 64 KB flash / 8 KB SRAM across 28/32/48/64
  pins. 28-pin available as SSOP (`-I/SS`, 1222 in stock), VQFN (`-E/3LW`, 2093) **and SPDIP
  through-hole (`-I/SP`, 285)**. A 32-bit M0+ in a DIP is a genuinely useful prototyping option.
- **PL10 electricals** (Table 37-1): VDD/VDDIO2 abs max −0.3 to +6.5 V; 50 mA max sink per I/O
  pin; 250 mA into VDD; 140 mA out of GND; 800 mW total. VOL/VOH characterized at 1.8/3.0/5.5 V.
- §3.2.3 — tie `VDDIO2` to `VDD` externally for single-supply mode. §2.2.1 — `AVDD` is internally
  connected to `VDD` on this part.
- **RNWF02** — ASCII AT commands over 2-wire UART; on-module TCP/IP, TLS 1.2, DHCP, DNS, WPA3 and
  **MQTT pub/sub**; so the host needs no network stack. Antenna rules: module at board edge,
  ground-plane edges aligned, ≥10 mm to plastic, ≥31.75 mm to metal.
- **Claude Code hook surface** — `Notification` has matchers `permission_prompt`, `idle_prompt`
  and `agent_needs_input`, which map exactly onto the `WAIT` lamp. `SessionEnd` hooks share a
  ~1.5 s budget. `UserPromptSubmit` stdout is injected into the model's context and `SessionStart`
  stdout is shown to the user — hence the never-write-to-stdout rule.
- Dev boards: `EV10P22A` (PL10 Curiosity Nano, `PIC32CM6408PL10048`, identical 64 K/8 K),
  `EV72E72A` (RNWF02 Add-on Board, 268 in stock).

**Mistake worth remembering**
I claimed PL10 had no PWM, and rejected it on that basis. The claim came from the Zephyr *board*
doc's supported-features table for `pic32cm_pl10_cnano`, which lists no PWM — but that table
describes only **what that board's port currently enables**, not what the silicon has and not
what drivers exist. The silicon has a full TCC; the driver has been in mainline all along.

Reviewer pushback — *"PL10 should be same TC and TCC peripherals as other SAM / PIC32C devices"* —
was correct, and checking the datasheet settled it in one query. The rule now in `CLAUDE.md`:
check **silicon / driver / board enablement** as three separate layers and say which you checked.
Only "no driver anywhere" is a blocker; a board-layer gap is devicetree work worth upstreaming.

This directly changed the hardware direction — from a 512 KB/64 KB, 48-pin part to a 64 KB/8 KB,
28-pin part, and turned footprint discipline into an explicit design goal (ADR-0008).

**Process note**
Lost ~a dozen inline plan annotations because VS Code's plan-review document is only live at the
approval gate — once implementation started, the buffer closed unsaved and the comments were
unrecoverable. Fix: the plan now lives in-repo at `docs/PLAN.md`, so annotations are durable and
diffable. The compact decision register (`D01`…`D51`) exists so a reviewer can comment per line
instead of hunting through prose.

**Open**
- **8 KB SRAM is unproven.** Estimate is ~6 KB. Must be measured with `ram_report` in Phase 2,
  before the PCB. Escape hatch is D20, on measured evidence only (ADR-0008).
- **D49 spike gates the PCB:** does `pwm_mchp_tcc_g1` bind to PL10 with devicetree work alone?
  Success = a breathing LED on an `EV10P22A`.
- TLS to the broker deferred; v1 is username/password on the LAN (ADR-0003). Trust&Go is on the
  chosen module variant so no respin is needed later.
- Hook block not yet installed into `.claude/settings.json`.

**Next**
Install and verify the `SessionEnd` journal reminder, then start Phase 1 — `wigwagd`, the
`wg-notify` hook client, and the `wigwag` CLI, all verifiable against `mosquitto_sub` with no
hardware present.
