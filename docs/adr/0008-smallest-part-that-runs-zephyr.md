# ADR-0008 — "Smallest Microchip part that usefully runs Zephyr" is an explicit design goal

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

The application is genuinely tiny: one UART, three PWM channels, one GPIO input, and a state
machine. Because the RNWF02 owns Wi-Fi, TCP/IP, TLS and MQTT (ADR-0002), the host MCU has no
network stack at all.

That leaves enormous latitude in part selection, and the default engineering instinct is to take
a comfortable part — the first plan draft chose a 512 KB flash / 64 KB SRAM device, roughly 8×
more RAM than needed, on the reasonable grounds that headroom removes risk.

But there is a second, more interesting question available for free: **how small a Microchip part
can usefully run Zephyr?** That is worth knowing independently of this gadget, and it is the kind
of result that generalises — to other people evaluating Zephyr on small Microchip parts, and to
the argument about whether Zephyr is viable below the usual thresholds.

Choosing the small part changes the character of the project. Footprint stops being an
afterthought and becomes a design constraint with teeth.

## Decision

**Target the smallest Zephyr-supported Microchip part and treat its 8 KB SRAM as a gated
requirement, not an aspiration.**

Target: `PIC32CM6408PL10028` — 28 pins, 64 KB flash, **8 KB SRAM** (ADR-0001). This is the floor
of the mainline-supported Microchip range; nothing smaller has a Zephyr board.

Working budget, explicitly an estimate to be replaced by measurements:

| Item | Est. RAM |
|---|---|
| Zephyr kernel + main thread | ~2.5 KB |
| 3 threads (AT client, lamp render, link supervisor) @ 768 B | ~2.3 KB |
| UART RX ring + AT line buffer (bounded, 256 B each) | ~0.5 KB |
| MQTT payload parse + lamp/link state | ~0.5 KB |
| Headroom | ~2.2 KB |
| **Total** | **~6 KB of 8 KB** |

Enforcement, which is what makes this a requirement rather than a wish:

- **Measure at every milestone.** `west build -t ram_report` and `rom_report`, with the numbers
  recorded in `JOURNAL.md`. No milestone is "done" without them.
- **No dynamic allocation** anywhere in the AT path. Bounded static buffers only, one per
  direction.
- **Justify additions in RAM.** Before adding a Kconfig option, subsystem or thread, state its
  cost.
- **Development happens on `EV10P22A`** (PL10 Curiosity Nano), whose `PIC32CM6408PL10048` has
  *identical* 64 KB/8 KB memory. A superset dev board would flatter the measurements and hide the
  problem until the PCB existed.
- **Defined escape hatch:** `PIC32CM1216JH01032` (32-pin, 128 KB/16 KB), taken **only** on
  measured evidence — never on a hunch that it'll be tight.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **Comfortable part (JH01, 64 KB SRAM)** | The first draft's choice, and the lower-risk engineering answer: no footprint work, no porting, richest Zephyr driver coverage. Rejected because it forfeits the interesting result and the discipline. Kept as the escape hatch in its smaller 16 KB form. |
| **Small part, but no enforcement** | "Use the 8 KB part and be careful." Rejected because unmeasured budgets are discovered late, at the worst moment — typically after a PCB exists. Without `ram_report` gates this is a wish, not a requirement. |
| **Smaller still (non-Zephyr Microchip parts)** | There are far smaller Microchip devices, but none with Zephyr support, and Zephyr is a project requirement. 8 KB genuinely is the floor of the question being asked. |
| **Bare metal or Harmony 3 on a tiny part** | Would fit in a fraction of the resources and is the best-supported path for this silicon. Rejected: it answers a different, already-known question. The point is Zephyr's floor. |

## Consequences

**Accepted costs**
- **Real risk that 8 KB does not fit.** The ~6 KB estimate is unproven. Mitigated by measuring
  early — the estimate gets replaced by a real number in Phase 2, well before the PCB.
- Ongoing footprint discipline: careful stack sizing, logging off or minimal in release builds,
  scrutiny of every Kconfig addition.
- Some comfort features may have to go — verbose logging, a shell, generous stacks. The link
  supervisor may need merging into the lamp thread.
- We own the PL10 TCC PWM devicetree enablement (D49) that a better-supported part would have
  given us free.

**Benefits**
- Produces a genuinely useful, generalisable answer about Zephyr on small Microchip parts.
- Forces good embedded habits — bounded buffers, no allocation, measured footprint — that make
  the firmware better regardless of part.
- Cheapest possible BOM, smallest package, and SPDIP-28 availability makes a socketed, zero-SMT
  prototype possible.
- A plausible upstream contribution (PL10 PWM enablement) falls out of the work.

**Revisit if** measured `ram_report` shows 8 KB is insufficient with no reasonable economies left
— then take the escape hatch, and **record the measured numbers in the journal**, because
"Zephyr plus an AT client does not fit in 8 KB, here is where it went" is itself the answer to
the question this ADR poses.
