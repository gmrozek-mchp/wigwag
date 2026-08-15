# Schematic — net-level spec

Plan item 20. **One flat sheet, 11 × 17 in (ANSI B / US Ledger), landscape** — D131.

This is the complete connection spec: every part, every pin, every net. The KiCad schematic is
generated *from* this document, and this document stays the artifact of record if the two drift.

Read alongside:
- [`PINOUT.md`](PINOUT.md) — the MCU pin map (ADR-0023). Every MCU net below traces to a row there.
- [`STACKUP.md`](STACKUP.md) — layer, placement and capacitance rules (ADR-0024). Values here obey it.

## Sources for anything not already in PINOUT.md / STACKUP.md

| Fact | Source |
|---|---|
| Complete module pinout, all 29 positions including `NC`, `PTA` and `RTCC` pins | `DS70005544C` [Table 2-1 / §2.1](https://onlinedocs.microchip.com/oxy/GUID-3DDF7D25-768C-4E66-8343-E56E3AE4B4BB-en-US-4/GUID-1309E9EF-4273-4159-82C7-10C35C2174B5.html) |
| Module pin 11: **"Do not leave this pin unconnected"** — connect to a tri-stated host I/O or a switch | Table 2-1 note 5 |
| Module pins 21/22 RTCC: current firmware does not support it; mount option *recommended* | Table 2-1 note 4 |
| Module pin 24 `TP`: **do not connect any external 1.5 V supply** | Table 2-1 note 3 |
| I²C is not user-configurable — it is the Trust&Go device's own bus | App Developer's Guide §2.2 note 4 |
| Strap1/Strap2 both low ⇒ UART1 host interface | `DS70005544` §2.2 Table 2-2 |
| MCP2221A pin 4 `RST` has an internal pull-up; `GP0`–`GP3` factory defaults | `DS20005565E` Table 1-1 |
| MCP1826S 3-pin: 1 `VIN`, 2 `GND`, 3 `VOUT`; exposed tab at ground potential | `DS20002057D` Table 3-1, §3.7 |
| **PICkit `Cortex SWD` column puts `SWDIO` on connector pin 8, `SWO` on pin 4** — a 6-pin header "will result in the loss of functions on Pins 7 and 8 affecting … SWD" | PICkit 5 UG [§3.3.1](https://onlinedocs.microchip.com/oxy/GUID-8D61C0B9-A97F-4F4D-99F8-1D7424264C2A-en-US-1/GUID-2E07C091-C3CD-4DE3-9187-80FA1E63E969.html) |
| SAM/PIC32C SWD → 10-pin adapter mapping; `MCLR` "recommended"; `SWO` "not implemented on all devices" | PICkit 5 UG [§3.3.3.2.2](https://onlinedocs.microchip.com/oxy/GUID-8D61C0B9-A97F-4F4D-99F8-1D7424264C2A-en-US-1/GUID-CDE77A07-EC39-4F0F-B912-EF9A332EA1EF.html) |
| PICkit Basic **ships an 8-pin-to-10-pin Arm SWD adapter board** plus a 12 cm flat cable | PICkit Basic UG [§3.3.2](https://onlinedocs.microchip.com/oxy/GUID-E7CBBF9B-B23F-4E8A-8B9D-C66C24729842-en-US-1/GUID-836280D1-D17B-4DE6-AF76-4970BF9C9A45.html) |

## Nets

| Net | Source | Feeds |
|---|---|---|
| `+5V` | J1 `VBUS` | lamp anodes (via per-colour R), FB1. **This is the "lamp rail"** (`CONTEXT.md`) |
| `+5V_LDO` | FB1 out | U4 `VIN` only — the node the bulk sits on, behind the inrush option |
| `+3V3` | U4 `VOUT` | U1, U2, U3 |
| `GND` | J1, all | everything; L2 plane |

`+5V` and `+5V_LDO` are split so the 10 µF bulk sits **behind** FB1. That keeps the inrush-limiting
option (D135) effective without putting a series element in the lamp current path, where a resistor
would eat the green LED's headroom (D26 exists because that headroom is already tight).

---

## Block 1 — Power in

| Ref | Value | Pkg | Connections | Note |
|---|---|---|---|---|
| **J1** | USB-C receptacle, 16-pin USB 2.0 | SMD | see below | D127 |
| R1 | 5.1 k | 0402 | J1 `CC1` → `GND` | sink advertisement |
| R2 | 5.1 k | 0402 | J1 `CC2` → `GND` | both, not one |
| FB1 | **0 Ω** | 0805 | `+5V` → `+5V_LDO` | inrush option, D135. Ferrite-compatible pad |
| **U4** | MCP1826S-3302E/DB | SOT-223-3 | 1 `VIN`←`+5V_LDO`, 2 `GND`, 3 `VOUT`→`+3V3`, tab→`GND` | D23 |
| C1 | **10 µF** X5R 16 V | **1206** | `+5V_LDO` → `GND` | LDO `CIN`; 1206 takes 22 µF (D135) |
| C2 | 100 nF | 0402 | `+5V_LDO` → `GND` | at U4 pin 1 |
| C3 | 4.7 µF X7R 16 V | 0805 | `+3V3` → `GND` | LDO `COUT`, datasheet's characterisation value |
| C4 | 100 nF | 0402 | `+3V3` → `GND` | at U4 pin 3 |

**J1 pin assignment** — D+/D− must be tied across both sides or the cable only works one way up:

| J1 pins | Net |
|---|---|
| A1, B12, A12, B1 | `GND` |
| A4, B9, A9, B4 | `+5V` |
| A5 (`CC1`) | R1 → `GND` |
| B5 (`CC2`) | R2 → `GND` |
| A6 **and** B6 | `USB_DP` |
| A7 **and** B7 | `USB_DM` |
| A8, B8 (`SBU`) | no-connect |
| Shield | `GND` |

**Nothing else on `USB_DP`/`USB_DM`** — no resistors, no caps, no common-mode choke. Table 4-1
note 1 is explicit that the MCP2221A integrates its own termination (D127). Any ESD part would have
to be a deliberately chosen low-capacitance TVS, and none is fitted.

---

## Block 2 — USB-serial bridge

| Ref | Value | Pkg | Connections |
|---|---|---|---|
| **U3** | MCP2221A-I/ST | TSSOP-14 | table below |
| C5 | 100 nF | 0402 | U3 `VDD` → `GND` |
| C6 | **470 nF** | 0402 | U3 `VUSB` → `GND` |

| U3 pin | Name | Net | Note |
|---:|---|---|---|
| 1 | `VDD` | `+3V3` | self-powered mode, so UART levels are 3.3 V |
| 2 | `GP0` | *no-connect* | factory default `LED_URx`; activity-LED option not fitted |
| 3 | `GP1` | *no-connect* | factory default `LED_UTx` |
| 4 | `RST` | *no-connect* | internal pull-up; no external part |
| 5 | `URx` | `CONSOLE_TX` ← U1 `PA22` | |
| 6 | `UTx` | `CONSOLE_RX` → U1 `PA23` | |
| 7 | `GP2` | *no-connect* | **not `USBCFG`, not wired to the MCU** — D116 |
| 8 | `GP3` | *no-connect* | factory default `LED_I2C` |
| 9, 10 | `SDA`, `SCL` | *no-connect* | the product has no I²C (ADR-0018) |
| 11 | `VUSB` | `+3V3` | tied to `VDD`; the internal LDO cannot supply it at 3.3 V (§1.6.2.1) |
| 12 | `D-` | `USB_DM` | |
| 13 | `D+` | `USB_DP` | |
| 14 | `VSS` | `GND` | |

`VUSB` ≤ `VDD` + 0.3 V is satisfied by construction — they are the same net.

---

## Block 3 — Host MCU

| Ref | Value | Pkg | Connections |
|---|---|---|---|
| **U1** | PIC32CM6408PL10028-I/SS | SSOP-28 | table below |
| C7 | 100 nF | 0402 | `+3V3` → `GND`, at pins 14/15 |
| C8 | 100 nF | 0402 | `+3V3` → `GND`, at pins 20/21 |
| C9 | 100 nF | 0402 | `+3V3` → `GND`, at pin 6 (`VDDIO2`) |
| R3 | **1 k** | 0402 | `SWCLK` → `+3V3` |
| R4 | 10 k | 0402 | `SWDIO` → `+3V3` |
| R5 | 10 k | 0402 | `RESET` → `+3V3` |
| C10 | 100 nF | 0402 | `RESET` → `GND` — **DNP** |
| SW1 | tactile, SPST | THT | `BUTTON` → `GND` |
| **J2** | **Cortex Debug, 10-pin 1.27 mm (2×5), shrouded + keyed** | SMD/THT | table below |
| **J3** | 4 pads / header, 2.54 mm | THT | 1 `+3V3`, 2 `GND`, 3 `MOD_TX`, 4 `MOD_RX` |

**Three caps, not one.** §40.3.1 requires one per supply *pin pair* — 14/15 and 20/21 — and
§40.3.1.3 requires one on `VDDIO2` even though D50 ties it to `VDD`. C8 is the easy one to forget.
All three go on the same side as U1, and the rail reaches the cap *before* the pin (§40.3.1).

**R3 is not optional.** Table 40-6 calls the 1 kΩ `SWCLK` pull-up "critical for reliable
operation". C10 is DNP because a cap on `RESET` can upset some debuggers' reset timing; the pad
exists if noise immunity turns out to matter more.

### J2 — Cortex Debug, 10-pin

| J2 pin | Standard name | Net | Note |
|---:|---|---|---|
| 1 | `VTref` | `+3V3` | target voltage reference; the debugger levels-shift from it |
| 2 | `SWDIO` | `SWDIO` ← U1.10 `PA20` | |
| 3 | `GND` | `GND` | |
| 4 | `SWCLK` | `SWCLK` ← U1.19 `PA31` | R3 1 kΩ pull-up |
| 5 | `GND` | `GND` | |
| 6 | `SWO` | *no-connect* | **no ITM on Cortex-M0+** — there is no SWO to bring out |
| 7 | `KEY` | — | pin omitted from the header, which is what keys it |
| 8 | `NC`/`TDI` | *no-connect* | see the SWO note below |
| 9 | `GND` | `GND` | |
| 10 | `nRESET` | `RESET` ← U1.18 `PA30` | R5 10 kΩ pull-up, C10 DNP |

**Why not a 5-pin ICSP header.** PICkit 5 §3.3.1's `Cortex SWD` column puts **`SWDIO` on connector
pin 8** and `SWO` on pin 4 — so a 5- or 6-pin header carries no SWD at all. The doc says so
directly: *"Use of a 6-pin header will result in the loss of functions on Pins 7 and 8 affecting
EJTAG, JTAG, SWD and ISP."* This connector is what the tools expect: **PICkit Basic ships an
8-pin-to-10-pin Arm SWD adapter board and a 12 cm cable** in the box (§3.3.2), PICkit 5 reaches it
through its Debugger Adapter Board (§3.3.3.2.2), and Atmel-ICE and J-Link Mini/EDU use it natively.

**One documented inconsistency, on the one pin we do not use.** §3.3.3.2.2 maps PICkit `SWO`
(its pin 4) to **10-pin adapter pin 8**, where the ARM Cortex Debug standard places `SWO` on
**pin 6** and `TDI` on pin 8. Pins 1/2/3/4/5/9/10 agree between the two, and those are all this
board needs. `PA20`, `PA31` and `PA30` are the only debug signals the die has — the PL10 is
Cortex-M0+ with no ITM (ADR-0001), so there is no SWO either way. **Both pin 6 and pin 8 are left
unconnected**, which is correct under either reading.

**J3 is the D59 breakout, and it is how this board is brought up without a Wi-Fi module** — see
*Bring-up without the module* below.

### U1 pin table

| Pin | Pad | Net | Note |
|---:|---|---|---|
| 1 | `PA07` | `MOD_RTS` → R14 → U2.15 | `SERCOM0 PAD[3]` = MCU **CTS in** ← module RTSn. DNP |
| 2 | `PA08` | *no-connect* | free (MVIO) |
| 3 | `PA09` | *no-connect* | free (MVIO) |
| 4 | `PA10` | *no-connect* | free (MVIO) |
| 5 | `PA11` | `MOD_RSVD11` → R17 → U2.11 | **held tri-state in firmware.** Table 2-1 note 5. D137 |
| 6 | `VDDIO2` | `+3V3` | C9. Tied to `VDD` per §3.2.3 / D50 |
| 7 | `PA17` | *no-connect* | free — standby `TCC0 WO1` |
| 8 | `PA18` | *no-connect* | free — standby `TCC0 WO2` |
| 9 | `PA19` | `MOD_INT` ← R12 ← U2.13 | module `INTOUT`, active low |
| 10 | `PA20` | `SWDIO` → **J2.2**, R4 | reserved |
| 11 | `PA21` | `BUTTON` → SW1 | internal pull-up, active low |
| 12 | `PA22` | `CONSOLE_TX` → U3.5 | `SERCOM1 PAD[2]`, mux C, `txpo = 1` |
| 13 | `PA23` | `CONSOLE_RX` ← U3.6 | `SERCOM1 PAD[3]`, mux C, `rxpo = 3` |
| 14 | `VDD` | `+3V3` | C7 |
| 15 | `GND` | `GND` | |
| 16 | `PA24` | *no-connect* | free — `XTAL32K1`, deliberately clear (D126) |
| 17 | `PA25` | *no-connect* | free — `XTAL32K2`, deliberately clear (D126) |
| 18 | `PA30` | `RESET` → **J2.10**, R5, C10 | reserved |
| 19 | `PA31` | `SWCLK` → **J2.4**, **R3 1 k** | reserved |
| 20 | `VDD` | `+3V3` | C8 |
| 21 | `GND` | `GND` | |
| 22 | `PA00` | `LAMP_G` → R18 → Q1 gate | `TCC0 WO0`, mux F |
| 23 | `PA01` | `LAMP_R` → R19 → Q2 gate | `TCC0 WO1`, mux F |
| 24 | `PA02` | `LAMP_Y` → R20 → Q3 gate | `TCC0 WO2`, mux F |
| 25 | `PA03` | `MOD_MCLR` → R11 → U2.4 | drive low to reset the module |
| 26 | `PA04` | `MOD_TX` → R9 → U2.19 | `SERCOM0 PAD[0]`, mux C, `txpo = 0` |
| 27 | `PA05` | `MOD_RX` ← R10 ← U2.14 | `SERCOM0 PAD[1]`, mux C, `rxpo = 1` |
| 28 | `PA06` | `MOD_CTS` → R13 → U2.16 | `PAD[2]` = MCU **RTS out** → module CTSn. DNP |

Note the direction pairing on pins 1 and 28: the MCU's CTS **input** takes the module's RTSn
**output**, and vice versa. Getting this backwards is the classic flow-control error, and
`PINOUT.md` already had it right.

---

## Block 4 — Wi-Fi module

| Ref | Value | Pkg | Connections | Note |
|---|---|---|---|---|
| **U2** | RNWF02PC-I/100 | 28-pin module | table below | D21 |
| R7 | **1.2 k** | 0402 | U2.2 `I2C_SCL` → `+3V3` | required on the `PC` variant |
| R8 | **1.2 k** | 0402 | U2.3 `I2C_SDA` → `+3V3` | internal Trust&Go bus, not an MCU bus |
| R9 | 33 Ω | 0402 | `MOD_TX`: U1.26 ↔ U2.19 | series + isolation, D136 |
| R10 | 33 Ω | 0402 | `MOD_RX`: U1.27 ↔ U2.14 | series + isolation, D136 |
| R11 | 33 Ω | 0402 | `MOD_MCLR`: U1.25 ↔ U2.4 | D136 |
| R12 | 33 Ω | 0402 | `MOD_INT`: U2.13 ↔ U1.9 | D136 |
| R13 | 33 Ω | 0402 | `MOD_CTS`: U1.28 ↔ U2.16 | **DNP** — D128 + D136 in one part |
| R14 | 33 Ω | 0402 | `MOD_RTS`: U2.15 ↔ U1.1 | **DNP** — as above |
| R15 | 10 k | 0402 | U2.10 `Strap1` → `GND` | pull-down, *not* a hard short |
| R16 | 10 k | 0402 | U2.26 `Strap2` → `GND` | pull-down, *not* a hard short |
| R17 | 33 Ω | 0402 | `MOD_RSVD11`: U1.5 ↔ U2.11 | D137 |
| C12 | **4.7 µF** X7R | **1206** | U2.20 `VDD` → `GND` | bulk, at the pin. 1206 takes 22 µF (D135) |
| C13 | 100 nF | 0402 | U2.20 `VDD` → `GND` | decoupling, at the pin |
| C14 | 100 nF | 0402 | U2.23 `VDDIO` → `GND` | §2.2.1 |
| TP1 | test point | — | U2.10 `Strap1` | so DFU stays reachable |
| TP2 | test point | — | U2.26 `Strap2` | so DFU stays reachable |
| TP3 | test point | — | U2.27 `UART2_TX` | module debug log, 460 800 8N1, TX only |
| TP4 | test point | — | U2.24 `TP` | **1.5 V PMU output — probe only, do not load** |

**R9–R14 and R17 do three jobs each**, which is why they are all populated as resistors rather than
zero-ohm links: §2.4's recommended series element (D136), the stuffing option for flow control
(D128, on R13/R14), and **isolation jumpers** — pull R9 and R10 and the module is electrically
detached from `SERCOM0`, which is what makes the next section possible on a fully-built board.

**Strap1/Strap2 are pulled down through resistors with test points, never shorted.** Both low
selects the UART1 host interface (§2.2 Table 2-2), which is what this design wants — but they are
also the module's DFU port. A hard short saves two resistors and permanently forecloses module
firmware update.

### U2 pin table

| Pin | Name | Net | Note |
|---:|---|---|---|
| 1 | `NC` | *no-connect* | |
| 2 | `I2C_SCL` | R7 → `+3V3` | Trust&Go bus, internal to the module |
| 3 | `I2C_SDA` | R8 → `+3V3` | |
| 4 | `MCLR` | ← R11 ← U1.25 | active low |
| 5 | `PTA_WLAN_ACTIVE` | *no-connect* | Bluetooth coexistence, unused |
| 6 | `PTA_BT_PRIO` | *no-connect* | unused |
| 7 | `Reserved` | *no-connect* | **do not connect** |
| 8 | `NC` | *no-connect* | |
| 9 | `GND` | `GND` | |
| 10 | `DFU_RX`/`Strap1` | R15 → `GND`, TP1 | |
| 11 | `Reserved` | → R17 → U1.5 `PA11` | **note 5: do not leave unconnected.** D137 |
| 12 | `GND` | `GND` | |
| 13 | `INTOUT` | → R12 → U1.9 | active low |
| 14 | `UART1_TX` | → R10 → U1.27 | module out → MCU RX. **230 400 default, D128** |
| 15 | `UART1_RTSn` | → R14 → U1.1 | module out → MCU CTS in. DNP |
| 16 | `UART1_CTSn` | ← R13 ← U1.28 | MCU RTS out → module in. DNP |
| 17 | `Reserved` | *no-connect* | **do not connect** |
| 18 | `Reserved` | *no-connect* | **do not connect** |
| 19 | `UART1_RX` | ← R9 ← U1.26 | MCU TX → module in |
| 20 | `VDD` | `+3V3` | C12 + C13 |
| 21 | `RTCC_OSC_IN` | *no-connect* | option declined — see below |
| 22 | `RTCC_OSC_OUT` | *no-connect* | option declined |
| 23 | `VDDIO` | `+3V3` | C14. Same rail as `VDD` (D50) |
| 24 | `TP` | TP4 only | 1.5 V PMU out. **Note 3: connect no external supply** |
| 25 | `NC` | *no-connect* | |
| 26 | `DFU_TX`/`Strap2` | R16 → `GND`, TP2 | |
| 27 | `UART2_TX` | TP3 | debug log out; **there is no `UART2_RX`** |
| 28 | `GND` | `GND` | |
| 29 | `GND` paddle | `GND` | thermal pad — **must be soldered** (§2.3), stitched (§2.4) |

**The RTCC option is declined** (pins 21/22 left `NC`). Table 2-1 note 4 recommends providing a
mount option for future module firmware, and this design does not, for two reasons: D126 already
kept the MCU's own `XOSC32K` free for timekeeping, making a module-side RTCC redundant for any
plausible revision; and §2.4 would constrain it to a via-free trace immediately beside the module,
which is the most congested strip on the board. Recorded so a later session sees a decision rather
than an omission.

---

## Block 5 — Lamps and button

Three identical channels. Low-side N-FET, LED on the 5 V rail, all `PWM_POLARITY_NORMAL` (D26).

| Lamp | MCU | Gate R | Gate pulldown | FET | LED | Current R |
|---|---|---|---|---|---|---|
| green | `PA00` pin 22 | R18 100 Ω | R21 100 k | Q1 | D1 | R24 **82 Ω** |
| red | `PA01` pin 23 | R19 100 Ω | R22 100 k | Q2 | D2 | R25 **150 Ω** |
| yellow | `PA02` pin 24 | R20 100 Ω | R23 100 k | Q3 | D3 | R26 **150 Ω** |

Per channel: `+5V` → current R → LED anode; LED cathode → FET drain; FET source → `GND`; FET gate
← gate R ← MCU pin, with the gate pulldown from gate to `GND`.

| Ref | Value | Pkg | Note |
|---|---|---|---|
| Q1–Q3 | BSS138-class logic-level N-FET | SOT-23 | Vgs(th) low enough to fully enhance at 3.3 V |
| D1–D3 | 10 mm diffused LED, green / red / yellow | THT | part not yet chosen |
| R18–R20 | 100 Ω | 0402 | gate series |
| R21–R23 | 100 k | 0402 | **gate pulldown — D138** |
| R24–R26 | see table | 0805 | ~60 mW at 20 mA; provisional |
| SW1 | tactile SPST | THT | to `GND`; MCU internal pull-up, no external part |

**The gate pulldowns are not optional, and they were missing from item 20's parts list.** MCU pins
are high-impedance inputs at power-on reset, so without them all three gates float from POR until
firmware drives them — the lamps would sit in an indeterminate state at boot, which is precisely
the fail-*invisible* behaviour Rule 4 exists to prevent. 100 k against a 3.3 V drive is
electrically free.

**R24–R26 are provisional.** They are computed for ~20 mA at an assumed Vf — green ~3.2 V (which is
why it is 82 Ω and why D26 needs the 5 V rail at all), red ~2.0 V, yellow ~2.1 V — and the LED part
is not chosen yet. Recompute from the real Vf as
`R = (5.0 − Vf − Vds_on) / I`, then match *perceived* brightness in firmware (Phase 5 item 26),
not by re-picking resistors: equal current across three colours does not look equal.

**SW1 has no external part** as item 20 specifies. It is user-touchable and goes straight to an MCU
pin, so an ESD/debounce cap is defensible — deliberately not fitted, and noted here so the choice
is visible if a unit ever fails from a static discharge.

---

## Mounting holes

Four, positions from Phase 4. **The two nearest the antenna must be nylon-only or omitted** — metal
fasteners within 31.75 mm of the antenna violate §2.3, and the antenna is at the top edge (D134).
The base pair may be metal.

## Bring-up without the module

Worth stating plainly, because it is the reason J3 exists and because the module is a long-lead
part: **this board can be fully exercised with U2 unpopulated.**

1. Leave U2 off. Fit everything else.
2. Attach a USB-serial adapter to **J3** (`+3V3`, `GND`, `MOD_TX`, `MOD_RX`).
3. Run `firmware/sim/fake_rnwf02.py` against it — the same simulator ADR-0015 built, which is how
   the AT client was developed and how Phases 1–2 were verified.
4. Console and lamps work normally over USB-C, since neither depends on U2.

On a board that *does* have U2 populated, pull **R9** and **R10** to detach the module and do the
same thing. That is the third job those series resistors are earning.

## Stuffing options in one place

| Option | Parts | Fitted by default? | Gated on |
|---|---|---|---|
| UART flow control | R13, R14 | **No** | D128 — measure the 230 400 link |
| More module/`VBUS` bulk | C1, C12 → 22 µF in the same 1206 pads | No (10 µF / 4.7 µF) | D135 — measure the rail through a TX burst |
| `VBUS` inrush limiting | FB1: 0 Ω → ferrite or R | No (0 Ω) | D135, if bulk grows past 10 µF |
| `RESET` noise cap | C10 | **No** | only if reset proves noisy |
| Bridge activity LEDs | U3 `GP0`/`GP1` | No | cosmetic; factory defaults already drive them |

Every open question on this board resolves into this table — a BOM change, not a respin. That is
deliberate (ADR-0023, ADR-0024).

## The generated schematic, and what ERC actually says

`hardware/kicad/wigwag.kicad_sch` is **generated from this document** by
[`gen_schematic.py`](gen_schematic.py) — regenerate rather than hand-editing:

```
python3 hardware/gen_schematic.py
kicad-cli sch erc --output erc.rpt hardware/kicad/wigwag.kicad_sch
```

Connectivity is by **net label on a short stub**, not drawn wire, so component placement is purely
cosmetic — rearranging in Eeschema cannot change the netlist. The generated placement is spaced to
be legible, not pretty; tidy it in the GUI freely.

**ERC status: 1 violation, and it is the expected one** — `footprint_link_issues` on U2, because
`wigwag:RNWF02PC_Module` has to be drawn from the package drawing (item 22). KiCad's own exported
netlist was checked against the tables above: 46 named nets, membership identical, plus 29
deliberately unconnected pins. Verified on **KiCad 10.0.5**.

Two things learned from making ERC clean, both worth keeping:

- **Datasheet-`NC` pins get no flag and no stub.** Pins declared with electrical type `no_connect`
  in the symbol — U2's 1, 7, 8, 17, 18, 25 and J2's 7, 8 — are left bare, because KiCad treats
  both a stub and an NC flag on such a pin as errors. The pin *type* is the documentation, and it
  makes ERC complain if anyone ever wires them, which is stronger than a flag. **The 21 functional
  pins we choose not to use** — U1's free pads, U3's `GP0`–`GP3`/`RST`/`SDA`/`SCL`, J1's `SBU`,
  J2's `SWO` — do get explicit flags, since for those "considered and declined" is a real claim.
- **`+5V_LDO` needs its own power flag.** It is a separate net from `+5V` because FB1 sits between
  them (D135), so a flag on `+5V` does not drive the LDO's `VIN`. Three flags total: `+5V`,
  `+5V_LDO`, `GND`. `+3V3` needs none — U4 pin 3 is typed `power_out`.

Also: DNP parts (R13, R14, C10) still appear in the netlist, as they should — DNP is a fab/BOM
attribute, not an electrical one.

## Open

- **D135** and **D128** — both measurements, both need hardware, neither blocks this schematic.
- **D1–D3 part choice**, which sets R24–R26.
- **J1 exact receptacle part**, which sets the footprint and the mechanical cutout Phase 4 needs.
