# ADR-0018 — An MCP2221A USB-serial bridge, and one transport at a time chosen from hardware evidence

- **Status:** Accepted, in part superseded
- **Date:** 2026-08-14
- **Superseded for transport selection by ADR-0021, then ADR-0022** — the final answer is a stored
  setting, not inference. The `USBCFG`/`SSPND` pin decision here is also withdrawn (D116). Fitting the
  `MCP2221A` and carrying the console over it stand unchanged.

## Context

Two gaps in the design, discovered from different directions on the same day, have the same cheap
answer.

**The console we already develop against does not survive the move to the product PCB.** The
Curiosity Nano's on-board debugger supplies a CDC console for free, and the whole firmware has been
built around having it: `RESET BY WATCHDOG` (ADR-0016), `NOT FEEDING, lamp task stale 506 ms`, `link
UNLINKED (module silent)`, the resolved PWM flags that exist precisely so a silently-ignored
devicetree value cannot hide (D74). None of that is new capability — it is capability that would
quietly disappear on a board with no debugger attached, leaving SWD as the only route to it. Rule 4
asks the device never to lie; it says nothing about the device being able to *explain* itself, and on
hardware with three lamps and no display that turns out to matter just as much.

**Wi-Fi is mandatory for a device that is usually within arm's reach of the machine driving it.**
ADR-0002 and ADR-0003 build everything on the RNWF02 and MQTT, which is right when the light sits
across the room or aggregates sessions from several machines (D30). But the common case is a lamp on
the same desk as the computer running Claude Code — and that case pays for a Wi-Fi module,
credentials in the image (D37/D56), a broker (ADR-0011), and a provisioning flow (ADR-0012) to move
four states down two feet of desk.

Three facts make the answer cheap rather than a project:

- **ADR-0009 already puts a USB-C connector on the board**, for power: `USB-C 5V → MCP1826 → 3.3V`.
  D+/D− are unconnected. This is not "add USB", it is "connect two pins already present".
- **`MCP2221A` needs almost nothing around it.** Datasheet DS20005565E: it integrates "the USB
  termination resistors and the oscillator needed for USB operation". No crystal, no resistors. It
  enumerates as a composite CDC+HID device using class drivers on Windows, macOS and Linux, so there
  is nothing to install on the host. `MCP2221A-I/ST` (TSSOP-14) had 9 888 in stock on 2026-08-14, AEC-Q100.
- **The 28-pin pin map has to be reworked anyway.** The product target is `PIC32CM6408PL10028`, and
  comparing pinout headers shows the 48-pin cnano mapping does not transfer: `PB02` (lamp WO2) and
  `PB00`/`PB01` (console) **do not exist on the 28-pin package at all**. So adding two pins now is
  nearly free, where adding them after layout is a respin.

The second gap raises a design question the first does not: if the device can be reached two ways,
which one does it believe? D75 is the cautionary tale — link supervision needed three independent
detectors because a single missing piece of positive evidence let the device sit in `READY` reporting
`LINKED` with its module dead.

## Decision

**Fit an `MCP2221A` USB-serial bridge on the product PCB.** `MCP2221A-I/ST`, TSSOP-14, on the
existing USB-C connector's D+/D− with `VUSB` tied to the 3.3 V rail alongside `VDD` — §1.6.2.1 is
explicit that the internal transceiver LDO cannot supply 3.3 V when `VDD` is already 3.3 V, and
getting this wrong degrades signalling rather than failing outright. Populated on the first build.

**Its UART is `SERCOM1`, spending PL10's last SERCOM**, since `SERCOM0` is the module (D76). This is
accepted knowingly: the product will never have I²C. An ambient-light sensor for auto-brightness, the
one plausible future want, can be analogue on an ADC pin.

**`GP2` is configured as `USBCFG` and `GP0` as `SSPND`, wired to MCU inputs.** These are not
decoration: `USBCFG` "indicates when the enumeration is completed", which is *hardware* evidence that
a live USB host is attached, and `SSPND` reports the host suspending. They are what make the next
decision reliable.

**The console goes over this bridge, and costs no firmware.** Zephyr's console is a devicetree and
pinmux assignment, so every existing `printk` reaches a CDC port on the host with no code written.
This alone justifies the part.

**One transport is active at a time, selected at boot from hardware evidence:**

| Condition | Transport |
|---|---|
| `USBCFG` asserted **and** a host heartbeat arrives within a few seconds | USB serial. Wi-Fi bring-up is skipped entirely — no credentials needed. |
| `USBCFG` asserted, no heartbeat | Fall back to Wi-Fi if configured, else fail-visible |
| `USBCFG` deasserted | Wi-Fi / MQTT, exactly as today |

**This works because a charger does not enumerate.** `USBCFG` distinguishes "plugged into a computer"
from "plugged into a wall wart", so the selection is a reading rather than a guess. A device on a
shelf powered by a charger correctly chooses Wi-Fi.

**One firmware image covers both build variants.** The fully-wired variant is "do not populate the
RNWF02", not a separate build.

**`CONTEXT.md`'s vocabulary does not change.** `IDLE`/`BUSY`/`WAIT`/`ERROR` are transport-independent;
only the carrier differs. Whatever protocol the USB path uses must keep `wigwag_state_parse()` as the
single place a state string becomes a state.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **Both transports live simultaneously, USB taking precedence** | My first instinct, and I talked myself out of it. `link.c` would need two concurrent trust evaluations, a precedence rule, and a fail-visible condition reasoning about both — with the rarely-exercised combinations being exactly where bugs hide. D75 was that bug, in a simpler system. Redundancy is worth very little for a status light and ADR-0007's promise is the thing most worth protecting. Revisit only if a real user need appears. |
| **Console and diagnostics only; MQTT stays the sole state path** | Would still fix the field-diagnosability gap for zero architectural cost, and is a strict subset of what is decided here — which is why the *firmware* order is console first. Rejected as the end state because the wired case is likely the common one, and the hardware to serve it costs the same either way. |
| **USB-only product; drop Wi-Fi entirely** | Genuinely tempting: one cable for power and data, no module, no credentials, no broker, no provisioning, a much cheaper BOM. Rejected because it gives up the light being anywhere but beside the machine, and multi-machine aggregation (D30) with it — and because it would supersede ADR-0002 and ADR-0003 on a hunch about usage rather than evidence. The chosen design keeps that variant available as a build option, which is a better way to find out. |
| **Footprint but do-not-populate** | The conservative version, keeping the 10 mA and the last-SERCOM commitment reversible, with SERCOM1 also on test pads. Rejected because the console benefit only exists if the part is fitted, and that benefit is the strongest argument for the part. Test pads on SERCOM1 remain worth having regardless. |
| **Native USB on a bigger MCU instead of a bridge** | Would give real UF2 drag-and-drop firmware update as a bonus. Rejected as out of scope: it reopens ADR-0008 and ADR-0001, and PL10 has no USB peripheral at all (no `usb.h` in the pack). Noted as the escape hatch if USB device-side features ever become requirements. |
| **`MCP2200` or a generic CH340/FT232** | Cheaper or more familiar. `MCP2200` caps at a lower rate and lacks the GPIO alternates; the point of the `MCP2221A` here is `USBCFG`/`SSPND`, which are what make transport selection evidence-based rather than heuristic. Staying on Microchip silicon also keeps the whole BOM sourceable from one vendor. |
| **`MCP2221` (non-A)** | Identical except the UART caps at 115 200 instead of 460 800. No reason to choose it. |

## Consequences

**Accepted costs**
- **~10 mA typ / 12 mA max always on** (§4.1 D004 at 3.0 V; 46 µA standby). Nothing to the 1 A
  MCP1826, but it roughly doubles quiescent draw and would matter to any future battery variant.
- **No I²C on the product, ever.** PL10 has exactly two SERCOMs.
- One more part, two caps, a short D+/D− pair, and two more MCU pins — taking the budget to roughly
  19 of 28, still comfortable.
- **The MCU cannot tell when the host opens the port**: the MCP2221A exposes `URx`/`UTx` only, no
  DTR/RTS. Liveness must stay application-level, in the same shape as `wigwag/host_online` (D75).
  `USBCFG` proves a host exists, not that anything is listening.
- A daemon serial backend is implied eventually. `termios` is stdlib on macOS and Linux, but Windows
  would want `pyserial`, and ADR-0010 makes the host cross-platform — a new runtime dependency needs
  its own ADR per the conventions.
- Default VID/PID means two wigwags on one machine are not distinguishable without programming a
  serial number into each (a one-time step with Microchip's configuration utility). Acceptable now;
  a manufacturing consideration later.

**Benefits**
- The console the firmware was developed against continues to exist on the product, with no probe
  attached and no firmware written. Continuity rather than a new feature — but losing it would have
  been a real regression, and one easy not to notice until a field unit misbehaved.
- Two more liveness signals, both hardware-grounded, available to `link.c` — better evidence than
  anything in the MQTT chain.
- A wired variant needs no credentials, no broker, no provisioning and no Wi-Fi module, which removes
  the entire class of setup failure for the most common deployment.
- Transport selection is a reading of the world rather than a configuration item, so there is nothing
  for a user to get wrong.

**Revisit if** a real need for simultaneous transports appears (then the peers design is pre-analysed
above), if quiescent current becomes a constraint, if something genuinely needs I²C, or if the USB-only
variant proves to be what everyone actually builds — in which case ADR-0002 and ADR-0003 should be
revisited deliberately rather than by drift.
