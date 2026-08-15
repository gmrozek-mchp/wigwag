#!/usr/bin/env python3
"""Generate hardware/kicad/wigwag.kicad_sch from the netlist in SCHEMATIC.md.

The netlist below is the machine-readable twin of `hardware/SCHEMATIC.md`, which stays the
artifact of record (D131). Regenerate rather than hand-editing the .kicad_sch:

    python3 hardware/gen_schematic.py

Design notes
------------
* **Connectivity is by net label, not drawn wire.** Every pin gets a 2.54 mm stub with a local
  label on its end. That is electrically identical to routing and is robust to generate; it also
  means placement is purely cosmetic, so rearranging in the GUI cannot break the netlist.
* **Symbols are embedded** in `lib_symbols`, so the file opens standalone without any external
  library installed. Pin numbers, names and electrical types are authored here from the
  datasheet tables cited in SCHEMATIC.md.
* **UUIDs are derived by hash**, so regenerating an unchanged netlist produces an identical file
  and the diff stays meaningful.
* Library Y is up, schematic Y is down: absolute pin y = component y - library pin y.
"""

import uuid
from pathlib import Path

NS = uuid.UUID("6f9b1e42-0000-4000-8000-776967776167")  # stable namespace: "wigwag"
def uid(key): return str(uuid.uuid5(NS, key))

ROOT = uid("root-sheet")
PROJECT = "wigwag"
PITCH = 2.54
STUB = 2.54

# ─────────────────────────────────────────────────────────────────────────────
# Symbol definitions.  etype ∈ passive power_in power_out input output bidirectional
#                             open_collector no_connect unspecified free
# ─────────────────────────────────────────────────────────────────────────────

PASSIVES = {
    # name          pin1   pin2   graphic
    "R":        (("1", "~"), ("2", "~"), "rect"),
    "C":        (("1", "~"), ("2", "~"), "cap"),
    "FB":       (("1", "~"), ("2", "~"), "rect"),
    "LED":      (("1", "A"), ("2", "K"), "led"),
    "SW_Push":  (("1", "1"), ("2", "2"), "sw"),
}

# Multi-pin symbols: (left pins, right pins) as (number, name, etype)
ICS = {
"PIC32CM6408PL10028": ([
    ("1",  "PA07",    "bidirectional"), ("2",  "PA08", "bidirectional"),
    ("3",  "PA09",    "bidirectional"), ("4",  "PA10", "bidirectional"),
    ("5",  "PA11",    "bidirectional"), ("6",  "VDDIO2", "power_in"),
    ("7",  "PA17",    "bidirectional"), ("8",  "PA18", "bidirectional"),
    ("9",  "PA19",    "bidirectional"), ("10", "PA20/SWDIO", "bidirectional"),
    ("11", "PA21",    "bidirectional"), ("12", "PA22", "bidirectional"),
    ("13", "PA23",    "bidirectional"), ("14", "VDD",  "power_in"),
], [
    ("28", "PA06",    "bidirectional"), ("27", "PA05", "bidirectional"),
    ("26", "PA04",    "bidirectional"), ("25", "PA03", "bidirectional"),
    ("24", "PA02",    "bidirectional"), ("23", "PA01", "bidirectional"),
    ("22", "PA00",    "bidirectional"), ("21", "GND",  "power_in"),
    ("20", "VDD",     "power_in"),      ("19", "PA31/SWCLK", "bidirectional"),
    ("18", "PA30/RESET", "input"),      ("17", "PA25", "bidirectional"),
    ("16", "PA24",    "bidirectional"), ("15", "GND",  "power_in"),
]),

"RNWF02PC": ([
    ("1",  "NC",           "no_connect"),  ("2",  "I2C_SCL",      "bidirectional"),
    ("3",  "I2C_SDA",      "bidirectional"),("4", "MCLR",         "input"),
    ("5",  "PTA_WLAN_ACT", "output"),      ("6",  "PTA_BT_PRIO",  "input"),
    ("7",  "RSVD7",        "no_connect"),  ("8",  "NC",           "no_connect"),
    ("9",  "GND",          "power_in"),    ("10", "DFU_RX/STRAP1","bidirectional"),
    ("11", "RSVD11",       "bidirectional"),("12","GND",          "power_in"),
    ("13", "INTOUT",       "output"),      ("14", "UART1_TX",     "output"),
    ("15", "UART1_RTSn",   "output"),
], [
    ("29", "GND_PADDLE",   "power_in"),    ("28", "GND",          "power_in"),
    ("27", "UART2_TX",     "output"),      ("26", "DFU_TX/STRAP2","bidirectional"),
    ("25", "NC",           "no_connect"),  ("24", "TP",           "passive"),
    ("23", "VDDIO",        "power_in"),    ("22", "RTCC_OSC_OUT", "output"),
    ("21", "RTCC_OSC_IN",  "input"),       ("20", "VDD",          "power_in"),
    ("19", "UART1_RX",     "input"),       ("18", "RSVD18",       "no_connect"),
    ("17", "RSVD17",       "no_connect"),  ("16", "UART1_CTSn",   "input"),
]),

"MCP2221A": ([
    ("1", "VDD",  "power_in"), ("2", "GP0",  "bidirectional"),
    ("3", "GP1",  "bidirectional"), ("4", "RST", "input"),
    ("5", "URx",  "input"),     ("6", "UTx",  "output"),
    ("7", "GP2",  "bidirectional"),
], [
    ("14", "VSS",  "power_in"), ("13", "D+",   "bidirectional"),
    ("12", "D-",   "bidirectional"), ("11", "VUSB", "power_in"),
    ("10", "SCL",  "bidirectional"), ("9",  "SDA",  "bidirectional"),
    ("8",  "GP3",  "bidirectional"),
]),

    # 3 pins only: the SOT-223 tab *is* pad 2 in KiCad's TabPin2 footprint (§3.7 says the
    # exposed tab is at ground potential), so a separate TAB pin would have no pad to map to.
"MCP1826S": ([
    ("1", "VIN", "power_in"), ("2", "GND", "power_in"),
], [
    ("3", "VOUT", "power_out"),
]),

"USB_C_Receptacle_2.0": ([
    ("A1",  "GND",  "power_in"), ("A4",  "VBUS", "power_in"),
    ("A5",  "CC1",  "bidirectional"), ("A6", "DP1", "bidirectional"),
    ("A7",  "DM1",  "bidirectional"), ("A8", "SBU1", "bidirectional"),
    ("A9",  "VBUS", "power_in"), ("A12", "GND",  "power_in"),
    ("SH",  "SHIELD", "passive"),
], [
    ("B1",  "GND",  "power_in"), ("B4",  "VBUS", "power_in"),
    ("B5",  "CC2",  "bidirectional"), ("B6", "DP2", "bidirectional"),
    ("B7",  "DM2",  "bidirectional"), ("B8", "SBU2", "bidirectional"),
    ("B9",  "VBUS", "power_in"), ("B12", "GND",  "power_in"),
]),

# ARM Cortex Debug, 10-pin 1.27 mm.  D140.
"Cortex_Debug_10": ([
    ("1", "VTref", "passive"), ("3", "GND",   "passive"),
    ("5", "GND",   "passive"), ("7", "KEY",   "no_connect"),
    ("9", "GND",   "passive"),
], [
    ("2", "SWDIO", "passive"), ("4", "SWCLK", "passive"),
    ("6", "SWO",   "passive"), ("8", "NC",    "no_connect"),
    ("10","nRESET","passive"),
]),

"Conn_01x04": ([
    ("1", "1", "passive"), ("2", "2", "passive"),
    ("3", "3", "passive"), ("4", "4", "passive"),
], []),

"TestPoint": ([("1", "1", "passive")], []),
"PWR_FLAG":  ([("1", "pwr", "power_out")], []),
}

# ─────────────────────────────────────────────────────────────────────────────
# The netlist.  {net: value} per pin.  "~" marks an intentional no-connect.
# ─────────────────────────────────────────────────────────────────────────────

P = []   # (ref, sym, value, footprint, x, y, {pin: net}, dnp)
def part(ref, sym, value, fp, x, y, nets, dnp=False, board=True):
    P.append((ref, sym, value, fp, x, y, nets, dnp, board))

# ── Block 1: power in ───────────────────────────────────────────────────────
part("J1", "USB_C_Receptacle_2.0", "USB-C 2.0 16P", "Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal",
     45, 75, {"A1":"GND","A12":"GND","B1":"GND","B12":"GND","SH":"GND",
              "A4":"+5V","A9":"+5V","B4":"+5V","B9":"+5V",
              "A5":"CC1","B5":"CC2",
              "A6":"USB_DP","B6":"USB_DP","A7":"USB_DM","B7":"USB_DM",
              "A8":"~","B8":"~"})
part("R1",  "R",  "5.1k",  "Resistor_SMD:R_0402_1005Metric", 30, 145, {"1":"CC1","2":"GND"})
part("R2",  "R",  "5.1k",  "Resistor_SMD:R_0402_1005Metric", 47, 145, {"1":"CC2","2":"GND"})
part("FB1", "FB", "0R",    "Resistor_SMD:R_0805_2012Metric", 95, 55, {"1":"+5V","2":"+5V_LDO"})
part("U4",  "MCP1826S", "MCP1826S-3302E/DB", "Package_TO_SOT_SMD:SOT-223-3_TabPin2",
     150, 50, {"1":"+5V_LDO","2":"GND","3":"+3V3"})
part("C1",  "C",  "10uF",  "Capacitor_SMD:C_1206_3216Metric", 95, 105, {"1":"+5V_LDO","2":"GND"})
part("C2",  "C",  "100nF", "Capacitor_SMD:C_0402_1005Metric", 112, 105, {"1":"+5V_LDO","2":"GND"})
part("C3",  "C",  "4.7uF", "Capacitor_SMD:C_0805_2012Metric", 145, 105, {"1":"+3V3","2":"GND"})
part("C4",  "C",  "100nF", "Capacitor_SMD:C_0402_1005Metric", 162, 105, {"1":"+3V3","2":"GND"})
part("PF1", "PWR_FLAG", "PWR_FLAG", "", 75, 25, {"1":"+5V"}, board=False)
part("PF2", "PWR_FLAG", "PWR_FLAG", "", 64, 145, {"1":"GND"}, board=False)
part("PF3", "PWR_FLAG", "PWR_FLAG", "", 120, 25, {"1":"+5V_LDO"}, board=False)

# ── Block 2: USB-serial bridge ──────────────────────────────────────────────
part("U3", "MCP2221A", "MCP2221A-I/ST", "Package_SO:TSSOP-14_4.4x5mm_P0.65mm",
     55, 205, {"1":"+3V3","2":"~","3":"~","4":"~","5":"CONSOLE_TX","6":"CONSOLE_RX",
               "7":"~","8":"~","9":"~","10":"~","11":"+3V3","12":"USB_DM",
               "13":"USB_DP","14":"GND"})
part("C5", "C", "100nF", "Capacitor_SMD:C_0402_1005Metric", 100, 205, {"1":"+3V3","2":"GND"})
part("C6", "C", "470nF", "Capacitor_SMD:C_0402_1005Metric", 117, 205, {"1":"+3V3","2":"GND"})

# ── Block 3: host MCU ───────────────────────────────────────────────────────
part("U1", "PIC32CM6408PL10028", "PIC32CM6408PL10028-I/SS", "Package_SO:SSOP-28_5.3x10.2mm_P0.65mm",
     258, 120, {"1":"MOD_RTS","2":"~","3":"~","4":"~","5":"MOD_RSVD11","6":"+3V3",
                "7":"~","8":"~","9":"MOD_INT","10":"SWDIO","11":"BUTTON",
                "12":"CONSOLE_TX","13":"CONSOLE_RX","14":"+3V3","15":"GND",
                "16":"~","17":"~","18":"RESET","19":"SWCLK","20":"+3V3","21":"GND",
                "22":"LAMP_G","23":"LAMP_R","24":"LAMP_Y","25":"MOD_MCLR",
                "26":"MOD_TX","27":"MOD_RX","28":"MOD_CTS"})
part("C7", "C", "100nF", "Capacitor_SMD:C_0402_1005Metric", 235, 200, {"1":"+3V3","2":"GND"})
part("C8", "C", "100nF", "Capacitor_SMD:C_0402_1005Metric", 252, 200, {"1":"+3V3","2":"GND"})
part("C9", "C", "100nF", "Capacitor_SMD:C_0402_1005Metric", 269, 200, {"1":"+3V3","2":"GND"})
part("R3", "R", "1k",    "Resistor_SMD:R_0402_1005Metric", 235, 40, {"1":"SWCLK","2":"+3V3"})
part("R4", "R", "10k",   "Resistor_SMD:R_0402_1005Metric", 252, 40, {"1":"SWDIO","2":"+3V3"})
part("R5", "R", "10k",   "Resistor_SMD:R_0402_1005Metric", 269, 40, {"1":"RESET","2":"+3V3"})
part("C10","C", "100nF", "Capacitor_SMD:C_0402_1005Metric", 286, 40, {"1":"RESET","2":"GND"}, dnp=True)
part("J2", "Cortex_Debug_10", "Cortex Debug 10P 1.27mm", "Connector_PinHeader_1.27mm:PinHeader_2x05_P1.27mm_Vertical",
     258, 245, {"1":"+3V3","2":"SWDIO","3":"GND","4":"SWCLK","5":"GND",
                "6":"~","7":"~","8":"~","9":"GND","10":"RESET"})
part("SW1","SW_Push", "TACTILE", "Button_Switch_THT:SW_PUSH_6mm", 295, 195, {"1":"BUTTON","2":"GND"})
part("J3", "Conn_01x04", "SERCOM0 breakout", "Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical",
     295, 240, {"1":"+3V3","2":"GND","3":"MOD_TX","4":"MOD_RX"})

# ── Block 4: Wi-Fi module ───────────────────────────────────────────────────
part("U2", "RNWF02PC", "RNWF02PC-I/100", "wigwag:RNWF02PC_Module",
     378, 125, {"1":"~","2":"I2C_SCL","3":"I2C_SDA","4":"MOD_MCLR_M","5":"~","6":"~",
                "7":"~","8":"~","9":"GND","10":"STRAP1","11":"MOD_RSVD11_M","12":"GND",
                "13":"MOD_INT_M","14":"MOD_RX_M","15":"MOD_RTS_M","16":"MOD_CTS_M",
                "17":"~","18":"~","19":"MOD_TX_M","20":"+3V3","21":"~","22":"~",
                "23":"+3V3","24":"MOD_TP","25":"~","26":"STRAP2","27":"MOD_DBG_TX",
                "28":"GND","29":"GND"})
part("R7",  "R", "1.2k", "Resistor_SMD:R_0402_1005Metric", 391, 45, {"1":"I2C_SCL","2":"+3V3"})
part("R8",  "R", "1.2k", "Resistor_SMD:R_0402_1005Metric", 408, 45, {"1":"I2C_SDA","2":"+3V3"})
part("R9",  "R", "33R",  "Resistor_SMD:R_0402_1005Metric", 310, 40, {"1":"MOD_TX","2":"MOD_TX_M"})
part("R10", "R", "33R",  "Resistor_SMD:R_0402_1005Metric", 310, 72, {"1":"MOD_RX_M","2":"MOD_RX"})
part("R11", "R", "33R",  "Resistor_SMD:R_0402_1005Metric", 310, 104, {"1":"MOD_MCLR","2":"MOD_MCLR_M"})
part("R12", "R", "33R",  "Resistor_SMD:R_0402_1005Metric", 310, 136, {"1":"MOD_INT_M","2":"MOD_INT"})
part("R13", "R", "33R",  "Resistor_SMD:R_0402_1005Metric", 310, 168, {"1":"MOD_CTS","2":"MOD_CTS_M"}, dnp=True)
part("R14", "R", "33R",  "Resistor_SMD:R_0402_1005Metric", 310, 200, {"1":"MOD_RTS_M","2":"MOD_RTS"}, dnp=True)
part("R17", "R", "33R",  "Resistor_SMD:R_0402_1005Metric", 310, 232, {"1":"MOD_RSVD11","2":"MOD_RSVD11_M"})
part("R15", "R", "10k",  "Resistor_SMD:R_0402_1005Metric", 340, 205, {"1":"STRAP1","2":"GND"})
part("R16", "R", "10k",  "Resistor_SMD:R_0402_1005Metric", 357, 205, {"1":"STRAP2","2":"GND"})
part("C12", "C", "4.7uF","Capacitor_SMD:C_1206_3216Metric", 340, 45, {"1":"+3V3","2":"GND"})
part("C13", "C", "100nF","Capacitor_SMD:C_0402_1005Metric", 357, 45, {"1":"+3V3","2":"GND"})
part("C14", "C", "100nF","Capacitor_SMD:C_0402_1005Metric", 374, 45, {"1":"+3V3","2":"GND"})
part("TP1", "TestPoint", "Strap1",    "TestPoint:TestPoint_Pad_D1.5mm", 374, 205, {"1":"STRAP1"})
part("TP2", "TestPoint", "Strap2",    "TestPoint:TestPoint_Pad_D1.5mm", 387, 205, {"1":"STRAP2"})
part("TP3", "TestPoint", "UART2_TX",  "TestPoint:TestPoint_Pad_D1.5mm", 400, 205, {"1":"MOD_DBG_TX"})
part("TP4", "TestPoint", "MOD_TP_1V5","TestPoint:TestPoint_Pad_D1.5mm", 413, 205, {"1":"MOD_TP"})

# ── Block 5: lamps and button ───────────────────────────────────────────────
LAMPS = [  # colour, MCU net, gate net, anode net, cathode net, Rgate, Rpd, Rled, Rval, Q, D
    ("green",  "LAMP_G", "GATE_G", "LED_G_A", "LED_G_K", "R18", "R21", "R24", "82R",  "Q1", "D1"),
    ("red",    "LAMP_R", "GATE_R", "LED_R_A", "LED_R_K", "R19", "R22", "R25", "150R", "Q2", "D2"),
    ("yellow", "LAMP_Y", "GATE_Y", "LED_Y_A", "LED_Y_K", "R20", "R23", "R26", "150R", "Q3", "D3"),
]
ICS["Q_NMOS_GSD"] = ([("1", "G", "input")], [("3", "D", "passive"), ("2", "S", "passive")])
for i, (col, mcu, gate, anode, cath, rg, rpd, rled, rv, q, d) in enumerate(LAMPS):
    y = 40 + i * 75
    part(rled, "R", rv,  "Resistor_SMD:R_0805_2012Metric", 130, y,      {"1":"+5V","2":anode})
    part(d,    "LED", f"LED_10mm_{col}", "LED_THT:LED_D10.0mm", 130, y+32, {"1":anode,"2":cath})
    part(q,    "Q_NMOS_GSD", "BSS138", "Package_TO_SOT_SMD:SOT-23", 158, y+16, {"1":gate,"2":"GND","3":cath})
    part(rg,   "R", "100R", "Resistor_SMD:R_0402_1005Metric", 182, y, {"1":mcu,"2":gate})
    part(rpd,  "R", "100k", "Resistor_SMD:R_0402_1005Metric", 197, y, {"1":gate,"2":"GND"})

# ─────────────────────────────────────────────────────────────────────────────
# Emitters
# ─────────────────────────────────────────────────────────────────────────────

def eff(hide=False, size=1.27, justify=None):
    j = f" (justify {justify})" if justify else ""
    h = " (hide yes)" if hide else ""
    return f"(effects (font (size {size} {size})){j}{h})"

def sym_pin(num, name, etype, x, y, ang, length=PITCH):
    return (f'      (pin {etype} line (at {x:g} {y:g} {ang}) (length {length:g})\n'
            f'        (name "{name}" {eff()})\n'
            f'        (number "{num}" {eff()})\n'
            f'      )')

def def_passive(name):
    (p1n, p1l), (p2n, p2l), gfx = PASSIVES[name]
    body = {
      "rect": '        (rectangle (start -1.016 -2.54) (end 1.016 2.54)\n'
              '          (stroke (width 0.254) (type default)) (fill (type none)))',
      "cap":  '        (polyline (pts (xy -2.032 -0.762) (xy 2.032 -0.762))\n'
              '          (stroke (width 0.508) (type default)) (fill (type none)))\n'
              '        (polyline (pts (xy -2.032 0.762) (xy 2.032 0.762))\n'
              '          (stroke (width 0.508) (type default)) (fill (type none)))',
      "led":  '        (polyline (pts (xy -1.27 -1.27) (xy -1.27 1.27))\n'
              '          (stroke (width 0.254) (type default)) (fill (type none)))\n'
              '        (polyline (pts (xy 1.27 -1.27) (xy -1.27 0) (xy 1.27 1.27) (xy 1.27 -1.27))\n'
              '          (stroke (width 0.254) (type default)) (fill (type outline)))',
      "sw":   '        (circle (center -1.016 0) (radius 0.508)\n'
              '          (stroke (width 0.254) (type default)) (fill (type none)))\n'
              '        (circle (center 1.016 0) (radius 0.508)\n'
              '          (stroke (width 0.254) (type default)) (fill (type none)))',
    }[gfx]
    hidenum = "(pin_numbers (hide yes))" if name in ("R", "C", "FB", "LED") else ""
    return f'''    (symbol "wigwag:{name}"
      {hidenum} (pin_names (offset 0)) (exclude_from_sim no) (in_bom yes) (on_board yes)
      (property "Reference" "{'R' if name in ('R','FB') else 'C' if name=='C' else 'D' if name=='LED' else 'SW'}" (at 2.54 0 90) {eff()})
      (property "Value" "{name}" (at 0 0 90) {eff()})
      (property "Footprint" "" (at 0 0 0) {eff(hide=True)})
      (symbol "{name}_0_1"
{body}
      )
      (symbol "{name}_1_1"
{sym_pin(p1n, p1l, "passive", 0, 3.81, 270)}
{sym_pin(p2n, p2l, "passive", 0, -3.81, 90)}
      )
    )'''

def def_ic(name):
    left, right = ICS[name]
    n = max(len(left), len(right))
    half_w = 17.78 if n > 6 else 10.16
    top = ((n - 1) * PITCH) / 2.0
    h = top + PITCH * 1.5
    pins, geo = [], []
    for i, (num, pname, et) in enumerate(left):
        y = top - i * PITCH
        pins.append(sym_pin(num, pname, et, -(half_w + PITCH), y, 0))
    for i, (num, pname, et) in enumerate(right):
        y = top - i * PITCH
        pins.append(sym_pin(num, pname, et, half_w + PITCH, y, 180))
    if name in ("TestPoint", "PWR_FLAG"):
        geo = ['        (circle (center 0 1.27) (radius 0.762)\n'
               '          (stroke (width 0.254) (type default)) (fill (type none)))']
        pins = [sym_pin("1", ICS[name][0][0][1], ICS[name][0][0][2], 0, 0, 90, 0)]
    else:
        geo = [f'        (rectangle (start {-half_w:g} {h:g}) (end {half_w:g} {-h:g})\n'
               f'          (stroke (width 0.254) (type default)) (fill (type background)))']
    return f'''    (symbol "wigwag:{name}"
      (pin_names (offset 1.016)) (exclude_from_sim no) (in_bom yes) (on_board yes)
      (property "Reference" "U" (at 0 {h + 2.54:g} 0) {eff()})
      (property "Value" "{name}" (at 0 {-(h + 2.54):g} 0) {eff()})
      (property "Footprint" "" (at 0 0 0) {eff(hide=True)})
      (symbol "{name}_0_1"
{chr(10).join(geo)}
      )
      (symbol "{name}_1_1"
{chr(10).join(pins)}
      )
    )'''

def pin_geometry(sym):
    """Return {pin: (dx, dy, side, etype)} in library coords."""
    out = {}
    if sym in PASSIVES:
        (p1n, _), (p2n, _), _ = PASSIVES[sym]
        out[p1n] = (0, 3.81, "up", "passive")
        out[p2n] = (0, -3.81, "down", "passive")
        return out
    left, right = ICS[sym]
    n = max(len(left), len(right))
    if sym in ("TestPoint", "PWR_FLAG"):
        return {"1": (0, 0, "down", ICS[sym][0][0][2])}
    half_w = 17.78 if n > 6 else 10.16
    top = ((n - 1) * PITCH) / 2.0
    for i, (num, _, et) in enumerate(left):
        out[num] = (-(half_w + PITCH), top - i * PITCH, "left", et)
    for i, (num, _, et) in enumerate(right):
        out[num] = (half_w + PITCH, top - i * PITCH, "right", et)
    return out

GRID = 1.27
def snap(v):
    """KiCad's default 1.27 mm grid. Off-grid endpoints are an ERC error."""
    return round(v / GRID) * GRID

DELTA = {"left": (-STUB, 0), "right": (STUB, 0), "up": (0, -STUB), "down": (0, STUB)}
JUST  = {"left": "right", "right": "left", "up": "left", "down": "left"}

body, used = [], set()
nc_count = skipped_nc = 0
for ref, sym, value, fp, cx, cy, nets, dnp, board in P:
    used.add(sym)
    cx, cy = snap(cx), snap(cy)
    props = [
        f'    (property "Reference" "{ref}" (at {cx:g} {snap(cy - 12):g} 0) {eff()})',
        f'    (property "Value" "{value}" (at {cx:g} {snap(cy + 12):g} 0) {eff()})',
        f'    (property "Footprint" "{fp}" (at {cx:g} {cy:g} 0) {eff(hide=True)})',
    ]
    geom = pin_geometry(sym)
    pinlist = "\n".join(f'    (pin "{p}" (uuid "{uid(ref+"/pin/"+p)}"))' for p in geom)
    body.append(f'''  (symbol (lib_id "wigwag:{sym}") (at {cx:g} {cy:g} 0) (unit 1)
    (exclude_from_sim no) (in_bom {"yes" if board else "no"}) (on_board {"yes" if board else "no"}) (dnp {"yes" if dnp else "no"})
    (uuid "{uid(ref)}")
{chr(10).join(props)}
{pinlist}
    (instances (project "{PROJECT}" (path "/{ROOT}" (reference "{ref}") (unit 1))))
  )''')
    # stubs + labels
    for pnum, (dx, dy, side, etype) in geom.items():
        net = nets.get(pnum)
        if net is None:
            continue
        if etype == "no_connect":
            # The pin's own electrical type already declares it unusable, and KiCad treats
            # both a stub and an NC flag on such a pin as errors. Leave it bare.
            skipped_nc += 1
            continue
        px, py = cx + dx, cy - dy
        ex, ey = px + DELTA[side][0], py + DELTA[side][1]
        body.append(f'  (wire (pts (xy {px:g} {py:g}) (xy {ex:g} {ey:g}))\n'
                    f'    (stroke (width 0) (type default)) (uuid "{uid(ref+"/w/"+pnum)}"))')
        if net == "~":
            nc_count += 1
            body.append(f'  (no_connect (at {ex:g} {ey:g}) (uuid "{uid(ref+"/nc/"+pnum)}"))')
        else:
            body.append(f'  (label "{net}" (at {ex:g} {ey:g} 0)\n'
                        f'    {eff(justify=JUST[side])} (uuid "{uid(ref+"/l/"+pnum)}"))')

libs = [def_passive(s) for s in PASSIVES if s in used] + \
       [def_ic(s) for s in ICS if s in used]

# A real symbol library, so lib_id "wigwag:X" resolves instead of raising lib_symbol_issues
# on every part. Same definitions, de-indented and without the library prefix.
def bare(block):
    block = block.replace('(symbol "wigwag:', '(symbol "', 1)
    return "\n".join(l[2:] if l.startswith("  ") else l for l in block.splitlines())

symlib = f'''(kicad_symbol_lib
  (version 20231120)
  (generator "wigwag/gen_schematic.py")
{chr(10).join(bare(b) for b in libs)}
)
'''

out = f'''(kicad_sch
  (version 20231120)
  (generator "wigwag/gen_schematic.py")
  (uuid "{ROOT}")
  (paper "B")
  (title_block
    (title "wigwag — desk stoplight for AI coding sessions")
    (date "2026-08-15")
    (rev "A")
    (comment 1 "Generated from hardware/SCHEMATIC.md by gen_schematic.py — do not hand-edit")
    (comment 2 "Pin map ADR-0023 · stackup/placement ADR-0024 · one B sheet D131")
  )
  (lib_symbols
{chr(10).join(libs)}
  )
{chr(10).join(body)}
  (sheet_instances (path "/" (page "1")))
)
'''

dest = Path(__file__).parent / "kicad"
dest.mkdir(exist_ok=True)
(dest / "wigwag.kicad_sch").write_text(out)
(dest / "wigwag.kicad_sym").write_text(symlib)
(dest / "sym-lib-table").write_text(
    '(sym_lib_table\n  (version 7)\n'
    '  (lib (name "wigwag")(type "KiCad")(uri "${KIPRJMOD}/wigwag.kicad_sym")'
    '(options "")(descr "wigwag project symbols, generated"))\n)\n')
(dest / "wigwag.kicad_pro").write_text('''{
  "board": {"design_settings": {}},
  "meta": {"filename": "wigwag.kicad_pro", "version": 1},
  "schematic": {"legacy_lib_dir": "", "legacy_lib_list": []},
  "sheets": [["%s", "Root"]],
  "text_variables": {}
}
''' % ROOT)

nets = {}
for ref, sym, value, fp, cx, cy, n, dnp, board in P:
    for p, net in n.items():
        if net and net != "~":
            nets.setdefault(net, []).append(f"{ref}.{p}")
print(f"parts={len(P)}  symbols={len(libs)}  nets={len(nets)}  "
      f"nc-flags={nc_count}  nc-type-pins-left-bare={skipped_nc}")
single = {k: v for k, v in nets.items() if len(v) < 2}
if single:
    print("!! single-node nets (would be ERC errors):")
    for k, v in sorted(single.items()):
        print(f"   {k}: {v}")
