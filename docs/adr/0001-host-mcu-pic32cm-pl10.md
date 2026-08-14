# ADR-0001 — The host MCU is PIC32CM PL10, in the 28-pin package

- **Status:** Accepted
- **Date:** 2026-08-14

> Supersedes an unpublished same-session draft that selected PIC32CM JH01 on the strength of a
> false claim that PL10 lacked PWM. See "The mistake that produced the first draft" below and
> the 2026-08-14 journal entry. The draft was never committed, so it was replaced rather than
> formally superseded.

## Context

Three requirements had to hold at once: Microchip silicon (hard requirement), Zephyr RTOS, and
Wi-Fi (ADR-0002 covers the radio). Zephyr is the binding constraint — it eliminates most of the
catalogue:

- **No 8-bit AVR support at all.** This disqualifies the AVR64DU32, which would otherwise have
  been ideal for a USB-tethered build (native crystal-less USB FS device, runs bus-powered from
  5 V VBUS with an internal 3.3 V USB VREG, no bridge chip, minimal BOM).
- **No MIPS SoC support**, so PIC32MX/MZ — including the otherwise-attractive single-chip
  WFI32E01 — is out.
- Supported Microchip families in mainline: `soc/microchip/{mec,miv,pic32c,pic64,sam,smartfusion2}`.

Within `pic32c`, mainline has ten boards: `pic32ck_gc01_cult`, `pic32cm_gc00_cpro`,
`pic32cm_jh01_cnano`, `pic32cm_jh01_cpro`, `pic32cm_pl10_cnano`, `pic32cm_sg00_cpro`,
`pic32cx_sg41_cult`, `pic32cx_sg61_cult`, `pic32cz_ca80_cult`, `pic32cz_ca90_cult`.

The application's needs are tiny: one UART, three PWM channels, one GPIO input. An explicit
secondary goal (ADR-0008) is to find the *smallest* Microchip part that usefully runs Zephyr,
which argues for the floor of that list rather than a comfortable middle.

## Decision

**PIC32CM PL10**, target part **`PIC32CM6408PL10028-I/SS`** — SSOP-28, Cortex-M0+ @ 24 MHz,
64 KB flash, **8 KB SRAM**. 1222 in stock, 5-week lead.

Supporting choices:

- **`PIC32CM6408PL10028-I/SP`** — the same die in **SPDIP-28 through-hole**, for a socketed
  breadboard prototype with no SMT. 285 in stock.
- **Dev board `EV10P22A`** (PL10 Curiosity Nano), which carries `PIC32CM6408PL10048` with
  *identical* 64 KB/8 KB memory — so footprint measurements transfer directly to the target,
  rather than flattering us the way a superset part would.
- **Escape hatch:** `PIC32CM1216JH01032` (32-pin, 128 KB/16 KB), taken only on measured
  `ram_report` evidence.

Verified capability, at all three layers:

| Layer | Status |
|---|---|
| Silicon TC/TCC PWM | ✅ datasheet §23.2 — TCC0 with NPWM, DPWM, dual-slope and critical PWM |
| TCC0 waveform outputs | ✅ WO0–WO3 available on the 28-pin part (§2.3) — 3 lamps + 1 spare |
| Mainline Zephyr PWM driver | ✅ `drivers/pwm/pwm_mchp_tcc_g1.c`, `pwm_mchp_tc_g1.c`, `Kconfig.mchp` |
| PL10 board port enabling PWM | ❌ not wired into that board's devicetree — this is our work |
| Zephyr flashing | ✅ pyOCD is the default runner; MPLAB IPE is the alternative |
| 5 V tolerance | ✅ VDD abs max +6.5 V, VOL/VOH characterized at 5.5 V (unused — see ADR-0009) |

Pin budget on SSOP-28 is roughly 15 of 28 used, so the small package is not a squeeze:
~4 power/ground, 1 reset, 2 SWD, 2 module UART (SERCOM0 PAD[0]/PAD[1]), 2 module control
(MCLR, INTOUT), 3 lamps (TCC0 WO0–WO2), 1 button.

## The mistake that produced the first draft

Worth recording, because the failure mode is easy to repeat. The Zephyr **board** documentation
for `pic32cm_pl10_cnano` lists supported features as CPU, UART, GPIO, DMA, flash, SRAM, systick,
NVIC, LEDs, pinctrl and clock control — no PWM, no ADC. That table was read as evidence that
*PL10 cannot do PWM*, and PL10 was rejected on it.

The table describes only what **that board's port currently enables**. It says nothing about the
silicon, and nothing about which drivers exist in the tree. The silicon has a full TCC, and the
driver has existed in mainline all along. Reviewer pushback ("PL10 should have the same TC and
TCC peripherals as other SAM / PIC32C devices") was correct.

Lesson, now recorded in `CLAUDE.md`: check three layers separately — silicon, driver, board
enablement — and say which you actually checked. Only "no driver anywhere" is a blocker; a gap
at the board layer is devicetree work, and worth upstreaming.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **PIC32CM JH01** (`PIC32CM5164JH01048`, 512 K/64 K) | The first draft's choice. Richest Zephyr driver set of any Microchip board (ADC, TC, TCC, DAC, DMA, RTC, WDT, UART, pinctrl, RSTC) and zero porting work — but 48–64 pins and 8× the RAM we need, which defeats ADR-0008. Retained as the escape hatch in its 32-pin/16 KB form. |
| **AVR64DU32** | Better than every ARM option on BOM and USB integration, but Zephyr has no AVR architecture support. Revisit only if the Zephyr requirement is dropped *and* the link becomes USB. |
| **PIC32CX SG41 / PIC32CZ CA80** | M4F/M7 with up to 256 KB SRAM in 100–128-pin packages. Gross overkill and a needlessly hard board. |
| **ATSAMD21** | Most battle-tested Atmel target in Zephyr (Arduino Zero, Feather M0, `samd21_xpro`). Rejected for being older silicon with no advantage here; PIC32CM's Zephyr coverage is now good. |
| **PIC32MZ-W1 / WFI32E01** | Single-chip Wi-Fi MCU — would collapse the BOM — but MIPS, and Zephyr has no PIC32MZ SoC support. |
| **PIC32CX-BZ2 / WBZ451** | Cortex-M4F with BLE, but no upstream Zephyr SoC support. |

## Consequences

**Accepted costs**
- **8 KB SRAM is the real constraint.** Estimated need is ~6 KB; that estimate is unproven and
  must be measured. ADR-0008 makes it a gated requirement with a defined escape hatch.
- **We own the PL10 TCC PWM devicetree enablement** (plan item D49). Scheduled early in Phase 2
  precisely because it gates the PCB: a breathing LED on an `EV10P22A` is the go/no-go signal
  before layout is committed.
- 24 MHz M0+ with no FPU. Irrelevant for this workload.

**Benefits**
- The smallest Zephyr-capable Microchip part, which is the point (ADR-0008).
- SSOP-28 is trivially hand-solderable; SPDIP-28 removes SMT from prototyping entirely.
- Dev board memory is identical to the target, so footprint numbers never lie to us.
- Every debug probe already on hand (PICkit 5, PICkit Basic, Atmel-ICE, J-Link) works.
- The TCC enablement is a plausible upstream contribution rather than throwaway work.

**Revisit if** measured `ram_report` shows 8 KB is genuinely insufficient (→ D20), or the D49
spike reveals the TCC driver cannot be bound to PL10 without writing a new driver.
