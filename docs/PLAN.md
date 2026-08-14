# wigwag — a physical status light for AI coding sessions

<!--
ANNOTATING THIS PLAN
Add notes inline, right under whatever they refer to, as blockquotes starting with NOTE:

    > NOTE: use the TQFP part, we have trays of them

Anything in a `> NOTE:` line is yours; everything else is the original plan.
Save the file, then say "read my notes".

Alternative: Plan permission mode reopens this as an annotatable document with inline
comments at the approval gate. That flow is lossy if the session moves on, so this
file is the durable copy.
-->

## Context

There is no ambient, glanceable signal for what an AI coding session is doing. You have to
look at the terminal or the editor panel to know whether it's working, finished, or blocked
waiting on you — and when several sessions run at once, the one that needs you is easy to miss.

**wigwag** is a desk stoplight that answers "does it need me right now?" from across the room.
(A wigwag is the classic railroad grade-crossing signal — a swinging red lamp.)

| Lamp | Meaning | Behavior |
|---|---|---|
| **green** | idle / ready for input | steady, dim |
| **yellow** | thinking / working | breathing ~0.8 Hz |
| **red** | waiting on you (permission, input) | steady, then slow blink after 30 s |
| _amber flicker_ | link lost — state unknown | deliberately distinct |

Complete project: firmware, host software, custom PCB, 3D-printed enclosure. **Microchip
silicon is a hard requirement.** Wi-Fi, untethered. Zephyr RTOS. Custom PCB. Plus a reusable
journaling workflow skill, established first.

**Design goal beyond the gadget:** find out how small a Microchip part can usefully run Zephyr.
The target is deliberately the smallest Zephyr-supported Microchip device — 28 pins, 8 KB SRAM
— which makes footprint discipline a first-class requirement rather than an afterthought.

**Vendor neutrality:** Claude Code is the only producer being built now, and it is sufficient.
But the name, the wire protocol, and the daemon are deliberately tool-agnostic so other AI
tools can be added later as additional producers. See "Future directions".

---

## Corrections from plan review

Recorded because the reasoning is worth keeping (→ JOURNAL entry + ADR-0001 rewrite):

1. **PL10 does have PWM. My earlier claim was wrong.** I read the Zephyr *board* doc's
   supported-features table and reported it as a silicon/driver limitation. Wrong on both counts:

   | Layer | PL10 reality |
   |---|---|
   | Silicon TC/TCC PWM | ✅ datasheet §23.2 — TCC0 with NPWM, DPWM, dual-slope, critical PWM |
   | Mainline Zephyr PWM driver | ✅ `drivers/pwm/pwm_mchp_tcc_g1.c`, `pwm_mchp_tc_g1.c`, `Kconfig.mchp` |
   | PL10 board port enabling it | ❌ not wired into that board's devicetree |

   So PWM on PL10 is **devicetree work reusing an existing driver** — a small, upstreamable
   contribution, not a blocker. This is what reopened PL10 as the target.

2. **TCC0 has four waveform outputs (WO0–WO3)** on the 28-pin part, and SERCOM0 UART maps to
   different pins, so three lamps + UART coexist with a spare channel. Verified in the
   pinout/multiplexing table, datasheet §2.3.

3. **Supply risk was over-weighted.** Direction is "design for the best-fit part." The 5164/JH01
   sourcing worry is dropped from the plan.

---

## Decision register

`built` = on disk · `settled` = decided · `open` = needs a call · `spike` = needs an experiment

### Phase 0 — documentation & workflow (built; being revised)

| # | Decision | Status |
|---|---|---|
| D01 | `JOURNAL.md` is newest-entry-first, not append-at-bottom | built |
| D02 | Entry template: Done / Why / Tried and rejected / Verified / Open / Next | built |
| D03 | Decisions become numbered ADRs in `docs/adr/`, 4-digit, one per file | built |
| D04 | ADR template **requires** an "Alternatives rejected" table | built |
| D05 | Accepted ADRs are superseded, never edited to change a decision | built |
| D06 | Skill named `journal` (`/journal`), generic so it lifts to `~/.claude/skills/` | built |
| D07 | Skill stays project-local for now, promoted to user dir once proven | settled |
| D08 | Skill bundles `entry.md`, `adr.md`, `journal-header.md` | built |
| D09 | `journal-reminder.sh` = SessionEnd hook, stderr warning if code changed but journal didn't | built |
| D10 | Reminder **never auto-writes** an entry | built |
| D11 | Reminder uses `git status`; silently no-ops outside a git tree | built |
| D12 | `CLAUDE.md` carries 4 standing rules | built |
| D13 | States are `IDLE`/`BUSY`/`WAIT`/`ERROR`, uppercase everywhere | built |
| D14 | `UNKNOWN` is a *link condition* (`LINKED`/`UNLINKED`), not a state | built |
| D15 | Plan mirrored to `docs/PLAN.md` so it survives sessions and is diffable | built |
| D16 | `git init` done; nothing committed yet | built |
| D17 | Install hook block into `.claude/settings.json` | open |
| D43 | Project name **wigwag** — vendor-neutral, so other AI tools can be added without a rename | settled |

### Hardware

| # | Decision | Status |
|---|---|---|
| D18 | Host MCU family = **PIC32CM PL10** — smallest Zephyr-supported Microchip part | settled |
| **D19** | **Target part = `PIC32CM6408PL10028-I/SS`, SSOP-28, 64 KB flash / 8 KB SRAM** (1222 in stock) | settled |
| **D19b** | Bring-up option: `-I/SP` **SPDIP-28 through-hole** — socketable, zero-SMT breadboard prototype | settled |
| D20 | Escape hatch if 8 KB proves impossible: `PIC32CM1216JH01032` (32-pin, 128 K/16 K) | settled |
| **D44** | **Dev board = `EV10P22A` PL10 Curiosity Nano** — `PIC32CM6408PL10048`, *identical* 64 K/8 K, so footprint truth from day one | settled |
| D21 | Wi-Fi = **`RNWF02PC-I/100`**, PCB antenna + Trust&Go, AT-over-UART | settled |
| D22 | Rejected WINCS02 (SPI, binary) for RNWF02's ASCII/UART + in-stock add-on board | settled |
| **D45** | **PCB antenna, not U.FL.** Enclosure absorbs the constraint: ≥10 mm to plastic, ≥31.75 mm to metal | settled |
| D23 | LDO = `MCP1826S-3302E/DB`, 1 A — covers RNWF02 TX peaks with margin | settled |
| **D50** | **Single 3.3 V rail for MCU + module; `VDDIO2` tied to `VDD` (single-supply, §3.2.3)** | settled |
| **D51** | **MVIO / 5 V VDD operation rejected** — no BOM saving, extra Zephyr porting, pinout constraints, `AVDD` tied to `VDD`. Left available for a future revision | settled |
| D24 | USB-C is power-only: no data, no D+/D− ESD network, 5.1 k CC pulldowns | settled |
| D25 | Debug = SWD, pyOCD runner (PICkit 5 / PICkit Basic / Atmel-ICE / J-Link all work) — **verified on the cnano's nEDBG, with DFP ≥ 1.5.437; see D69** | settled |
| **D26** | **Lamps = 3× 10 mm diffused through-hole LEDs, driven from the 5 V rail via low-side N-FETs.** FETs are **required**, not optional — see below | settled |
| **D27** | **4-layer PCB** with dedicated GND and PWR planes for EMC | settled |
| **D46** | TCC0 WO0/WO1/WO2 drive the three lamps; SERCOM0 PAD[0]/PAD[1] drive the module UART | settled |

#### Power architecture

```
USB-C 5V ──┬── MCP1826 ──► 3.3V ──┬──► PL10  VDD  (VDDIO2 tied to VDD, single-supply)
           │                      └──► RNWF02 VDD / VDDIO
           │
           └──► lamp anodes (per-color R)
                     │
               LED cathodes ──► 3× low-side N-FET ──► GND
                                       ▲
                             gates ◄── TCC0 WO0/WO1/WO2 (3.3V PWM)
```

**Single 3.3 V rail for MCU and module. LEDs on 5 V behind FETs.**

*Why the LEDs need 5 V:* 10 mm diffused **green** LEDs run Vf up to ~3.0–3.4 V. From a 3.3 V
rail there is no headroom for a current-setting resistor, so the lamp would be dim, wildly
Vf-sensitive, and effectively un-dimmable. On 5 V there is ~1.6 V of headroom worst case.

*Why not run the MCU at 5 V,* even though PL10 permits it (VDD abs max +6.5 V, VOL/VOH
characterized at 5.5 V, 50 mA per-pin sink — so direct GPIO LED drive would work):

| Reason | Detail |
|---|---|
| No BOM saving | The module needs 3.3 V regardless, so the LDO stays either way |
| MVIO costs porting budget | Dual-supply mode needs `MVIO.VDDIO2CFG`, power sequencing, `VDDIO2OK` monitoring, extra decoupling — and near-certainly another Zephyr enablement task on top of D49, to avoid three SOT-23 FETs |
| Constrains pinout | Module UART would be locked to the VDDIO2 pins (PA08–PA13), lamps to the VDD domain |
| Analog supply | `AVDD` is internally tied to `VDD` on this part (§2.2.1), so VDD would be raw, noisy USB 5 V |
| Robustness | FETs keep LED current out of the MCU entirely (GND pins cap at 140 mA) and leave brightness headroom |

Per §3.2.3, `VDDIO2` is tied to `VDD` externally for single-supply operation. MVIO is left
unused but available if a later revision wants a 5 V-domain peripheral.

FET selection: logic-level N-channel, Vgs(th) low enough to fully enhance at 3.3 V
(e.g. BSS138-class) at 20–60 mA per lamp — undemanding.

### Software

| # | Decision | Status |
|---|---|---|
| D28 | Transport = MQTT, **retained** messages; broker local *or* remote (ADR-0011) | built |
| **D29** | **TLS defaults ON for any non-loopback broker**; explicit plaintext allowed but warns loudly (ADR-0011) | built |
| D30 | Aggregation `ERROR > WAIT > BUSY > IDLE`, per-`session_id`, 15-min TTL | settled |
| D31 | Daemon + CLI in Python 3.11+ with `paho-mqtt`, managed by **uv**; paho imported lazily so tests need no deps | built |
| **D32** | **Hook client is shell using bash `/dev/udp`** over loopback UDP — no `jq`/`sed`/`nc`/Python/Node. Measured **2.9 ms median** (ADR-0010) | built |
| **D52** | **Loopback UDP, not AF_UNIX** — no Windows AF_UNIX datagrams, and UDP `sendto` cannot block or fail when the daemon is down, making Rule 3 structural | built |
| **D53** | **Cross-platform host**: per-platform config/runtime paths, PowerShell hook fallback. Windows is **untested** | built |
| **D54** | Coalescing keyed on `state` + session count, **not** `reason` — else every tool call republishes twice | built |
| **D55** | `topic_prefix` and `client_id` configurable, so two lights can share one broker | built |
| D33 | Hooks always `exit 0` and write **nothing** to stdout | settled |
| D34 | Fail-visible: > 10 s without broker → amber flicker | settled |
| D35 | Button publishes raw presses; the host decides meaning | settled |
| **D56** | **Commissioning: compile-time Kconfig for v1; SoftAP provisioning via the module's own service for v1.1** (ADR-0012) | settled |
| **D57** | **USB commissioning is impossible** — PL10 has no USB peripheral (Table 8-1). Would need an MCP2221A bridge and would reverse D24 | settled |
| **D58** | Long-press the existing button enters provisioning mode; all three lamps cycle so the mode is unmistakable | settled |
| **D59** | PCB must break out host UART (SERCOM0) to pads/header — bench commissioning at zero BOM cost | settled |
| **D60** | **Broker address is configured during provisioning, not auto-discovered** (ADR-0013) | settled |
| **D61** | **mDNS/DNS-SD `_mqtt._tcp` rejected** — RNWF02 has no mDNS and mosquitto does not advertise (both verified) | settled |
| **D62** | **`AT+CFGCP` persists config to module NVM** (firmware v3.0+), so broker settings survive reboots and are entered once ever | settled |
| **D63** | Broker field defaults to a **hostname**, not an IP — survives DHCP lease changes via router-registered local DNS | settled |
| D36 | Generic push API (`wigwag set …`) for CI, PR bots, cron | settled |
| D37 | Credentials in gitignored `firmware/credentials.conf` + `host/.env` | settled |
| **D47** | **Works identically in the VS Code extension and the terminal** — hooks are a CLI-level feature and the extension bundles the CLI, so one implementation covers both | settled |
| **D48** | **8 KB SRAM budget is a gated requirement**, enforced by `ram_report` in CI, not hoped for | settled |

### Process

| # | Decision | Status |
|---|---|---|
| D38 | Zephyr **mainline** first; Zephyr4Microchip as fallback for driver gaps | settled |
| D39 | Host software + simulated-module firmware before the PCB. *`native_sim` replaced by real cnano hardware — see D66* | settled |
| D40 | Buy `EV10P22A` + `EV72E72A` to validate AT commands before committing layout | settled |
| D41 | CAD = OpenSCAD, parametric, headless STL render | settled |
| D42 | RF clearances encoded as OpenSCAD `assert()`s | settled |
| **D49** | **PL10 TCC PWM devicetree enablement — PASSED on hardware.** Devicetree only, no driver work: `firmware/boards/pic32cm_pl10_cnano.overlay`. LED0 breathes steadily; 500 Hz carrier confirmed on an oscilloscope. TCC0 WO0/WO1/WO2 is safe to commit to the PCB | settled |
| **D64** | **west workspace top directory = repo root**; `firmware/west.yml` is the manifest repo and `firmware/` the app (ADR-0014) | built |
| **D65** | **Pinned to an explicit mainline SHA with a 4-module `name-allowlist`**, `--depth=1`; SDK 1.0.1 `arm-zephyr-eabi` only. ~1 GB tree + 1.4 GB SDK (ADR-0014) | built |
| **D66** | **The module simulator runs against real cnano hardware, not `native_sim`** — Zephyr's POSIX arch is documented as not working on macOS. AT core stays Zephyr-free so it unit-tests under plain clang (ADR-0015) | settled |
| **D67** | **Zephyr SDK 1.0.1 ships no macOS host tools** (installer skips them), so `cmake`/`ninja`/`dtc`/`gperf` come from Homebrew | settled |
| **D68** | **`max-bit-width = <16>` for PL10 TCC0**, not the JH01's 24 — `TCC_COUNT_Msk` is `0x0000FFFF` | settled |
| **D69** | **Run `pyocd pack update` before `pack install`** — pyOCD resolves versions from a cached index it never refreshes, and DFP 1.4.418 faults on every connect where 1.5.437 works | settled |
| **D71** | **No dependency on MPLAB X or `ipecmd`.** pyOCD is the only supported flashing path: `west` + Zephyr SDK + pyOCD, DFP from the public CMSIS index, no vendor IDE required. Verified by a full erase/program round trip | settled |
| **D70** | **The lamp renderer schedules on absolute deadlines, not `k_msleep`** — measured 1297 ms against an intended 1250 ms, drift by construction | settled |

---

## Footprint plan — the 8 KB question

The interesting engineering constraint. Estimated budget, **to be measured not assumed**:

| Item | Est. RAM |
|---|---|
| Zephyr kernel + main thread | ~2.5 KB |
| 3 threads (AT client, lamp render, link supervisor) @ 768 B | ~2.3 KB |
| UART RX ring + AT line buffer (bounded, 256 B each) | ~0.5 KB |
| MQTT payload parse + lamp/link state | ~0.5 KB |
| Headroom | ~2.2 KB |
| **Total** | **~6 KB of 8 KB** |

Tactics: `CONFIG_LOG` off or minimal in release; no dynamic allocation anywhere in the AT path;
one static buffer per direction; `MAIN_STACK_SIZE` tuned down from default; consider merging the
link supervisor into the lamp thread if stacks get tight.

Gate: `west build -t ram_report` and `rom_report` recorded in the journal at each milestone.
The escape hatch is D20, taken only on measured evidence.

**First measurements (2026-08-14, D49 spike):** 3 880 B of 8 KB (47.4 %), flash 13 972 B of
60 KB. The estimate above allocated ~2.5 KB to "kernel + main thread"; the reality is that
**3 766 of 3 878 B is kernel stacks** — `z_interrupt_stacks` 2 048 B, `z_main_stack` 1 024 B,
`z_idle_stacks` 256 B — versus 66 B for all drivers combined. Sizing those three down (512/512/128)
measures **1 704 B, 20.8 %**. Not adopted yet: stack sizing needs peak-usage evidence, not
optimism. But the 8 KB question now has a real answer — the budget is dominated by tunables, not
by code.

## Pin budget — SSOP-28

| Function | Pins |
|---|---|
| Power / ground / VDDIO2 (MVIO domain — must sit at 3.3 V to match the module) | ~4 |
| Reset | 1 |
| SWD (SWCLK, SWDIO) | 2 |
| Module UART TX/RX — SERCOM0 PAD[0]/PAD[1] | 2 |
| Module control — MCLR out, INTOUT in | 2 |
| Lamps — TCC0 WO0/WO1/WO2 | 3 |
| Button | 1 |
| **Used / available** | **~15 of 28** |

Comfortable margin for optional UART flow control, the module's `UART2_TX` debug line, and a
board status LED. Exact pin assignment is a Phase 3 task against datasheet §2.3.

---

## Architecture

```
Claude Code hooks  (VS Code extension or terminal — identical)
      ▼
wigwag        (CLI client, POSIX sh + nc)
      │  AF_UNIX datagram → /tmp/wigwag.sock
      ▼
wigwagd       (daemon, Python)
      │  per-session state + TTL, aggregate ERROR>WAIT>BUSY>IDLE
      ▼
mosquitto     (retained publish)
      │
      ▼  Wi-Fi
RNWF02  ──UART/AT──  PIC32CM PL10  (Zephyr)
                          │
                    TCC0 PWM → 3 FETs → 3 lamps  ·  button
```

### Topics

| Topic | Dir | Payload | Notes |
|---|---|---|---|
| `wigwag/state` | host→dev | `{"state":"WAIT","reason":"permission_prompt","sessions":2}` | retained, QoS 1 |
| `wigwag/brightness` | host→dev | `0`–`255` | retained |
| `wigwag/button` | dev→host | `{"event":"press","ms":120}` | |
| `wigwag/online` | dev→host | `1` / `0` | `0` as MQTT Last Will |

### Hook → state mapping

| Hook | Matcher | State |
|---|---|---|
| `SessionStart` | `startup`, `resume`, `clear` | `IDLE` |
| `UserPromptSubmit` | — | `BUSY` |
| `PreToolUse`, `PostToolUse` | `*` | `BUSY` (heartbeat, refreshes TTL) |
| `Notification` | `permission_prompt`, `idle_prompt`, `agent_needs_input` | **`WAIT`** |
| `Stop` | — | `IDLE` |
| `StopFailure` | `*` | `ERROR` |
| `SessionEnd` | `*` | drop session |

Hook safety is non-negotiable: always `exit 0`; never write to stdout (`UserPromptSubmit` stdout
is injected into the model's context, `SessionStart` stdout is shown to the user); works with the
daemon down; `SessionEnd` hooks share a ~1.5 s budget so it must be fire-and-forget.

---

## Repo layout

```
wigwag/
├── CLAUDE.md  JOURNAL.md  CONTEXT.md  README.md
├── docs/adr/  docs/PLAN.md
├── host/{wigwagd/, wigwag, hooks/wg-notify, settings.hooks.json, tests/}
├── firmware/
│   ├── src/{main,rnwf_at,lamp,button,link}.c
│   ├── boards/pic32cm_pl10_cnano.overlay
│   ├── boards/wigwag_rev_a/          custom board defn
│   ├── dts/pl10-tcc-pwm.overlay      the D49 spike
│   ├── prj.conf  prj_release.conf  credentials.conf.example
│   └── sim/fake_rnwf02.py
├── hardware/wigwag/                  KiCad 9, 4-layer
├── enclosure/*.scad
└── .claude/skills/journal/
```

---

## Implementation phases

### Phase 0 — Journal practice and documentation  ·  ✅ COMPLETE

1. ✅ Repo scaffold: `.gitignore`, `README.md`, `CONTEXT.md`, `CLAUDE.md`, directory tree.
2. ✅ `journal` skill at `.claude/skills/journal/` — `SKILL.md`, templates `entry.md` / `adr.md` /
   `journal-header.md`, and `journal-reminder.sh`. Written with no project specifics (D06).
3. ✅ `JOURNAL.md` created and seeded, newest-first, including the PL10/PWM error.
4. ✅ ADR-0001 … ADR-0009:
   - 0001 Host MCU is PIC32CM PL10, 28-pin — including the mistake that produced the first draft
   - 0002 RNWF02 network co-processor over deprecated `winc1500` and no-Zephyr WFI32/WBZ451
   - 0003 MQTT + retained messages as transport
   - 0004 Host-side session aggregation with TTL
   - 0005 Journal + ADR + CONTEXT + CLAUDE.md doc structure
   - 0006 Mainline Zephyr first, Zephyr4Microchip fallback
   - 0007 Fail-visible: never display a stale state
   - 0008 Smallest-part-that-runs-Zephyr as an explicit design goal, with the 8 KB budget
   - 0009 Power architecture — single 3.3 V rail, LEDs on 5 V behind FETs, MVIO/5 V VDD rejected
5. ✅ `SessionEnd` journal reminder installed in `.claude/settings.json` and verified across seven
   cases in an isolated scratch repo: warns on changed-code-without-journal (tracked and
   untracked), silent when the journal was updated, when nothing changed, when no journal exists,
   and outside a git tree. Always exits 0 (D17).

Remaining Phase 0 loose end: nothing is committed to git yet (D16).

### Phase 1 — Host software (no hardware needed)
6. `brew install mosquitto`, local broker with username/password.
7. `wigwagd`: AF_UNIX listener, session table + TTL, priority aggregation, retained publish,
   subscribe to `wigwag/button`, coalesce heartbeat bursts.
8. `wg-notify` hook client: POSIX sh, `sed` out `session_id`, one datagram via `nc -U`.
9. `wigwag` CLI: `set|get|status|watch`.
10. Install hooks; verify with `mosquitto_sub -t 'wigwag/#' -v` against a real session driven
    through IDLE → BUSY → WAIT → IDLE. Confirm identical behavior in the VS Code extension and
    a terminal session (D47).
11. Tests: aggregation priority, TTL expiry, hook latency, daemon-down path.

### Phase 2 — Firmware
12. ✅ Zephyr workspace, `west init`, mainline + `hal_microchip` (D64, D65, ADR-0014); `blinky`
    builds for `pic32cm_pl10_cnano`.
13. **D49 spike, do this early — it gates the PCB:** get TCC0 PWM running on PL10. Add TCC nodes
    + pinctrl to the SoC/board devicetree, bind `pwm_mchp_tcc_g1`. If mainline resists, try
    Zephyr4Microchip; if it's genuinely missing, write it and upstream it. Success = a breathing
    LED on a PL10 Curiosity Nano.
    Done in `firmware/boards/pic32cm_pl10_cnano.overlay` — mainline needed **no** change, since
    the whole gap was the missing devicetree nodes. Visual confirmation still outstanding.
14. `rnwf_at.c`: bounded ring-buffer line assembly, request/response with timeouts, unsolicited
    result dispatch, connect state machine (reset → AT → Wi-Fi → MQTT → subscribe) with backoff.
    Core written free of Zephyr headers so it unit-tests under plain clang on macOS (D66).
15. `sim/fake_rnwf02.py` — AT server + `paho-mqtt` bridge on the host, wired to the cnano's
    SERCOM0 through a 3.3 V USB-UART adapter, tested against the real broker. **Not `native_sim`:**
    Zephyr's POSIX architecture does not work on macOS (D66, ADR-0015).
16. `lamp.c`: 3 PWM channels, ~100 Hz render, gamma-corrected steady/breathe/blink/flicker.
17. `button.c` (GPIO IRQ + debounce) and `link.c` (supervision → amber flicker, WDT).
18. Hardware bring-up: `EV10P22A` + `EV72E72A`, five jumpers (TX, RX, MCLR, 3V3, GND).
19. **Record `ram_report`/`rom_report`** in the journal; confirm the 8 KB budget holds (D48).

### Phase 3 — PCB (KiCad 9, 4-layer)
20. Schematic: PL10 SSOP-28 + RNWF02PC + MCP1826 + USB-C power-only + 3 lamp channels with
    low-side FETs off the 5 V rail + button + SWD header + module `UART2_TX` test point +
    `STRAP1`/`STRAP2` pulled low for UART host mode + VDDIO2 tied to 3.3 V.
21. Stackup: signal / GND plane / PWR plane / signal. Module at board edge, ground-plane edges
    aligned, antenna keepout, bulk capacitance at the module for TX peaks.
22. ERC/DRC, 3D export to drive the enclosure model, BOM, fab outputs, order.

### Phase 4 — Enclosure (OpenSCAD)
23. Parametric: 10 mm lens bores, lamp pitch, visor length, wall thickness, PCB outline and
    standoffs from the KiCad export, USB-C cable exit, press-fit translucent lens caps.
24. RF constraints as `assert()`s: ≥10 mm plastic clearance at the antenna end, no metal
    fasteners within 31.75 mm, magnets at the base only.
25. Headless STL render via `Makefile`, print-settings notes.

### Phase 5 — Integration
26. Tune brightness, breathe rate, red-blink escalation, reconnect timing.
27. Soak across concurrent sessions; confirm no stuck states, correct TTL expiry.
28. README with photos, wiring, provisioning; final JOURNAL + ADR pass.

---

## Shopping list

| Part | What | Stock |
|---|---|---|
| `PIC32CM6408PL10028-I/SS` | MCU, SSOP-28, 64 K/8 K | 1222 |
| `PIC32CM6408PL10028-I/SP` | same die, **SPDIP-28 through-hole** for socketed bring-up | 285 |
| `RNWF02PC-I/100` | Wi-Fi module, PCB antenna, Trust&Go | 432 |
| `MCP1826S-3302E/DB` | 1 A LDO, SOT-223 | — |
| **`EV10P22A`** | PL10 Curiosity Nano (identical 64 K/8 K SoC) | — |
| **`EV72E72A`** | RNWF02 Add-on Board | 268 |
| — | 3× 10 mm diffused LEDs (R/Y/G), 3× SOT-23 N-FET, resistors, tactile switch, USB-C receptacle | — |

Debuggers already on hand (PICkit 5, PICkit Basic, Atmel-ICE, J-Link) all work via pyOCD/J-Link.

---

## Verification

- **Host, no hardware:** `mosquitto_sub -t 'wigwag/#' -v` shows IDLE → BUSY → WAIT → IDLE against
  a real session; two concurrent sessions confirm WAIT beats BUSY; verified in both the VS Code
  extension and a terminal session.
- **Firmware, no module:** `fake_rnwf02.py` on the host against the live broker, driving the real
  cnano over SERCOM0 (ADR-0015). The AT core's parser and state machine additionally unit-test
  under plain clang with no hardware and no Zephyr.
- **D49 gate:** a breathing LED on PL10 hardware proves TCC PWM before any layout is committed.
- **Footprint gate:** `ram_report` ≤ 8 KB with margin, recorded per milestone.
- **Hook safety:** hooks emit nothing on stdout and `exit 0` with the daemon stopped.
- **On hardware:** `west flash -r pyocd`; kill the broker → amber flicker within 10 s; restore →
  retained message restores correct state; power-cycle → correct state with no host action.
- **Enclosure:** `make` renders STLs; `assert()`s fail loudly on RF clearance violations.

## Open questions

| # | Question | Assumption if unanswered |
|---|---|---|
| D29 | *Resolved* — TLS is automatic off-loopback (ADR-0011). Device-side TLS with Trust&Go remains future work. | — |
| — | Is module firmware **v3.0+** (needed for `AT+CFGCP`)? | Assume yes; **verify at Phase 2 bring-up** and record the version in the journal |
| — | Does the RNWF02 provisioning service behave as documented? | Assume yes; **Phase 2 spike** alongside D49 |
| D17 | Install hooks into `.claude/settings.json`? | Yes, project-local, journal reminder now and state hooks in Phase 1 |

## Future directions (noted, not planned)

- **Other AI producers.** The daemon, topics, and CLI are vendor-neutral; adding another tool
  means writing an adapter that reports session states, not touching firmware or protocol.
  Claude Code alone is sufficient for now.
- **TLS + Trust&Go** for the MQTT link, using the module's on-board secure element.
- **Secure credential storage** using the module's Trust&Go element, and EU RED Delegated Act
  compliance if wigwag ever became a product rather than a personal device (see ADR-0012).
- **Upstreaming** the PL10 TCC PWM devicetree support to mainline Zephyr, if D49 shows it missing.
