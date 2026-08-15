# Stackup, placement and RF keepout — wigwag product PCB

The authoritative layout spec. Decision and reasoning in **ADR-0024**; the register entries are
D132–D136. The pin- and net-level spec is [`PINOUT.md`](PINOUT.md) (ADR-0023); this document is
everything about *where things go and what layer they go on*.

Plan item 21. Consumed by item 22 (ERC/DRC, fab outputs) and by Phase 4, which takes the board
outline and the antenna end as inputs to the enclosure model.

## Sources, and what each one settled

Read from primary sources, not recalled. Where a number is inferred rather than stated, it says so.

| Fact | Source |
|---|---|
| Module at board edge; **module ground outline edge aligned with the host ground-plane edge**; 31.75 mm to metal; 10 mm to plastic; keepout under RF and voltage test points | `DS70005544C` [§2.3](https://onlinedocs.microchip.com/oxy/GUID-3DDF7D25-768C-4E66-8343-E56E3AE4B4BB-en-US-4/GUID-FE451F8B-FAD9-4491-A070-73D476297A8B.html) |
| **No-copper region 5.3 mm deep × 15.73 mm wide** from the module PCB edge | `DS70005544C` p. 13, Figure 2-8 callouts |
| Antenna is on the module's **own top copper**, nothing below it; **no PCB material** below the antenna preferred; no host copper or planes in that area | `DS70005544C` [§2.6.1](https://onlinedocs.microchip.com/oxy/GUID-3DDF7D25-768C-4E66-8343-E56E3AE4B4BB-en-US-4/GUID-A91B562C-0294-4B4D-AC63-91E3F9BBD501.html) |
| **Efficiency 45 % on a 57.2 × 25.4 mm board, 69 % on 85 × 40 mm.** Peak gain 1.18 dBi at 2410 MHz; 2400–2485 MHz | `DS70005544C` Table 2-3 + note 1 |
| Signals on inner and bottom layers; **top layer under the module must be ground**, many GND vias; no fan-out under module or antenna; **GND via ≥ 10 mil hole**; **series resistor on every digital interface and reserved pin, close to the module** | `DS70005544C` [§2.4](https://onlinedocs.microchip.com/oxy/GUID-3DDF7D25-768C-4E66-8343-E56E3AE4B4BB-en-US-4/GUID-AA9297A8-8387-45BF-8CD5-B3DE980F1D42.html) |
| Module away from HF clocks and RF sources; clean supply; **GND/VDD traces wide enough for peak TX current** | `DS70005544C` [§2.5](https://onlinedocs.microchip.com/oxy/GUID-3DDF7D25-768C-4E66-8343-E56E3AE4B4BB-en-US-4/GUID-8187CFD5-5D41-4E1F-BFB2-B2630FD524E0.html) |
| **Peak TX 311 mA typ** (802.11b 11 Mbps, 20 dBm); 310 mA (11g 6 Mbps); RX 92–98 mA; XDS 0.7 µA | `DS70005544C` Table 3-5 / 3-6 |
| Module `VDD` 3.0–3.6 V; abs max 500 mA into `VDD` pins, 500 mA out of `GND` pins | `DS70005544C` Table 3-1, 3-3 |
| Bulk **and** decoupling cap at module pins 20 and 23, placed close to the pin | `DS70005544C` [§2.2.1](https://onlinedocs.microchip.com/oxy/GUID-3DDF7D25-768C-4E66-8343-E56E3AE4B4BB-en-US-4/GUID-7962EF13-C332-48B0-B3B4-4BF4303E5B56.html) |
| Module body **14.73 × 21.72 × 2.1 mm**; land pattern; silkscreen and copper keepout zones; *"keep these areas free from routes and exposed copper; ground fill with solder mask may be placed here"* | `DS70005544C` p. 38, drawing C04-23567 Rev C |
| LDO `COUT` min 1 µF, **max recommended 22 µF**; `CIN` **≥ `COUT`** for step-load response; curves characterised at `CIN` = `COUT` = 4.7 µF X7R; GND pin carries only quiescent current but must return to the output cap | `MCP1826` `DS20002057D` §3.3, §4.3, §4.4, §2.0 |
| **≤ 10 µF effective across `VBUS`** or inrush limiting is required; **`VUSB` bypass 0.22–0.47 µF ceramic**; USB 2.0 **full speed**; no external parts on D+/D− | `MCP2221A` `DS20005565E` §1.5, §1.6.2.2, Table 1-1, Table 4-1 note 1 |
| One decoupling cap **per supply pin pair**, same side as the MCU, **decoupling first in the power chain**; `VDDIO2` gets its own; `AVDD` internally tied to `VDD` on this device | `PIC32CM PL10` [§40.3.1](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-8C5FCE9F-5DAB-4EE1-921A-7B8556812DBB.html), [§40.3.1.2](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-FB45EE97-D7F2-4265-A8B5-79D4850C9D8A.html), [§40.3.1.3](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-1161AB62-565B-4749-BD0E-26EE9A8CF15A.html) |

Two cautions about the sources themselves:

- **The package drawing read is annotated "With Metal Shield and Coaxial Connector"** — the U.FL
  variant. The 14.73 × 21.72 mm body is the same module PCB, but **confirm the `PC` variant's own
  drawing and land pattern when the footprint is drawn** rather than trusting this cross-read.
- **15.73 mm is read off the figure**, and it is the module body width (14.73 mm) plus ~0.5 mm each
  side. That reading is an inference; the 5.3 mm depth is stated directly.

## The stackup

4-layer, 1.6 mm finished (D27). **L2 is a solid, unbroken ground plane and is the reference for
every signal that matters.**

| Layer | Role | Contents |
|---|---|---|
| **L1** top | signal + GND fill | all components; USB D+/D−, module UART, and anything else timing-sensitive; GND flood everywhere else. **Solid GND under the module footprint**, heavily stitched (§2.4) |
| **L2** | **solid GND** | nothing else. No routing, no splits, no cutouts — except the antenna keepout below |
| **L3** | power | `+3V3` island and `+5V` lamp-rail island; **GND fill in all remaining area**, stitched to L2 |
| **L4** bottom | signal + GND fill | slow nets only — lamp gate drives, button, module `MCLR`/`INTOUT`, SWD; GND flood elsewhere |

Nominal build for 1.6 mm 4-layer: ~0.2 mm prepreg L1↔L2, ~1.065 mm core L2↔L3, ~0.2 mm prepreg
L3↔L4; 1 oz outer, 0.5 oz inner. **Confirm against the chosen fab's actual stackup at item 22** —
this is the typical asymmetric build, not a verified spec. The consequence that matters: L1 is
tightly coupled to L2, which is why the critical nets live on L1.

### Layer rules

1. **No signal crosses a plane split.** L3 is necessarily split (two rails), so any net whose return
   would have to hop islands stays on L1 referenced to L2. This is why L4 carries only slow nets.
2. **Nothing routes under the module or the antenna on any layer** (§2.4). Fan out from the module
   pads straight to vias and leave the area.
3. **Stitch generously.** GND vias under the module's exposed pads, **≥ 10 mil (0.254 mm) drill**
   (§2.4), and a via fence along the board edges tying L1/L2/L3-fill/L4 grounds together.
4. **Power and return run to the decoupling caps first, then to the pin** (§40.3.1) — the cap is
   first in the power chain, not a stub hung off the pin.
5. Rail traces ≥ 0.5 mm; **`VBUS` and the 3V3 run to the module ≥ 0.8 mm**, because §2.5 asks
   explicitly for GND and VDD wide enough for peak TX current and that path carries the 311 mA burst.

## The antenna keepout

The one part of this document that cannot be traded away. **Four layers, not one.**

```
                    ┌─── module PCB edge (antenna end) ─────┐
   board outline ───┤   5.3 mm  ← no copper, no material    │  ← module overhangs here
   & GND plane      ├───────────────────────────────────────┤
   edge  ══════════▶│   16.42 mm  module ground outline     │  ← solid GND on L1, stitched
                    │   sits over host ground plane         │
                    └───────────────────────────────────────┘
                         15.73 mm wide keepout
```

- **No copper on L1, L2, L3 or L4** in a region **5.3 mm deep × 15.73 mm wide** from the module's
  antenna-end PCB edge (§2.3, §2.6.1, p. 13).
- **The board outline is cut back to that same line, so the antenna end overhangs** — §2.6.1's
  preferred case is *no PCB material* below the antenna, and Figure 2-9 labels it "Best Case".
  16.42 mm of the 21.72 mm module still sits on board, so the 5.3 mm overhang is mechanically
  unremarkable. Fallback if the outline must stay rectangular: keep the material, keep the
  all-layer copper keepout. That is Figure 2-9's "Good Case" and costs efficiency.
- **The L2 ground-plane edge coincides with the module's ground outline edge** (§2.3). The plane
  stops there; it does not reach under the antenna.
- Copper keepout under the module's **RF test point and voltage test points**, or solder-mask the
  whole region except the exposed ground paddle (§2.3). The land-pattern drawing's own note allows
  ground fill with solder mask over those zones.
- **≥ 31.75 mm to any metal structure; ≥ 10 mm to plastic, in all directions** (§2.3). Binding on
  the enclosure — Phase 4 item 24 already asserts both.

**A discrepancy worth knowing:** §2.3's bullets say 31.75 mm for *metal* and 10 mm minimum for
*plastic*, but the Figure 2-8 caption says 31.75 mm "for all metallic and plastic structures".
The project follows the bullets (10 mm plastic, 31.75 mm metal — D45, `CONTEXT.md`), because they
are the specific statement and the caption is a summary. The enclosure wall therefore sits inside
the figure's stricter reading. That is a *performance* risk, not a functional one, and it is
settled empirically: measure RSSI and throughput with the enclosure on before committing to it.

## Placement

The board is a **vertical column**: three lamps stacked as a stoplight, viewer to the front, PCB
plane parallel to the front face, 10 mm through-hole LEDs entering from the back.

```
   TOP  ────────────────────────────────  ← antenna overhangs, ≥10 mm plastic above
        │   RNWF02 module                │
        │   bulk + decoupling, straps,   │  ← series R's on the digital pins, close in
        │   I²C pull-ups, test points    │
        ├────────────────────────────────┤
        │   PL10 MCU + 3 decoupling      │
        │   SWD header · UART pads (D59) │
        ├────────────────────────────────┤
        │   lamp  ● green                │
        │   lamp  ● yellow               │  ← FETs + gate/current resistors beside each
        │   lamp  ● red                  │
        ├────────────────────────────────┤
        │   MCP1826 LDO · MCP2221A       │
        │   USB-C receptacle · VBUS bulk │  ← cable exits at the base
   BASE ────────────────────────────────  ← magnets here, and only here
```

Why this order, top to bottom:

- **Module at the top edge, antenna pointing up and away from everything.** The one edge with no
  connector, no switching and no metal near it.
- **Every noise source and every high-current path is at the far end from the antenna** — the LDO,
  the USB-C receptacle, the full-speed bridge, and the three FETs whose edges are the broadband
  offender on this board. §2.5 asks for exactly this separation.
- **The magnets are at the base because the antenna is at the top.** Phase 4's "magnets at the base
  only" (item 24) stops being an arbitrary rule and becomes the 31.75 mm metal keepout expressed in
  the enclosure. On a board of the target height the antenna-to-magnet distance is ~70 mm, roughly
  double the requirement.
- The USB cable exits at the base, which is where the enclosure wants it anyway.

### Board outline

**Target ~40 × 85 mm.** Not an aesthetic choice: Table 2-3 note 1 measures the *same antenna* at
**45 % average efficiency on a 57.2 × 25.4 mm board and 69 % on an 85 × 40 mm board.** A three-lamp
column with 10 mm lenses lands near 85 × 40 mm naturally, so the better-measured geometry is
available for free. **Do not shrink the ground plane to save panel area** — that is 1.9 dB of link
budget, and this device lives on a desk at the far end of a house from the broker.

Lamp pitch and the exact outline are Phase 4 parameters (item 23) derived from the lens bores. The
constraint item 21 imposes is only this: the ground plane stays at or above the 85 × 40 mm class,
and the antenna end keeps its keepout.

## Decoupling and bulk capacitance

### The three limits that interact

"Bulk capacitance at the module for TX peaks" cannot be satisfied by simply adding capacitance.
Three sourced limits bound it from different directions:

| Limit | Value | Source |
|---|---|---|
| LDO output capacitance, max recommended | **22 µF** | `MCP1826` §4.3 |
| LDO input cap should be **≥** output cap, for step loads | `CIN` ≥ `COUT` | `MCP1826` §4.4 |
| Effective capacitance across `VBUS` | **≤ 10 µF** or fit inrush limiting | `MCP2221A` §1.6.2.2 |

`VBUS` *is* the LDO input, and the 3V3 net *is* the LDO output — so the USB inrush ceiling and the
`CIN ≥ COUT` guidance together cap total 3V3 capacitance at ~10 µF, well below the 22 µF the LDO
would otherwise allow. All three hold simultaneously only at 10 µF / 10 µF.

### As designed

| Node | Value | Why |
|---|---|---|
| `VBUS`, at the receptacle | **10 µF** X7R + 100 nF | USB inrush ceiling; doubles as LDO `CIN` |
| LDO `VOUT` | **4.7 µF** X7R | datasheet's own characterisation value, so the published curves apply |
| Module `VDD` (pin 20) | **4.7 µF** + **100 nF** | §2.2.1 bulk + decoupling, both close to the pin |
| Module `VDDIO` (pin 23) | **100 nF** | §2.2.1; same rail as `VDD` (D50) |
| MCU `VDD`/`GND` pins 14/15 | **100 nF** | one per supply pin pair (§40.3.1) |
| MCU `VDD`/`GND` pins 20/21 | **100 nF** | the second pair — *not* optional, and easy to drop |
| MCU `VDDIO2` pin 6 | **100 nF** | §40.3.1.3, even tied to `VDD` (D50) |
| Bridge `VDD` (pin 1) | **100 nF** | |
| Bridge `VUSB` (pin 11) | **0.47 µF** | §1.6.2.2 states **0.22–0.47 µF**; 100 nF is under-spec |

Total 3V3 ≈ **10.6 µF** nominal, `VBUS` ≈ **10.1 µF**. Inside the 22 µF §4.3 ceiling with wide
margin, and `CIN` and `COUT` are equivalent — which is what §4.4 asks for ("of equivalent (or
higher) value").

**Do not read those two totals to better than about ±20 %.** X5R/X7R ceramics lose substantial
capacitance under DC bias, so a nominal 10 µF 0805 at 5 V may deliver 6–7 µF effective; the same
derating applies on both sides of the LDO. The 4 % difference between the two columns is far inside
the part tolerance, and the USB ≤ 10 µF rule is likewise written against nominal/effective
capacitance rather than a measured value. This is exactly why D135 is a measurement and not a
calculation.

### Footprints sized so the answer can change without a respin

Whether 10 µF holds module `VDD` above its 3.0 V minimum through a TX burst **is not knowable from
the published data** — the MCP1826 datasheet gives transient *curves* but no numeric ΔV spec, and
the module's burst duration is unspecified. So it gets measured, and the board is built to survive
either answer:

- Module bulk and `VBUS` bulk on **1206 pads**, which take 22 µF in the same footprint.
- A **series element footprint in `VBUS`** ahead of the bulk — 0 Ω fitted by default — so inrush
  limiting can be added if the bulk grows past 10 µF.

Same tactic as the DNP `RTS`/`CTS` footprints in `PINOUT.md`: buy the option, not the part.
**Measurement gate:** scope module `VDD` at pin 20 during a TX burst (`test wifi` is enough to
force association traffic); the rail must stay ≥ 3.0 V. Record the number in the journal. D135.

## Parts item 20 does not yet list

Both found in the layout sources rather than the schematic ones, so they belong here until item 20
absorbs them:

- **Series resistors on every module digital interface pin, placed close to the module** — §2.4
  recommends them for digital interface *and* reserved pins. That is `UART1_TX`, `UART1_RX`,
  `MCLR`, `INTOUT` populated, plus the two DNP flow-control pins. **33 Ω footprints, 0 Ω
  acceptable**; the datasheet gives no value. Cheap, and they are the only lever left for edge-rate
  problems once the board exists.
- **`VUSB` bypass is 0.47 µF, not "a ceramic cap"** — `PINOUT.md` and item 20 both say the latter.
  §1.6.2.2 gives 0.22–0.47 µF.

## Consequences for `PINOUT.md`

**No swap class was needed.** The placement above routes the assignment as drawn: the lamp block on
`PA00`/`PA01`/`PA02` (pins 22/23/24) faces the lamp column, the module UART on `PA04`–`PA07` (pins
26–28 and 1) faces the module at the top, and the console on `PA22`/`PA23` (pins 12/13) faces the
bridge at the base. The package's pin order and the board's vertical zoning happen to agree, so
ADR-0023's documented degrees of freedom stay unspent and available to item 22.

## Known deviation

A bus-powered USB device must support suspend at ≤ 500 µA (`MCP2221A` §1.6.2.2, USB 2.0). This
device draws ~100 mA continuously and **does not implement suspend** — it cannot, since `SSPND` is
deliberately unwired (D116) so the MCU never learns the host suspended. A status light that goes
dark when the host sleeps would be a fail-*invisible* light, which Rule 4 forbids. Recorded as an
accepted deviation, not an oversight; it affects formal USB compliance only, and the device is
powered from a port or charger that does not care.
