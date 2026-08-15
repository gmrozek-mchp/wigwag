# ADR-0024 — Stackup and RF placement: the ground plane is the antenna, and bulk capacitance has a ceiling

- **Status:** Accepted
- **Date:** 2026-08-15

## Context

Plan item 21 pins down the stackup, the module placement, the antenna keepout and the bulk
capacitance. It is the first thing after the pin assignment (ADR-0023) that can invalidate a layout
degree of freedom, and the last thing before the schematic is drawn.

Four claims had to come from the module's own layout sections rather than from general practice, and
two of them changed the design:

**The RNWF02's placement and routing guidelines are more specific than "put it at the edge".**
`DS70005544C` §2.3, §2.4 and §2.6.1 give a **5.3 mm × 15.73 mm no-copper region**, require the
**module ground outline edge to coincide with the host ground-plane edge**, prefer **no board
material at all** below the antenna, require the **top layer under the module to be ground** with
≥ 10 mil stitching vias, forbid fan-out under the module or antenna, and recommend **series
resistors on every digital interface pin**. `CONTEXT.md`'s existing definition of **keepout** was
correct but incomplete — it had the 10 mm and 31.75 mm figures and the ground-edge alignment, and
was missing the no-copper geometry and the fact that the exclusion applies to all four layers.

**The ground plane is part of the antenna, and the datasheet quantifies it.** Table 2-3 note 1
measures the same antenna at **45 % average efficiency on a 57.2 × 25.4 mm board and 69 % on an
85 × 40 mm board.** Board outline is therefore an RF parameter with a measured coefficient, not a
mechanical leftover — about 1.9 dB.

**"Bulk capacitance at the module for TX peaks", as item 21 phrased it, is not achievable by adding
capacitance.** Three sourced limits pull against each other: the MCP1826 recommends **22 µF max** on
its output (§4.3) and **`CIN` ≥ `COUT`** for step loads (§4.4), while USB allows **≤ 10 µF across
`VBUS`** without inrush limiting (`MCP2221A` §1.6.2.2). `VBUS` is the LDO input and the 3V3 net is
its output, so the last two together cap total 3V3 capacitance near 10 µF — less than half what the
LDO alone would permit.

**The peak is 311 mA, and it is typical, not maximum.** Table 3-5: 311 mA at 802.11b 11 Mbps /
20 dBm, 310 mA at 11g 6 Mbps, against 92–98 mA receiving. The "Max." column is empty.

## Decision

**The spec in [`hardware/STACKUP.md`](../../hardware/STACKUP.md) is the layout of record.** Five
points carry the reasoning.

**1. L2 is a solid, unbroken ground plane and the reference for everything that matters.** The
stackup stays `signal / GND / PWR / signal` as D27 settled, with one refinement D27 could not have
anticipated: **L3 is necessarily split**, because there are two rails (3V3 and the 5 V lamp rail),
so it is a power layer with GND filling the remainder rather than a plane. The rule that follows is
that **no signal crosses a plane split** — USB D+/D− and the module UART live on L1 over solid L2,
and L4 carries only slow nets (lamp gates, button, `MCLR`/`INTOUT`, SWD). On a 1.6 mm build L1 is
~0.2 mm from L2 and L4 is ~0.2 mm from split L3, which is the whole argument for that division.

**2. The antenna keepout is a four-layer exclusion, and the board outline is cut back to meet it.**
No copper on any layer in the 5.3 × 15.73 mm region, the L2 plane edge coincident with the module's
ground outline edge, and the antenna end **overhanging the board** — §2.6.1's preferred case and
Figure 2-9's "Best Case". 16.42 mm of the 21.72 mm module remains on board, so the overhang is
mechanically uninteresting.

**3. The module goes at the top edge and every noise source goes at the base.** LDO, USB-C
receptacle, full-speed bridge and the three FETs all sit at the far end from the antenna, which is
what §2.5 asks for and which also puts the cable exit where the enclosure wants it. **This is why
the magnets are at the base**: Phase 4's "magnets at the base only" becomes the 31.75 mm metal
keepout expressed mechanically rather than a rule of thumb.

**4. The board targets ~40 × 85 mm, on the antenna-efficiency evidence.** Table 2-3 note 1's two
data points make the larger ground plane worth 1.9 dB, and a three-lamp column with 10 mm lenses
arrives near that size anyway. **The ground plane is not to be shrunk for panel area.**

**5. Capacitance is set by the constraint envelope, and the footprints are sized for the other
answer.** 10 µF on `VBUS`, 4.7 µF at the LDO output, 4.7 µF + 100 nF at the module's `VDD`, one
100 nF per MCU supply pin pair plus `VDDIO2`, and **0.47 µF on the bridge's `VUSB`** (§1.6.2.2
specifies 0.22–0.47 µF; the tree currently says only "a ceramic cap"). That is ~9.9 µF on 3V3 and
~10.1 µF on `VBUS` — the only point where all three limits hold at once. Whether it is *enough*
is not derivable from the published data, so the module bulk and `VBUS` bulk go on **1206 pads**
that accept 22 µF, and `VBUS` gets a **0 Ω series footprint** ahead of the bulk for inrush limiting.
The question is settled by scoping module `VDD` during a TX burst (D135, `spike`).

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **GND on both inner layers** (sig / GND / GND / sig), power as fat traces | Genuinely attractive here: it removes the split-plane return problem outright and maximises the ground the module's §2.3 asks for. Rejected because D27 settled a dedicated PWR plane and the split's only real cost — L4 return paths — is already removed by keeping every critical net on L1. Two rails on ~40 × 85 mm route comfortably on L3. **Revisit if item 22's routing pushes fast nets onto L4.** |
| **Keep the board rectangular; rely on the copper keepout alone** | Simpler outline, one less fab feature, and it is explicitly a "Good Case" in Figure 2-9. Rejected because the overhang is free — the enclosure is 3D-printed and absorbs any outline — and §2.6.1 states the preference for no PCB material plainly. Kept as the documented fallback. |
| **Shrink the board toward the 57.2 × 25.4 mm add-on-board size** | Cheaper panel, and the add-on board demonstrably works. Rejected on the vendor's own measurement: 45 % against 69 %. Paying 1.9 dB to save a few cm² of a one-off board is the wrong trade for a device that must stay linked from a desk, and fail-visible (Rule 4) means a marginal link is *visible* to the user as the wigwag. |
| **Module at the base, near the USB connector** | Shortens the 3V3 run from the LDO to the module — the one path carrying the 311 mA burst — which is a real benefit. Rejected because it puts the antenna beside the receptacle, the bridge and the magnets, violating both §2.5's separation and the 31.75 mm metal keepout. The supply path is fixed instead with copper: ≥ 0.8 mm on that run, per §2.5's own instruction to size GND and VDD for peak TX current. |
| **Fit 22 µF at the module and accept > 10 µF on `VBUS`** | Directly serves the TX peak, and the LDO explicitly permits 22 µF. Rejected as an unforced spec violation *before any measurement says it is needed*: it either breaks USB inrush compliance or demands an inrush-limiting part chosen on a guess. The footprints make it a value change later. |
| **Ignore §4.4's `CIN` ≥ `COUT` guidance** | It is a recommendation, not a limit, and plenty of designs run more output than input capacitance. Rejected because this application is precisely the one the section warns about — "applications that have output step load requirements" — and a 311 mA burst behind a 10 inch-equivalent USB cable is exactly that. |
| **Skip the series resistors on the module's digital pins** | Nothing needs them today at 115 200, and §2.4 says "recommended", not "required". Rejected because they cost two cents and are the *only* remaining lever on edge rates once the board is fabricated — and D128 may raise that link to 230 400, which is the direction that makes edges matter more, not less. 0 Ω is a valid fit. |
| **Trust `CONTEXT.md`'s keepout definition as complete** | It was written from ADR-0002 and is correct as far as it goes. Rejected for the same reason ADR-0023 rejected trusting D101: it omitted the information that changes the layout — the all-layer scope and the 5.3 mm geometry. Sharpened rather than replaced. |

## Consequences

**Accepted costs**
- **A non-rectangular board outline**, because the antenna end is cut back. One extra outline
  feature for the fab and a shape the enclosure must match.
- **L4 is restricted to slow nets**, so routing freedom on the bottom layer is deliberately spent to
  keep L2 the sole reference. On a 13-signal board this is affordable; on a denser one it would not
  be.
- **~40 × 85 mm is larger than the electronics need.** The ground plane is sized for the antenna,
  not for the parts.
- **Total 3V3 capacitance is held at ~10 µF pending measurement**, which is the least comfortable
  number in this ADR — it is bounded by a USB compliance rule rather than by the load.
- Formal USB suspend compliance is **not** met and cannot be, since `SSPND` is unwired (D116) and a
  light that goes dark on host sleep would be fail-invisible. Documented deviation.

**Benefits**
- The antenna gets the vendor's best-case treatment on every count that costs nothing: overhang,
  four-layer keepout, aligned ground edge, stitched ground under the module, and the 69 % ground
  plane rather than the 45 % one.
- **Every placement decision traces to a cited rule**, so item 22 has no discretionary RF choices
  left to get wrong.
- The enclosure's RF `assert()`s (item 24) stop being free-floating numbers and become derived from
  where the antenna is — the magnets-at-the-base rule now has a reason attached.
- **No swap class from ADR-0023 was spent.** The package pin order and the board's vertical zoning
  agree, so all of `PINOUT.md`'s documented freedoms remain available to item 22.
- Two parts item 20 was going to miss are now written down: the series resistors and the 0.47 µF
  `VUSB` value.

**Revisit if** item 22's routing forces a fast net onto L4 (then reconsider GND on both inner
layers); if the measured droop at 10 µF fails the 3.0 V floor (then the 1206 pads and the `VBUS`
series footprint are the fitted answer); or if the enclosure cannot hold 10 mm of plastic clearance
above the antenna, which would reopen the outline rather than the stackup.
