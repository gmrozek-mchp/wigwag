# Journal

Append-only development log. **Newest entry first.** Durable decisions live in
[`docs/adr/`](docs/adr/) and are referenced from here as `ADR-NNNN`.

Entries record what was done, why, and — importantly — what was tried and rejected.

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
