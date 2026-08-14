# ADR-0006 — Build on mainline Zephyr; treat Zephyr4Microchip as the fallback

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

Microchip maintains a downstream Zephyr presence at `github.com/Zephyr4Microchip`, including
`zephyr` (a fork of the upstream project), `microchip-zephyr` (described as a downstream fork for
early device enablement), `hal_microchip`, `microchip-zsdk`, `microchip-tf-psa-crypto`,
`zephyr-wireless` (Bluetooth LE and OpenThread for PIC32CX-BZx and WBZx) and `touch`. Branches
are organised per device family — the dsPIC33A work, for example, lives on a `dsPIC33A` branch.

So the newest Microchip enablement lands downstream before it reaches mainline. That is a real
advantage, and it matters here: the project depends on PL10 TCC PWM support that mainline has as
a *driver* but does not wire up in the PL10 board's devicetree (ADR-0001, plan item D49).

The tension: downstream gets us features sooner, mainline gets us stability, reviewed code,
documentation that matches, and no rebase burden.

## Decision

**Develop against mainline Zephyr.** Use `Zephyr4Microchip` as an explicit, documented fallback,
consulted in this order when a gap appears:

1. **Mainline, board layer.** Most likely case: the driver exists and only needs devicetree
   wiring. Do that work locally in `firmware/boards/` or a `dts/*.overlay`.
2. **Zephyr4Microchip.** If a driver or SoC feature is genuinely absent upstream, check the
   relevant downstream branch and take it from there, pinned via the `west` manifest.
3. **Write it.** If neither has it, implement it — and upstream it, since it is Microchip silicon
   and the author works at Microchip.

Every use of a downstream source must be recorded in the journal with the specific gap that
justified it, so the dependency is deliberate rather than accidental.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **Zephyr4Microchip as the primary tree** | Newest enablement, and arguably the "insider" choice. Rejected as the default: a downstream fork means rebasing against upstream churn, docs that may not match the tree, and a project pinned to a vendor branch's lifecycle. For a part already supported in mainline, it buys nothing we need. |
| **Mainline only, no downstream at all** | Cleanest dependency story, but leaves no answer if the D49 spike finds a real driver gap — the alternative would be writing a driver from scratch under schedule pressure. Keeping the fallback documented costs nothing until used. |
| **Pin a specific Zephyr release (e.g. 4.x LTS)** | Stability, and attractive for a product. Rejected for now because PIC32C enablement is actively landing, so we want recent `main`. Worth revisiting once the design settles. |
| **Vendor SDK instead of Zephyr (MPLAB Harmony 3, bare metal)** | Best-supported path for this silicon by a distance, with reference applications for the exact dev boards. Rejected because Zephyr is a stated project requirement — and the 8 KB exercise (ADR-0008) is specifically about Zephyr's floor. |

## Consequences

**Accepted costs**
- Tracking `main` means occasional breakage from upstream churn. The `west` manifest is pinned to
  a known-good revision and moved deliberately, not floating.
- If we do fall back downstream for one feature, the build becomes a mix of sources and the
  manifest must make that obvious.
- Upstreaming, if it comes to that, adds review latency that a downstream branch would not.

**Benefits**
- Reviewed code, matching documentation, and no rebase burden for the common case.
- The D49 spike is scheduled early precisely so this decision is tested before the PCB is
  committed, when switching trees is still cheap.
- A credible path to contributing PL10 TCC PWM support upstream, which benefits the silicon
  beyond this project.

**Revisit if** the D49 spike shows mainline cannot bind `pwm_mchp_tcc_g1` to PL10 without
substantial new code that already exists downstream — in which case take it from
Zephyr4Microchip and record the pin.
