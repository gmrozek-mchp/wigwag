# ADR-0023 — The 28-pin pin assignment: one forced pin, and a UART that need not start at PAD[0]

- **Status:** Accepted
- **Date:** 2026-08-15

## Context

Phase 3 opens with the pin map, because everything downstream — layout, the enclosure's lamp
pitch, the product devicetree — is derived from it, and because D101 established that the map
cannot be inherited. The dev board is a 48-pin `PIC32CM6408PL10048`; the product is
`PIC32CM6408PL10028` (D19), and `PB00`–`PB03` **do not exist on that die at all**. The cnano's
lamp pin (`PB02` = `TCC0 WO2`) and its console pins (`PB00`/`PB01` = `SERCOM1`) have nowhere to go.

Four claims had to be re-derived from primary sources rather than carried forward, and the full
evidence table is in [`hardware/PINOUT.md`](../../hardware/PINOUT.md). Two of them changed the
answer:

**D101 under-recorded the available options.** It listed `TCC0 WO1` on `PA01`/`PA09`/`PA25` and
missed `PA17`; it listed `SERCOM1` as `PAD0`+`PAD1` only. The ATDF shipped in-tree
(`modules/hal/.../atdf/PIC32CM6408PL10028.atdf`) turns out to be the authoritative machine-readable
source for all of this — the pad↔package map *and* every mux option — and it agrees pad-for-pad
with the HAL pinout header and with datasheet §2.3 on all eleven rows the datasheet's own table
extract could be read for.

**D101's central problem — that the lamps and the console want the same pins — was an artefact of
an assumption, not a property of the silicon.** It holds only if a USART's TxD must sit on
`PAD[0]`. §29.6.1 says otherwise: `CTRLA.TXPO = 0x1` selects `PAD[2] = TxD, PAD[3] = XCK`, and
`CTRLA.RXPO` independently selects any of the four pads.

## Decision

**The assignment in [`hardware/PINOUT.md`](../../hardware/PINOUT.md) is the pin map of record.**
13 signal pins and 5 power pins, 18 of 28, with 8 pads genuinely free. Three points carry the
reasoning; the rest is bookkeeping.

**1. `PA00` is the only viable `TCC0 WO0`, so the lamp block is forced.** `WO0` exists on exactly
three pads of this die, and the other two are disqualified:

| Candidate | Why not |
|---|---|
| `PA08` | In the **`VDDIO2` (MVIO) domain**. Dead if `VDDIO2` is not up — and the lamps *are* the product (Rule 4), so they must not depend on a second rail sequencing correctly, even with D50 tying `VDDIO2` to `VDD`. |
| `PA24` | Is **`XTAL32K1`**. §13.5.1 is unconditional — the pin is taken the instant `XOSC32K` is enabled, silently, in the oscillator rather than in `PORT`. That trap already cost a controlled hardware A/B on the cnano. |

`PA00` therefore takes `WO0`, and `PA01`/`PA02` follow as `WO1`/`WO2` — contiguous pins 22/23/24,
three FET gates as one bundle. `PA17`/`PA18` are left free as standby `WO1`/`WO2`.

**2. The console runs `SERCOM1` with `txpo = 1`, `rxpo = 3`, on `PA22`/`PA23` (mux C).** Those two
pads have **no `TCC0` function and no `SERCOM0` function at all** — spare capacity from the point
of view of every other consumer. The lamp/console contention D101 flagged is dissolved rather than
traded away. Mainline's driver writes both fields straight into `CTRLA`
(`uart_mchp_sercom_g1.c:376`) and the binding types them as plain ints, so this stays a devicetree
change with no code, preserving ADR-0018's "the console comes free".

**3. The module UART stays on `SERCOM0` at `PA04`/`PA05` mux C — the cnano's own pins (D76).** It
is the one line of `pinctrl` that transfers verbatim from the dev board, so the pairing that has
been exercised for two phases is the pairing that ships.

**`PA30` (RESET), `PA31` (SWCLK) and `PA20` (SWDIO) are reserved and must not be re-muxed.** SWCLK
carries a **1 kΩ** pull-up, which Table 40-6 calls critical rather than advisory. Note that
`SERCOM1 PAD[0]` is also offered on `PA20` and `PA31`; both are traps.

**`PA06`/`PA07` are held, unpopulated, for `SERCOM0` RTS/CTS** (`txpo = 2` puts RTS on `PAD[2]`,
CTS on `PAD[3]`). This is the one place the assignment spends a pin on a maybe, and D128 is why:
the module's real baud rate is an open question, and flow control is the answer that needs
board support rather than firmware.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **Lamps on `PA24`/`PA25`/`PA18`, bridge on `PA00`/`PA01`** — the split D101 proposed | The obvious reading of the options as recorded, and it does fit. Rejected once `txpo = 1` removed the constraint that motivated it: it spends `XTAL32K1`/`XTAL32K2` on lamps, inheriting the cnano's oscillator-override trap onto a board that has no reason to carry it, and permanently forecloses `XOSC32K` for a future revision. Deliberately keeping those two pads clear is free here. |
| **Lamps on `PA08`/`PA09`/`PA10`** (contiguous, all three `WO` available) | The tidiest-looking block on the die. Rejected because all four of `PA08`–`PA11` are MVIO: the lamps would go dark if `VDDIO2` were ever mis-sequenced or externally supplied, which is precisely the fail-*invisible* mode ADR-0007 exists to prevent. |
| **Module UART on `PA10`/`PA11` mux D** | Frees the `PA04`–`PA07` block and is arguably what MVIO is *for* — talking to another 3.3 V part. Rejected on ADR-0009's own reasoning, which explicitly declined to let the module UART be "locked to the `VDDIO2` pins", and because it discards D76's continuity with the dev board. Kept as a documented swap. |
| **Console on `PA08`/`PA09` mux C** | Also `PAD[2]`/`PAD[3]`, so it works with the same `txpo = 1`. Rejected for the MVIO reason again — and the console is the only way this board explains itself (ADR-0018), so it is the last thing that should acquire a second supply dependency. |
| **Lamps on `PA00`/`PA17`/`PA18`, leaving `PA01`/`PA02` for `SERCOM1 PAD0/PAD1`** | Spends "SERCOM-barren" pads on lamps and keeps the SERCOM-rich block for UARTs, which sounds like the right kind of thrift. Rejected because `txpo = 1` means the thrift buys nothing, and it scatters three FET gates across pins 22, 7 and 8 — opposite ends of the package for what is physically one cluster of lamps. |
| **Drop `INTOUT`, keep only `MCLR`** | Nothing in the firmware uses either today (`link.c` supervises over AT commands, D75), and `INTOUT` serves low-power wake that a mains-powered desk lamp does not want. Rejected because with 8 pads spare the pin costs nothing, and an unrouted signal is a respin while an unused one is a comment. `MCLR` is kept for the stronger reason: a hard module reset is a recovery path AT commands cannot provide. |
| **Wire `SERCOM0` RTS/CTS as populated** | Would settle D128 by brute force. Rejected as premature — flow control has never been needed at 115 200, and fitting it before the baud question is measured would add two nets and two module pins on a guess. Footprints and a DNP are the cheap half of the option. |
| **Trust D101's recorded option list** | It is the project's own note from the day before, and re-deriving felt redundant. Rejected because it was *wrong in the direction that mattered* — it omitted `PA17` and the entire `PAD[2]`/`PAD[3]` family, which is exactly the information that changes the assignment. CLAUDE.md's "verify against real sources" earns its keep here. |

## Consequences

**Accepted costs**
- **`PA23` is `ADC0 VREFP`.** An external ADC reference is given up. Internal references remain,
  and the one plausible analogue want (an ambient-light sensor, ADR-0018) does not need one.
- **`PA03` is `TCC0 WO3`**, spent on `MCLR`. A fourth PWM channel has no role in a three-lamp
  stoplight, and `PA11`/`PA19` remain as `WO3` alternates.
- Two pads (`PA06`/`PA07`) are reserved against a question that may resolve as "not needed".
- The product devicetree overlay is a near-total rewrite of the cnano one, not an edit. Only the
  `SERCOM0` pinmux line survives.

**Benefits**
- The lamps depend on exactly one supply rail and no oscillator, which is the property Rule 4
  actually needs.
- `XOSC32K` remains available to a future revision instead of being spent on a lamp.
- Every lamp is `PWM_POLARITY_NORMAL` (uniform low-side FETs, D26), so **the product does not
  depend on `firmware/patches/0001-*.patch`** — that patch exists only for the cnano's active-low
  on-board LED (D72/D74). The boot-time flag print stays regardless; it is what would catch a
  silently-dropped devicetree cell.
- 8 free pads and 2 reserved, against a plan that budgeted "~17 of 28" — the footprint question
  never became a pin question.
- `txpo = 1` is a reusable result: on this family a SERCOM USART has *two* independent pin pairs,
  not one, which roughly doubles the placement freedom any future PL10 design has.

**Revisit if** the layout cannot route the assignment within the swap classes listed in
`PINOUT.md`; if D128 resolves toward flow control (then `PA06`/`PA07` populate); or if a revision
wants `XOSC32K`, which is now possible without touching a lamp.
