# ADR-0009 — Single 3.3 V rail for MCU and module; LEDs on 5 V behind FETs

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

The board is powered from USB-C, **power only** — because the link is Wi-Fi, there is no USB data,
no D+/D− pair to protect, and no high-speed routing. Just 5 V and CC pull-downs.

Three loads have to be fed:

| Load | Requirement |
|---|---|
| RNWF02 module | 3.0–3.6 V (3.3 V typ), with Wi-Fi TX current peaks |
| PIC32CM PL10 | 1.8–5.5 V — unusually flexible; abs max +6.5 V |
| 3× 10 mm diffused LEDs | red/yellow Vf ~2.0–2.2 V, **green Vf up to ~3.4 V** |

The green LED is the forcing constraint. From a 3.3 V rail, a 3.4 V Vf part leaves *negative*
headroom for a current-setting resistor: the lamp would be dim, wildly sensitive to Vf spread,
and effectively un-dimmable.

PL10 is a 5 V-capable part with **MVIO** (a second I/O supply domain, `VDDIO2`), which raises a
genuine question: run the MCU straight from USB 5 V, drive the LEDs directly from GPIO with real
headroom, and use `VDDIO2` at 3.3 V for the pins facing the module. The relevant ratings
(Table 37-1) permit it comfortably:

| Rating | Value |
|---|---|
| VDD / VDDIO2 abs max | −0.3 to +6.5 V |
| Max DC sink per I/O pin | **50 mA** |
| Max current into VDD / VDDIO2 | 250 mA each |
| Max current out of GND pins | 140 mA |
| Total power dissipation | 800 mW |

VOL/VOH are characterized at 1.8 V, 3.0 V and 5.5 V, so 5 V operation is fully specified.

## Decision

**One 3.3 V rail from an `MCP1826S-3302E/DB` (1 A, SOT-223) feeds both the MCU and the module.
`VDDIO2` is tied to `VDD` externally for single-supply operation. The LEDs run from the 5 V USB
rail through low-side N-FETs, gated by TCC0 PWM.**

```
USB-C 5V ──┬── MCP1826 ──► 3.3V ──┬──► PL10  VDD  (VDDIO2 tied to VDD)
           │                      └──► RNWF02 VDD / VDDIO
           │
           └──► lamp anodes (per-color R)
                     │
               LED cathodes ──► 3× low-side N-FET ──► GND
                                       ▲
                             gates ◄── TCC0 WO0/WO1/WO2 (3.3 V PWM)
```

Tying `VDDIO2` to `VDD` is what the datasheet recommends for single-supply mode (§3.2.3: *"If
VDDIO2 is configured in single supply mode, VDD and VDDIO2 must have the same supply sequence. It
is recommended to connect them together outside of the device"*).

FETs: logic-level N-channel, fully enhanced at 3.3 V Vgs (BSS138-class), 20–60 mA per lamp — an
undemanding part.

LDO sizing: the module's TX peaks dominate. 1 A is generous, and 5 V→3.3 V at ~350 mA worst case
is ~0.6 W in SOT-223, which is acceptable with adequate copper.

MVIO is left **unused but available** — the hardware doesn't preclude it if a later revision wants
a 5 V-domain peripheral.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **5 V VDD + MVIO, direct GPIO LED drive** | Electrically fine (50 mA/pin, 5.5 V characterized) and eliminates three FETs. Rejected on four grounds: **(1)** no BOM saving overall — the module needs 3.3 V so the LDO stays either way; **(2)** dual-supply MVIO needs `MVIO.VDDIO2CFG` setup, power-sequencing care, `VDDIO2OK` monitoring and extra decoupling, plus almost certainly *another* Zephyr enablement task on top of the TCC PWM work (ADR-0001 D49) — spending scarce porting budget to avoid three SOT-23 parts; **(3)** it constrains the pinout, locking the module UART to the `VDDIO2` pins (PA08–PA13) and the lamps to the VDD domain; **(4)** `AVDD` is internally tied to `VDD` on this part (§2.2.1), so VDD would be raw, unregulated USB 5 V sitting on the analog supply. |
| **Everything on 3.3 V, LEDs included** | The simplest possible topology. Fails on the green LED: Vf up to 3.4 V from a 3.3 V rail leaves no resistor headroom, so brightness would be dim and Vf-lottery dependent. |
| **Everything on 5 V, no LDO** | Would remove the regulator entirely, but the RNWF02 tops out at 3.6 V VDD. Not an option. |
| **Buck converter instead of an LDO** | Better efficiency at the module's TX peaks. Rejected as unwarranted complexity and a switching-noise source next to a 2.4 GHz radio, for a mains-powered desk device where ~0.6 W of dissipation is irrelevant. |
| **`MCP1700` (250 mA)** | Cheaper and lower quiescent current, already familiar. Rejected: 250 mA does not reliably cover RNWF02 Wi-Fi TX peaks, and browning out the radio mid-transmit is a miserable class of bug to chase. |
| **Direct GPIO drive at 3.3 V for red/yellow, FET only for green** | Saves two FETs. Rejected because it makes the three lamp channels electrically *different* — different drive impedance and PWM behaviour per color, so brightness matching and dimming curves stop being comparable. Uniformity is worth two SOT-23s. |

## Consequences

**Accepted costs**
- Three extra FETs and their gate resistors on the BOM.
- Two rails on the board (5 V and 3.3 V), which the 4-layer stackup handles cleanly with a
  dedicated power plane.
- LED brightness tracks the USB 5 V rail, which is only loosely regulated (4.75–5.25 V) — visually
  irrelevant, and PWM makes it adjustable anyway.
- MVIO, a genuinely interesting feature of this part, goes unexercised.

**Benefits**
- Full brightness headroom on all three colors, with identical drive behaviour per channel.
- LED current never flows through the MCU or the LDO — GND pins stay far from their 140 mA limit.
- No MVIO complexity in firmware, and no second Zephyr enablement task competing with D49.
- Free choice of pins for both the lamps and the module UART, since everything is one domain.
- Clean, regulated 3.3 V on `AVDD` should the ADC ever be wanted.

**Revisit if** a future revision needs a 5 V-domain peripheral (MVIO is available), or if LED
current rises enough to want constant-current drivers rather than resistors.
