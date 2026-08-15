# Pin assignment — `PIC32CM6408PL10028-I/SS` (SSOP-28)

The authoritative pin map for the product PCB. Decision and reasoning in **ADR-0023**; the
register entries are D124–D128.

**This is not the Curiosity Nano's map.** The dev board is a 48-pin `PIC32CM6408PL10048` and
`PB00`–`PB03` do not exist on this die at all (D101), so nothing in
`firmware/boards/pic32cm_pl10_cnano.overlay` transfers except `SERCOM0` on `PA04`/`PA05`.

## Sources, and what each one settled

Everything below was read from a primary source rather than recalled. Where two sources exist
they agree; where only one exists it is named.

| Fact | Source |
|---|---|
| Package pin ↔ pad map, all 28 positions | `PIC32CM6408PL10028.atdf` `<pinout name="DUAL28MVIO">`, cross-checked against datasheet [§2.3](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-F7504DEC-694C-4288-9810-58130AB9109E.html) for 11 rows |
| Which pads exist on this die (23 of 32 PA lines, no PB at all) | ATDF `<pinout>` + `hal_microchip/.../pio/pic32cm6408pl10028.h` |
| TCC0 / SERCOM0 / SERCOM1 mux options | ATDF `<signal>` table, confirmed pad-for-pad against the same HAL header |
| `TXPO=0x1` puts TxD on `PAD[2]`, `XCK` on `PAD[3]`; `RXPO` selects any pad | datasheet [§29.6.1](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-C368FEE7-8828-47A5-BB81-1DFEC7EA2B5A.html) `CTRLA.TXPO` / `CTRLA.RXPO` |
| `PA20` = SWDIO, `PA31` = SWCLK, `PA30` = RESET | datasheet §2.3 `PA20` row; independently, cnano user guide Table 3-2 (DBG0/DBG1/DBG3) |
| SWCLK needs a **1 kΩ** pull-up; SWDIO a weak one | datasheet [§40.8](https://onlinedocs.microchip.com/oxy/GUID-DE09DA5A-1CBB-49A8-9DA0-B2EB94E57E56-en-US-11/GUID-B59C8584-FEC7-44FC-BAC2-15D5AF841F64.html) Table 40-6 |
| `PA08`–`PA11` are the `VDDIO2` (MVIO) domain and are dead without it | datasheet §2.3 note 3; cnano user guide §4.5 |
| `PA24`/`PA25` are `XTAL32K1`/`XTAL32K2` | ATDF `OSC32KCTRL.XIN32`/`XOUT32`; datasheet §2.3 rows |
| `VDDIO2` tied to `VDD` for single-supply | datasheet §3.2.3 (ADR-0009, D50) |
| RNWF02PC pin numbers and names | `DS70005544C` Table 2-1 |
| Strap1/Strap2 both low ⇒ UART1 host interface | `DS70005544` §2.2 Table 2-2 |
| MCP2221A pinout, `VUSB` rule, no external parts on D+/D− | `DS20005565E` Table 1-1, §1.6.2.1, §4.1 Table 4-1 note 1 |

## The package

Pins 1–14 down one side, 15–28 up the other.

| Pin | Pad | Assignment |
|----:|-----|---|
| 1 | `PA07` | *reserved* — `SERCOM0 PAD[3]` = CTS if flow control is ever fitted (D128) |
| 2 | `PA08` | free (MVIO domain) |
| 3 | `PA09` | free (MVIO domain) |
| 4 | `PA10` | free (MVIO domain) |
| 5 | `PA11` | free (MVIO domain) |
| 6 | `VDDIO2` | **tie to 3V3** alongside `VDD` (§3.2.3, D50) |
| 7 | `PA17` | free — standby `TCC0 WO1` |
| 8 | `PA18` | free — standby `TCC0 WO2` |
| 9 | `PA19` | module `INTOUT` in (active low), `EXTINT[3]` available |
| 10 | `PA20` | **SWDIO** — reserved. Weak pull-up (10–100 kΩ) |
| 11 | `PA21` | button in (active low, internal pull-up), `EXTINT[5]` available |
| 12 | `PA22` | console TX — `SERCOM1 PAD[2]`, mux **C**, `txpo = 1` |
| 13 | `PA23` | console RX — `SERCOM1 PAD[3]`, mux **C**, `rxpo = 3` |
| 14 | `VDD` | 3V3 |
| 15 | `GND` | |
| 16 | `PA24` | free — `XTAL32K1`. Deliberately unused; see below |
| 17 | `PA25` | free — `XTAL32K2`. Deliberately unused; see below |
| 18 | `PA30` | **RESET** — reserved |
| 19 | `PA31` | **SWCLK** — reserved. **1 kΩ pull-up required** |
| 20 | `VDD` | 3V3 |
| 21 | `GND` | |
| 22 | `PA00` | lamp **green** — `TCC0 WO0`, mux **F** |
| 23 | `PA01` | lamp **red** — `TCC0 WO1`, mux **F** |
| 24 | `PA02` | lamp **yellow** — `TCC0 WO2`, mux **F** |
| 25 | `PA03` | module `MCLR` out (active low — drive low to reset the module) |
| 26 | `PA04` | module TX — `SERCOM0 PAD[0]`, mux **C**, `txpo = 0` |
| 27 | `PA05` | module RX — `SERCOM0 PAD[1]`, mux **C**, `rxpo = 1` |
| 28 | `PA06` | *reserved* — `SERCOM0 PAD[2]` = RTS if flow control is ever fitted (D128) |

**13 signal pins + 5 power/ground = 18 of 28.** Two more held in reserve, eight genuinely free.

## Why the lamps sit where they do

`TCC0 WO0` is the tight one, and it forces the whole block. Its only three options on this die are
`PA00`, `PA08` and `PA24`:

- `PA08` is in the **MVIO domain** — dead if `VDDIO2` is not up. The lamps *are* the product
  (Rule 4), so they must not depend on a second supply rail being sequenced correctly, even
  though D50 ties `VDDIO2` to `VDD`.
- `PA24` is **`XTAL32K1`**. Datasheet §13.5.1 is unconditional: those pins are taken the moment
  `XOSC32K` is enabled, silently, in the oscillator rather than in `PORT`. That exact trap already
  cost a hardware A/B on the cnano (see the overlay's `&xosc32k` comment) and it is not worth
  re-inheriting on a board we control.

So **`PA00` is the only viable `WO0`**, and once `PA00` is a lamp, `PA01`/`PA02` are the natural
`WO1`/`WO2` — contiguous pins 22/23/24, three FET gates as one bundle. `PA17`/`PA18` stay free as
the standby `WO1`/`WO2` should layout ever need them.

Keeping `PA24`/`PA25` clear has a second payoff: `XOSC32K` stays **available** to a future
revision (RTC timekeeping) instead of being permanently spent on a lamp.

## Why the console is on `PA22`/`PA23`

D101 recorded `SERCOM1 PAD0/PAD1` as the only option — `PA00`+`PA01` or `PA10`+`PA11` — and noted
that both collide with the lamps. **That overlap was an artefact of assuming TxD must be on
`PAD[0]`.** It need not be: §29.6.1 `CTRLA.TXPO = 0x1` selects `PAD[2] = TxD, PAD[3] = XCK`, and
`RXPO` selects any of the four pads independently.

That opens `PAD[2]`+`PAD[3]`, and on this die those reach `PA22`/`PA23` (mux C) — two pins with
**no `TCC0` function and no `SERCOM0` function at all**. The lamp/console contention disappears
entirely rather than being traded off.

Mainline's driver passes both fields straight through
(`uart_mchp_sercom_g1.c:376`, `CTRLA |= RXPO(rxpo) | TXPO(txpo)`), and the binding types them as
plain ints, so this is devicetree only — no driver work, consistent with ADR-0018's "the console
comes free".

Cost, and it is real: `PA23` is `ADC0 VREFP`, so an external ADC reference is given up. The
internal references remain, and nothing on the roadmap wants one.

`SERCOM1 PAD[0]` also appears on `PA20` and `PA31` — **both are SWD pins.** Do not use them.

## Reserved and forbidden

| Pad | Pin | Why |
|---|---|---|
| `PA30` | 18 | RESET. Reclaimable as GPIO (§16.5.1) but never on this board — this is why Table 2-1 counts 22 I/O on a die that bonds out 23 pads |
| `PA31` | 19 | SWCLK. **1 kΩ pull-up is called "critical for reliable operation"** (Table 40-6). Hot-plug detection dies if the pin is re-muxed (§18.4.5.2.2) |
| `PA20` | 10 | SWDIO. Weak pull-up (10–100 kΩ) is best practice per §40.8 |

## Peripheral configuration that goes with the pins

```
tcc0    24 MHz GCLK0, prescaler 1, 16-bit  →  366 Hz floor, 500 Hz carrier fits (as cnano)
        WO0/WO1/WO2 on PA00/PA01/PA02 mux F
        all three PWM_POLARITY_NORMAL — low-side FETs are uniformly active high (D26)

sercom0 module UART   txpo = 0  rxpo = 1   PA04/PA05 mux C
        ** baud is an open question — see D128. The module's UART1 default is 230 400,
           not the 115 200 the whole firmware is built around. **

sercom1 console UART  txpo = 1  rxpo = 3   PA22/PA23 mux C
        115 200 (MCP2221A handles up to 460 800)
```

Because every lamp is `PWM_POLARITY_NORMAL` on this board, the product does **not** depend on
`firmware/patches/0001-*.patch`. That patch exists only because the cnano's on-board yellow LED is
wired active low (D72) and mainline drops the polarity cell (D74). Keep the boot-time flag print
regardless — it costs nothing and it is what would catch a silently-ignored devicetree value.

## Connections to the two companion parts

### RNWF02PC — `DS70005544C` Table 2-1

| Module pin | Name | Goes to |
|---:|---|---|
| 2 | `I2C_SCL` | **1.2 kΩ pull-up to VDDIO.** Internal to the module (its own Trust&Go device) — not an MCU bus. Required on the `PC` variant |
| 3 | `I2C_SDA` | **1.2 kΩ pull-up to VDDIO.** Same |
| 4 | `MCLR` | MCU `PA03` (pin 25) |
| 7, 17, 18 | Reserved | **Do not connect** |
| 10 | `DFU_RX`/**Strap1** | Pull **down** — with a resistor, plus a test point. See below |
| 11 | Reserved | Test point (datasheet: tri-stated host I/O or a switch, "for future use") |
| 13 | `INTOUT` | MCU `PA19` (pin 9) |
| 14 | `UART1_TX` | MCU `PA05` (pin 27, RX) |
| 15 | `UART1_RTSn` | MCU `PA07` (pin 1) — **footprint only**, DNP unless D128 needs flow control |
| 16 | `UART1_CTSn` | MCU `PA06` (pin 28) — **footprint only**, DNP as above |
| 19 | `UART1_RX` | MCU `PA04` (pin 26, TX) |
| 20 | `VDD` | 3V3 (3.0–3.6 V) |
| 23 | `VDDIO` | 3V3 (1.8–3.6 V) |
| 24 | `TP` | PMU 1.5 V test point — **do not load** |
| 26 | `DFU_TX`/**Strap2** | Pull **down** — with a resistor, plus a test point |
| 27 | `UART2_TX` | Debug-log test point, 460 800 8N1. **TX only — there is no `UART2_RX`** |
| 9, 12, 28, 29 | `GND` | Ground; 29 is the thermal paddle |

Strap1/Strap2 both low selects the UART1 host interface (§2.2 Table 2-2), which is what this
design needs — but **those same two pins are the module's DFU port.** Pull them down through
resistors and bring both to test points, so module firmware update stays physically possible.
Hard-shorting them to ground would save two resistors and permanently foreclose it.

### MCP2221A-I/ST — `DS20005565E` Table 1-1

| Bridge pin | Name | Goes to |
|---:|---|---|
| 1 | `VDD` | 3V3 |
| 2 | `GP0` | NC or activity LED — factory default is `LED_URx`. **Not `SSPND`; not wired to the MCU** (D116) |
| 3 | `GP1` | NC or activity LED — factory default `LED_UTx` |
| 4 | `RST` | Internal pull-up; no external part needed |
| 5 | `URx` (in) | MCU `PA22` (pin 12, console TX) |
| 6 | `UTx` (out) | MCU `PA23` (pin 13, console RX) |
| 7 | `GP2` | NC or test point — factory default is `USBCFG`. **Not wired to the MCU** (D116) |
| 8 | `GP3` | NC — factory default `LED_I2C` |
| 9, 10 | `SDA`, `SCL` | NC. The product has no I²C (ADR-0018) |
| 11 | `VUSB` | **3V3, tied to `VDD`.** §1.6.2.1: with `VDD` at 3.3 V the internal transceiver LDO *cannot* supply `VUSB`, so it must come from the rail. Bypass locally with a ceramic cap. Abs max +4.0 V and `VUSB ≤ VDD + 0.3 V` |
| 12, 13 | `D-`, `D+` | USB-C receptacle |
| 14 | `VSS` | Ground |

**No external parts belong on D+/D−.** §4.1 Table 4-1 note 1: the lines have built-in impedance
matching, and "no external resistors, capacitors or magnetic components are necessary on the
D+/D- signal paths". This narrows what D24 originally omitted rather than reversing it — if ESD
protection is wanted anyway, it must be a genuinely low-capacitance TVS, chosen deliberately.

USB-C sink duties are unchanged: 5.1 kΩ CC pull-downs, and ≤ 10 µF effective across `VBUS`
(§1.6.2.2) or inrush limiting.

## Swap freedom at layout time

Final placement may move any of these within its class without reopening ADR-0023. Anything not
listed is fixed.

| Signal | May move to |
|---|---|
| lamp red `WO1` | `PA17` (pin 7) |
| lamp yellow `WO2` | `PA18` (pin 8) |
| lamp green `WO0` | **nowhere** — see above |
| module UART | `PA10`+`PA11` mux D (pins 4/5) — *but that puts the module link in the MVIO domain* |
| console UART | `PA08`+`PA09` mux C (MVIO), or `PA24`+`PA25` mux D (spends `XOSC32K`) |
| module `MCLR`, module `INTOUT`, button | any free pad; prefer one with `EXTINT` for `INTOUT` and the button |
