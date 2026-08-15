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
| **D56** | **Commissioning: compile-time Kconfig for v1; SoftAP provisioning via the module's own service for v1.1** (ADR-0012). *Superseded as the primary path by D105 — configuration is set over the console and stored, and Kconfig now supplies only defaults. ADR-0012 stays accepted for the phone-only case, and D58's long-press stays reserved for it* | settled |
| **D57** | **USB commissioning is impossible** — PL10 has no USB peripheral (Table 8-1). Would need an MCP2221A bridge and would reverse D24 | settled |
| **D58** | Long-press the existing button enters provisioning mode; all three lamps cycle so the mode is unmistakable | settled |
| **D59** | PCB must break out host UART (SERCOM0) to pads/header — bench commissioning at zero BOM cost | settled |
| **D60** | **Broker address is configured during provisioning, not auto-discovered** (ADR-0013) | settled |
| **D61** | **mDNS/DNS-SD `_mqtt._tcp` rejected** — RNWF02 has no mDNS and mosquitto does not advertise (both verified) | settled |
| **D62** | **`AT+CFGCP` persists config to module NVM** (firmware v3.0+), so broker settings survive reboots and are entered once ever | settled |
| **D63** | Broker field defaults to a **hostname**, not an IP — survives DHCP lease changes via router-registered local DNS | settled |
| D36 | Generic push API (`wigwag set …`) for CI, PR bots, cron | settled |
| D37 | Credentials in gitignored `firmware/credentials.conf` + `host/.env`. On the device these are now **defaults only** (D108); `firmware/CMakeLists.txt` merges the file automatically when present, and `credentials.conf.example` documents it | built |
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
| **D72** | **The cnano's LED0 is active low**, so the dev-board overlay uses `PWM_POLARITY_INVERTED`. Mainline's board dts says `GPIO_ACTIVE_HIGH` — an upstream bug. **The PCB is active high** (low-side FETs, D26), so `lamp.c` must take polarity from devicetree, never inherit the board's | settled |
| **D73** | **Local patch to the pinned tree**: mainline's `microchip,{tc,tcc}-g1-pwm.yaml` name the third `pwm-cell` `polarity`, so `PWM_DT_SPEC_GET()` silently discards polarity. Fixed in `firmware/patches/`, to be upstreamed (ADR-0006 rung b) | settled |
| **D74** | **The firmware prints its resolved PWM flags at boot**, because a devicetree value that is silently ignored is otherwise invisible | settled |
| **D75** | **Link supervision needs positive liveness in both failure domains**, not just absence of bad news. Demonstrated on hardware: with its module killed, the device sat in `READY` reporting `LINKED` indefinitely. `link.c` polls the module with a bare `AT` *and* subscribes to `wigwag/host_online` — the module poll cannot see a dead broker, and the topic cannot see a dead module | settled |
| **D76** | **Module UART = SERCOM0 on PA04 (PAD0/TX) / PA05 (PAD1/RX)**, mux C, reached via a `wigwag,module-uart` chosen node. Everything else is taken by the debugger, the crystal footprint, the touch button, or the MVIO domain | settled |
| **D77** | **Interrupt-driven UART receive into a 256-byte static ring**, drained every 10 ms; overruns counted and reported. Polled receive drops bytes at 115200 with no deep FIFO | settled |
| **D78** | **Stack sizes are measured, never guessed** — `firmware/prj_stacks.conf` + thread analyzer, and the exercise must provoke every path or the number is meaningless. Peaks: main 860/1024, lamp 348/512, idle 92/256, ISR 264/2048 | settled |
| **D79** | **`CONFIG_ISR_STACK_SIZE=1024`** on that evidence (4× the measured 264 B), returning 1 KB — 12.5 % of the part's SRAM. RAM 66.7 % → 54.2 % | settled |
| **D80** | **No MPU on PL10**, so `HW_STACK_PROTECTION` is unavailable; `STACK_SENTINEL` is the measurement-build substitute and detects overflow after the fact | settled |
| **D82** | **XOSC32K disabled on the cnano** — it owns PA24/PA25 (XTAL32K1/2) and overrides their GPIO/peripheral function per datasheet §13.4.2.2, silently killing two lamps. The board's crystal is also disconnected by default, so the driver was waiting on an oscillator that could never start | settled |
| **D83** | **Gamma lives in `lamp.c`, not the renderer** — it is pure arithmetic, and on the Zephyr side it shipped a bug where every level below 41 rendered as off. Tests now cover monotonicity, endpoints and dead zones | settled |
| **D84** | **Power-on lamp test**: each lamp to full for 400 ms, then all three. A dim steady state proves nothing; full brightness would have exposed both of the above immediately | settled |
| **D86** | **The button is polled at 10 ms, not interrupt-driven** — PL10's devicetree has no `eic` node, and a press lasts ~100 ms while debounce needs tens of ms anyway, so polling *is* the debounce. The device never sleeps (D24), so a pin interrupt buys nothing | settled |
| **D92** | **`wigwag/online` birth message is keyed on AT `READY`, not the link condition** — the topic means "connected to the broker", which stays true when the host daemon dies. Retained, once per connection. The Last Will is verified firing on hardware | settled |
| **D88** | **Brightness is two layers**: `wigwag/brightness` is runtime *preference* over MQTT (retained, 0-255), while per-lamp `gain` is *calibration* in devicetree beside polarity. One mechanism would let preference clobber calibration | settled |
| **D89** | **Both scales apply to the perceptual level, not the duty** — gamma cubes, so scaling duty would make half-brightness look like ~79 % | settled |
| **D90** | **Brightness cannot silence the fail-visible pattern** — it floors at `LAMP_FAULT_MIN_BRIGHTNESS` (96), because a device dimmed to nothing is indistinguishable from one switched off (Rule 4, ADR-0007). Verified on hardware at brightness 0 | settled |
| **D91** | **App-local devicetree binding `wigwag,lamps`** — `pwm-leds` allows only `label` and `pwms` on children. Not for upstream; a local `vendor-prefixes.txt` registers the prefix | settled |
| **D87** | **Presses are published unretained and dropped if the link is down** — a press is an event, not a state, and one delivered late would misreport when it happened (D35) | settled |
| **D85** | **`LAMP_IDLE_DIM = 128`** (12.6 % duty), chosen by eye from a hardware sweep. Revisit with real 10 mm lamps at 20–60 mA; `wigwag/brightness` is the place for per-desk trimming | settled |
| **D81** | **The AT service loop runs on the main thread**, renamed `at` for reports. A dedicated thread costs ~600 B (~7 % of SRAM) and would *not* reduce peak depth, since main's peak is `max(init, loop)` rather than their sum. Revisit when a second context needs the AT client, when the WDT needs multi-thread liveness, or if credentials lengthen its commands. *The WDT clause is now resolved — see D93: multi-thread liveness needed check-ins, not threads* | settled |
| **D93** | **Watchdog feeding is earned, not automatic** — both the AT loop and the render thread must check in with `wdog.c` within 500 ms or the device stops feeding and reboots ~2 s later. A watchdog fed from one loop certifies half the system and silently vouches for the other half (ADR-0016). Demonstrated on hardware with a deliberately wedged render thread | settled |
| **D94** | **Detection budget: 500 ms staleness + 2 s hardware window = ~2.5 s**, inside D34's 10 s. Normal mode, no closed window — a minimum window catches a task running too fast, which is not a failure this device has, and would let the feed itself reset a healthy device | settled |
| **D95** | **`CONFIG_HWINFO` is not used; the firmware reads `RSTC.RCAUSE` itself** and leaves the `rstc` node disabled. `hwinfo_mchp_g1.c` reads PL10's RCAUSE at offset 0 with JH's bit positions — it returns 0 for every reset, and would call a watchdog reset `RESET_PIN` at the right address. Upstream bug 4; verified `rcause 0x10` on a real watchdog reboot | settled |
| **D96** | **PL10 flash support is our own driver, as an out-of-tree Zephyr module** at `firmware/modules/pic32cm-pl-nvmctrl/` — not a tree patch (`west update` reverts those silently) and not app-local (it belongs to the SoC). Mainline's two Microchip flash drivers target other peripheral revisions; the g1 one would issue commands PL10 does not define (ADR-0017) | built |
| **D97** | **Compatible `microchip,pic32cm-pl-nvmctrl`, family-named**, following the family's own upstream precedent `microchip,pic32cm-pl-clock`. Geometry read from `PARAM` at runtime (`NVMP` pages of `8 << PSZ`) and cross-checked against devicetree, so one binding covers 6408PL and 1216PL and a wrong `reg` fails loudly | built |
| **D98** | **Measured flash timing: page erase 10.1 ms, word write ~0.13 ms** at 24 MHz. Erase and write stall the CPU including interrupts (§26.4.2.3.1), so a single `erase()` call is capped at ~**49 pages (~24 KB)** by ADR-0016's 500 ms watchdog budget. The 4 KB storage partition is 8 pages / 81 ms. `FLMPER` would cut that ~8x and is deferred, gated on reading `BOOTPROT` | settled |
| **D99** | **Issuing an *enable* command (`FLWR`/`FLPER`) is itself a command that clears `INTFLAG.READY`** — storing to the array before it lands sets `STATUS.PROGE` and silently does nothing. Measured `INTFLAG=0` right after writing `FLPER`. The first driver failed every erase this way while writes worked by accident of a `memcpy` in the gap | settled |
| **D100** | **A bootloader, if built, is a developer convenience and bare-metal** — a stripped-down Adafruit-derived SAM-BA monitor targeting Zephyr's in-tree `bossac` runner, not MCUboot and not a Zephyr app. UF2 itself is impossible: PL10 has no USB peripheral, and the MCP2221A cannot lend one. `__VTOR_PRESENT = 1`, so a relocated app is viable (`docs/usb-serial-and-bootloader.md`) | spike |
| **D101** | **The 28-pin target package does not have the cnano's pins.** `PB02` (lamp WO2) and `PB00`/`PB01` (console) do not exist on `PIC32CM6408PL10028` at all. Everything still fits — TCC0 WO0/WO1/WO2 on `PA00/01/02`, `PA08/09/10`, or `PA24/PA25/PA18`; SERCOM1 on `PA00/PA01` (mux D) or `PA10/PA11` (mux C) — but D49's specific mapping is cnano-only and the Phase 3 pin assignment must be redone against the 28-pin pinout | settled |
| **D102** | **Fit an `MCP2221A` USB-serial bridge** on the existing USB-C connector's D+/D-, `SERCOM1`, `VUSB` tied to 3V3 alongside `VDD`. Spends PL10's last SERCOM, so the product will never have I2C. Populated on the first build; **the console comes free** — a devicetree assignment, no firmware (ADR-0018) | settled |
| **D103** | **`GP2` = `USBCFG`, `GP0` = `SSPND`, wired to MCU inputs** — hardware evidence that a live USB host is attached and that it has not suspended. Not decoration: they are what makes transport selection a reading rather than a guess, since a charger does not enumerate | settled |
| **D104** | **Built.** **One transport at a time, selected from `USBCFG` plus a host heartbeat.** USB wins when a live host is present, else Wi-Fi/MQTT as today; no credentials needed on the wired path. Both transports live at once was rejected — two concurrent trust evaluations and a fail-visible rule spanning both is the complexity shape that produced D75 (ADR-0018) | settled |
| **D105** | **Configuration over the console, hand-rolled in three layers** — `lineedit.c` (character editing, the replaceable layer), `cmd.c` (vocabulary and validation), `console.c` (effects). Zephyr's shell **does not link**: measured `RAM overflowed by 464 bytes`, ~4 KB against 3624 free. embedded-cli was assessed and declined — unpublished footprint, history-dominated, and an interactive prompt model that fights the machine half of a shared wire (ADR-0019) | built |
| **D106** | **Backspace kept, history and completion not.** Retyping a 63-character passphrase after one typo is the real pain, and it costs five lines. Arrow keys are made *inert* — without an escape filter, Up inserts a literal `[A` into a passphrase | settled |
| **D107** | **Over-long input is refused, never truncated**, at both layers (`LINEEDIT_TOO_LONG`, `settings_apply`). A silently shortened passphrase is stored, looks right, and fails association with nothing to point at — Rule 4 applied to input | settled |
| **D108** | **Settings persist via NVS in the 4 KB storage partition; stored values beat build-time defaults.** NVS rather than a hand-rolled blob: the hard part is consistency across a power cut and even sector wear, not storing bytes. Costs **302 B of RAM** for the string cache, because NVS returns copies rather than addresses into mapped flash | built |
| **D109** | **`set` stages, `save` commits, `brightness` and `gain` also apply immediately.** Calibration is judged by eye, so it must be visible before it is stored; a half-typed network stays recoverable. Replies say `(not saved)` so the distinction is visible rather than remembered | settled |
| **D110** | **Secrets are never printed back** — `show` reports `<set>`/`<unset>` for the Wi-Fi passphrase and broker password, and unknown keys default to secret so the failure direction is printing less | settled |
| **D111** | **The wired path demands a *periodic* `host on`, within 10 s.** Over MQTT `wigwag/host_online` is retained once with `0` as the Last Will, so the broker holds the value and reports the death; a serial line has neither, and `USBCFG` cannot help because a machine whose daemon crashed still enumerates. Five missed beats at the daemon's existing 2 s tick, and the same 10 s budget as D34. The daemon sends it from its existing 2 s loop (`SerialPublisher`) | built |
| **D112** | **USB outranks a healthy Wi-Fi link, and a quiet host goes fail-visible even while Wi-Fi is up.** Untrusted immediately on staleness, then a 5 s release window before handing back — switching straight to a possibly-staler retained MQTT state would trade a known unknown for an unknown one. An orderly `host off` or a deasserted `USBCFG` skips the window. Verified on hardware | built |
| **D113** | **No SSID configured means the AT client never starts.** Previously it reset, timed out and backed off forever against a network named `""`. That is the wired variant's normal state, and it is the concrete form of ADR-0018's "no credentials needed on the wired path" | built |
| **D114** | **`pyserial` on every platform, lazily imported, as an optional extra** (`wigwagd[serial]`) rather than `termios` on POSIX and `pyserial` on Windows. A split backend would make Windows the *untested* path while development happens on macOS, and it saves nothing — Windows needs the dependency either way. Lazy import keeps D31's property that the test suite runs with nothing installed (ADR-0020) | built |
| **D115** | **Serial port discovery is opt-in and refuses to guess.** `port = "auto"` matches the MCP2221A's factory USB identity, VID `0x04D8` / PID `0x00DD` (datasheet Registers 1-5 to 1-8); zero or several matches is an error. A daemon that silently picked the first of two serial ports would eventually drive somebody's 3D printer | built |
| **D116** | **Reopens D103.** `USBCFG`/`SSPND` buy less than ADR-0018 claimed: the device is USB-powered (ADR-0009), so an unplugged cable is a power-off rather than an observable deassert, and a charger simply produces no bytes — so received bytes are both necessary *and* sufficient. `USBCFG` reduces to a boot-latency optimisation, and `SSPND` is not even GP0's factory default (that is `LED_URx`). Worth dropping the two pins unless a reason appears | open |

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
| USB bridge UART — SERCOM1 PAD[0]/PAD[1] (D102) | 2 |
| USB bridge status — `USBCFG` in, `SSPND` in (D103) | 2 |
| **Used / available** | **~19 of 28** |

Still comfortable, with margin for the module's `UART2_TX` debug line and a board status LED.

**The 28-pin package is not the cnano (D101).** `PB00`–`PB03` do not exist on
`PIC32CM6408PL10028`, so neither the dev board's lamp pin (`PB02` = TCC0 WO2) nor its console pins
(`PB00`/`PB01` = SERCOM1) transfer. Verified options on the 28-pin part, from
`hal_microchip/.../pio/pic32cm6408pl10028.h`:

| Function | 28-pin options |
|---|---|
| Lamps, TCC0 WO0/WO1/WO2 (mux F) | `PA00`/`PA01`/`PA02`, or `PA08`/`PA09`/`PA10`, or `PA24`/`PA25`/`PA18` |
| USB bridge, SERCOM1 PAD0/PAD1 | `PA00`+`PA01` (mux D), or `PA10`+`PA11` (mux C) |
| Module, SERCOM0 PAD0/PAD1 | `PA04`+`PA05` (mux C), as D76 |

Note the overlap: `PA00`–`PA02` and `PA08`–`PA11` serve both the lamps and SERCOM1, so the two
cannot both take the same block. One workable split is lamps on `PA24`/`PA25`/`PA18` with the
bridge on `PA00`/`PA01`. Exact assignment is a Phase 3 task against datasheet §2.3, and `PA20`
(SWDIO), `PA31` (SWCLK) and `PA30` (RESET) are reserved.

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
17. `button.c` (debounce + publish) and `link.c` (supervision → amber flicker, WDT). **Done** — plus
    brightness (D88–D90), `wigwag/online` (D92) and the watchdog (ADR-0016, D93–D95).
    `link.c` done and verified across all three failure domains (D75). `button.c` done and verified
    on hardware — 14 presses, no duplicates, long-hold at 3 s — but **polled rather than
    interrupt-driven** (D86), since PL10 has no `eic` node. The WDT is the remaining piece.
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
| **`MCP2221A-I/ST`** | USB-serial bridge, TSSOP-14 — no crystal, no termination resistors, CDC class driver on all three OSes (D102, ADR-0018) | 9888 |
| `MCP2221A-I/P` | same die, PDIP-14, for breadboard bring-up before layout | 2640 |
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

Two hardware questions opened on 2026-08-14 and deliberately left open; the full analysis, part
numbers, stock and design traps are in [`usb-serial-and-bootloader.md`](usb-serial-and-bootloader.md).

| # | Question | Where it stands |
|---|---|---|
| Q1 | Add an `MCP2221A` USB-serial bridge? | **Resolved 2026-08-14 — yes, fitted and populated.** D102/D103, ADR-0018 |
| Q2 | Is USB a peer to MQTT, a variant, or diagnostics only? | **Resolved 2026-08-14 — one transport at a time, chosen from `USBCFG`.** D104, ADR-0018 |

Both resolved: the wire protocol is in `CONTEXT.md`, and the dependency question is D114/ADR-0020.

| # | Question | Where it stands |
|---|---|---|
| Q3 | Should the AT client keep retrying Wi-Fi while USB holds the device? | **Open.** Observed on hardware: 947 accumulated timeouts during one end-to-end session. Wasted power and UART traffic, not a correctness fault. Pausing it would slow recovery when USB goes away, so it needs a moment's thought rather than a quick patch |
| Q4 | Drop `USBCFG`/`SSPND` from the pin plan? | **Open — D116.** Bytes turn out to be necessary and sufficient |


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
- **Upstreaming two mainline bug fixes** found during the D49 spike — the `microchip,{tc,tcc}-g1-pwm`
  binding naming its third `pwm-cell` `polarity` instead of `flags` (so polarity is silently
  discarded), and `pic32cm_pl10_cnano`'s LED0 declared active high when the hardware is active low.
  Process, maintainer routing and ready-to-send issue and commit text are in
  [`docs/upstreaming-to-zephyr.md`](upstreaming-to-zephyr.md). Neither is submitted yet.
- **Upstreaming the PL10 TCC PWM devicetree enablement** itself (the `tcc0` node, its generic-clock
  channel and pinctrl group) — currently an application overlay, and the natural home is
  `dts/arm/microchip/pic32c/pic32cm_pl/common/pic32cm_pl.dtsi` so all four PL10 packages get it.
