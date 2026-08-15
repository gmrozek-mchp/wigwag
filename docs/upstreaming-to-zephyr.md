# Upstreaming to Zephyr mainline

Five mainline issues were found during Phase 2 (see `JOURNAL.md`, 2026-08-14). Each is worked around
locally — in `firmware/patches/` or in the board overlay — and each should go upstream, per ADR-0006's
ladder: fix it locally, then contribute it, rather than living on a fork.

This file holds everything needed to submit them, so the work does not have to be re-derived. All
process details below were read from the Zephyr documentation on 2026-08-14, not recalled.

**A caution earned the hard way.** Bug 3 went through three drafts. The first claimed two pins lost
and a one-second boot delay, both from reading code and datasheet prose. Measurement killed the boot
delay outright. A datasheet section then argued the pin loss was only one — so the report was narrowed
— and a controlled A/B on hardware showed the original two-pin claim had been right all along, with
the datasheet section simply not matching the silicon. Measure before asserting; where the datasheet
and the board disagree, believe the board, and say which one you tested.

Tree state when the bugs were found: mainline pinned at
`357467a011cd2557a1a3f0b4be83d817c4addc9b` (`main`, 2026-08-14), Zephyr 4.4.99.

## Status

| # | Bug | Issue | PR | State |
|---|---|---|---|---|
| 1 | `microchip,{tc,tcc}-g1-pwm` name the third `pwm-cell` `polarity`, not `flags` | — | — | not submitted |
| 2 | `pic32cm_pl10_cnano` declares LED0 `GPIO_ACTIVE_HIGH`; hardware is active low | — | — | not submitted |
| 3 | `pic32cm_pl10_cnano` enables `XOSC32K` (crystal disconnected by default), silently taking PA24 **and PA25** | — | — | not submitted |
| 4 | `hwinfo_mchp_g1.c` reads PIC32CM PL's `RCAUSE` at the wrong offset with JH's bit positions | — | — | not submitted |
| 5 | PIC32CM PL declares `flash0` directly under `/soc`, so `FIXED_PARTITION_DEVICE()` cannot resolve for the whole family | — | — | not submitted |

Searched upstream on 2026-08-14: **no existing issue or PR covers any of them.** Possibly adjacent, open:
[#107066](https://github.com/zephyrproject-rtos/zephyr/issues/107066) "Unexpected DT binding
inference for specific properties (pwms)".

## Process

From [Contribution Guidelines](https://docs.zephyrproject.org/latest/contribute/guidelines.html)
and [Contributor Expectations](https://docs.zephyrproject.org/latest/contribute/contributor_expectations.html):

1. **File a GitHub issue first.** The guidelines direct you to do this before starting work on a
   bug, and it gives the commit a `Fixes #N` to close.
2. Fork `zephyrproject-rtos/zephyr`, clone, rename `origin` → `upstream`, add your fork as
   `origin`, and branch off `main`.
3. Commit with **`git commit -s`**. DCO sign-off is mandatory:
   `Signed-off-by: Your Name <your.email@example.com>`, using your **legal name** — "pseudonyms,
   hacker handles, and the names of groups are not allowed" — and an email matching the commit
   author.
4. Push to your fork and open the PR in the web UI.
5. Address review by **interactive rebase**. No fixup or merge commits.

Commit message rules that bite:

- Subject is `[area]: [summary of change]`, **one line, under 72 characters**, then a completely
  blank line.
- **"An empty commit message body is not permitted. Even for trivial changes, please include a
  descriptive commit message body."** Both fixes below are one-liners; both still need a body.
- Body lines ~75 characters or less; explain what, why, assumptions, and how it was verified.
- `Fixes #1234` auto-closes on merge.
- Prefer **small, self-contained PRs** — "reviewed more quickly and reviewed more thoroughly".
  Hence three PRs here, not one.

## Routing

All four touch Microchip areas; bug 4 is in a driver rather than a board or binding:

```
Microchip PIC32 Platforms:
  maintainers: ArunMCHP, NhMchp
  collaborators: fabin-mchp, Farsin-Nasar-Microchip, mchp-asif,
                 sunil-abraham, AzharMCHP, nandojve
  files: boards/microchip/pic32*/          <- bugs 2 and 3
         dts/bindings/*/microchip,*-g*     <- bug 1
  labels: "platform: Microchip PIC32"
```

Bug 4 is in `drivers/hwinfo/hwinfo_mchp_g1.c`, which `MAINTAINERS.yml` places under the same
Microchip PIC32 platform area by copyright and by the `mchp` filename, so the routing is the same.
The `Drivers: HWINFO` area maintainers should be added as reviewers as well.

All are Microchip-authored files (© 2026 Microchip), so there may be an internal review path in
parallel with the upstream PR.

---

## Bug 1 — the third `pwm-cell` is named `polarity` instead of `flags`

**The important one**, because it fails silently.

### Evidence

Zephyr resolves PWM flags through `include/zephyr/devicetree/pwms.h`:

```c
#define DT_PWMS_FLAGS_BY_IDX(node_id, idx) \
        DT_PHA_BY_IDX_OR(node_id, pwms, idx, flags, 0)
```

It looks for a cell named **`flags`** and **defaults to 0 when absent**. Both
`dts/bindings/pwm/microchip,tc-g1-pwm.yaml` and `microchip,tcc-g1-pwm.yaml` declare:

```yaml
pwm-cells:
  - channel
  - period
  - polarity        # <- should be: flags
```

So `PWM_DT_SPEC_GET()` discards whatever polarity the devicetree asked for. No warning, no error.

- **52 of the in-tree PWM bindings** use `channel, period, flags`. These two are the only outliers.
- **The rename is safe.** `pwm-cells` are positional in devicetree, so no `.dts` needs changing, and
  nothing in tree reads the cell by name. The four in-tree users
  (`sam_e54_xpro`, `pic32cx_sg41_cult`, `pic32cx_sg61_cult`, `pic32cm_jh01_cpro`) are unaffected.
- The one theoretical breakage is out-of-tree code calling `DT_PWMS_CELL(node, polarity)`. Say so in
  the PR rather than letting a reviewer find it.

### How it was found (useful in the issue)

Adding `PWM_POLARITY_INVERTED` to a `pwms` cell changed the generated devicetree —
`pwms = <&tcc0 0x2 0x1e8480 0x1>` — but produced a **byte-identical binary**, which pyOCD reported
when flashing. Printing the resolved `pwm_dt_spec.flags` at boot gave `0x0` before the binding fix
and `0x1` after.

### Issue text

> **Title:** `microchip,{tc,tcc}-g1-pwm` bindings silently discard PWM polarity
>
> The `microchip,tc-g1-pwm` and `microchip,tcc-g1-pwm` bindings name their third `pwm-cells` entry
> `polarity`. `DT_PWMS_FLAGS_BY_IDX()` expands to
> `DT_PHA_BY_IDX_OR(node_id, pwms, idx, flags, 0)`, which looks for a cell named `flags` and
> defaults to `0` when it is absent — so `PWM_DT_SPEC_GET()` silently drops the polarity the
> devicetree requested. Specifying `PWM_POLARITY_INVERTED` has no effect, and there is no warning.
>
> 52 of the in-tree PWM bindings use `channel, period, flags`; these two are the only outliers.
>
> Reproduced on a PIC32CM PL10 Curiosity Nano (`pic32cm_pl10_cnano`) driving an LED from TCC0 WO2
> via an application overlay. The resolved `pwm_dt_spec.flags` reads `0x0` with
> `PWM_POLARITY_INVERTED` set in devicetree; renaming the binding cell to `flags` makes it read
> `0x1` and the output invert as requested.
>
> Secondary observation, possibly worth its own fix: because `DT_PHA_BY_IDX_OR` substitutes a
> default, *any* binding that misnames this cell fails quietly. A compliance check asserting that a
> three-entry `pwm-cells` names its third entry `flags` would catch the whole class at CI time.

### Commit message

```
dts: bindings: pwm: microchip: name the third pwm-cell "flags"

The microchip,tc-g1-pwm and microchip,tcc-g1-pwm bindings declare their
third pwm-cells entry as "polarity". Zephyr resolves PWM flags with

  #define DT_PWMS_FLAGS_BY_IDX(node_id, idx) \
          DT_PHA_BY_IDX_OR(node_id, pwms, idx, flags, 0)

which looks for a cell named "flags" and defaults to 0 when it is absent.
With these bindings PWM_DT_SPEC_GET() therefore discards the polarity
requested by the devicetree, with no warning and no error: specifying
PWM_POLARITY_INVERTED in a pwms cell has no effect at all.

52 of the PWM bindings in tree use "channel, period, flags"; these two are
the only outliers, so this is a naming slip rather than a deliberate
interface.

The rename is safe for existing users: pwm-cells are positional in
devicetree, so no .dts changes are needed, and no in-tree code reads the
cell by name. Out-of-tree code using DT_PWMS_CELL(node, polarity) would
need to switch to the standard flags accessor.

Verified on a PIC32CM PL10 Curiosity Nano driving an LED from TCC0 WO2:
the resolved pwm_dt_spec flags read 0x0 before this change and 0x1 after,
and the inverted output then matched the board's active-low LED.

Fixes #<issue>

Signed-off-by: Your Name <you@example.com>
```

The local fix is `firmware/patches/0001-dts-bindings-pwm-microchip-name-the-third-pwm-cell-flags.patch`.

---

## Bug 2 — `pic32cm_pl10_cnano` LED0 polarity

### Evidence

- PIC32CM PL10 Curiosity Nano user guide, **Table 4-1**: "User LED (yellow), **active low**".
- Same guide **§4.1**: "Driving the connected I/O line to GND will activate the LED."
- `boards/microchip/pic32c/pic32cm_pl10_cnano/pic32cm_pl10_cnano.dts` declares
  `gpios = <&portb 2 GPIO_ACTIVE_HIGH>`.

Empirically: with a cubic gamma ramp and non-inverted PWM, the lamp sat lit with a brief dark dip —
the inverse of the intended breath. Inverting made it fade evenly.

**Before submitting, check the board schematic (user guide §8.1).** The claim rests on the user
guide text plus one observation, and a maintainer will reasonably want the schematic cited.

Why it survived: blinky toggles a *logical* state and looks identical either way, as does any
symmetric fade. It only shows up when brightness is meant to track a value.

### Commit message

```
boards: microchip: pic32cm_pl10_cnano: fix LED0 polarity

LED0 on the PIC32CM PL10 Curiosity Nano is active low. The board user
guide lists it as "active low" and states that driving the connected I/O
line to GND activates the LED, but the devicetree declares led0 as
GPIO_ACTIVE_HIGH.

The error is invisible to the usual tests: blinky toggles the logical
state and looks identical either way, as does any symmetric PWM fade. It
shows up as soon as brightness is meant to track a value - a gamma-
corrected ramp renders inverted.

Signed-off-by: Your Name <you@example.com>
```

Note this fix is **not** carried in `firmware/patches/`: wigwag drives PB02 from TCC0 rather than
GPIO, so the board's `led0` node is disabled in our overlay and the wrong flag does not affect us.
It is still wrong for everyone else.

---

---

## Bug 3 — `pic32cm_pl10_cnano` enables XOSC32K, but its crystal is disconnected by default

Found the hard way: two lamps on TCC0 outputs simply never lit, with no error anywhere.

### Evidence

`boards/microchip/pic32c/pic32cm_pl10_cnano/pic32cm_pl10_cnano.dts` enables the external 32.768 kHz
oscillator, with clock-failure detection:

```
xosc32k: xosc32k {
        compatible = "microchip,pic32cm-pl-xosc32k";
        xosc32k-cfd-en = <1>;
        xosc32k-startup-time = "16K";
        xosc32k-en = <1>;
};
```

But the board leaves that crystal **unconnected**. Its user guide is explicit: *"The crystal is not
connected to the target MCU by default, as the GPIO pins are routed to the edge connector"*, and
connecting it means cutting straps J107/J108 and soldering J109/J110.

**Consequence: PA24 and PA25 both stop working for anything else, silently.**

Established by a controlled A/B on hardware — identical firmware, identical wiring, one devicetree
property changed — with lamps on TCC0 WO0 (PA24) and WO1 (PA25), driven to full brightness by a
power-on lamp test:

| | `xosc32k-en = <1>` | `xosc32k-en = <0>` |
|---|---|---|
| PA24 (WO0) | no output | works |
| PA25 (WO1) | no output | works |

The pinctrl assignment is accepted and then ignored, with no diagnostic anywhere, because the
override happens inside the oscillator rather than in PORT.

Datasheet §13.5.1 states this unconditionally: *"The XTAL32K1 and XTAL32K2 pins are automatically
configured when the XOSC32K oscillator is enabled."*

**A datasheet discrepancy worth reporting alongside it.** §13.4.2.2 says the override is
mode-dependent: *"In External Clock (EXTCLK) mode, only the XTAL32K1 pin is overridden and controlled
by OSC32KCTRL, while the XTAL32K2 pin may be used as a GPIO pin."* This board is in EXTCLK mode —
`xosc32k-xtal-en` defaults to 0 and the board does not set it, confirmed in the generated devicetree
as `..._xosc32k_xtal_en 0`, so XTALEN reaches the register as 0 — and yet **PA25 is overridden too**.
Either §13.4.2.2 does not describe this silicon or the hardware behaves unconditionally as §13.5.1
says. That is a question for Microchip rather than for Zephyr, but it is worth stating in the issue so
nobody else trusts the mode-dependent sentence.

Nothing in the board configuration consumes XOSC32K — `gclkgen0` is sourced from `oschf` — so the
oscillator is enabled, unusable, and unused. The configuration is also internally inconsistent: the
board sets `xosc32k-startup-time = "16K"` while leaving `xosc32k-xtal-en` at 0, and §13.6.7 says the
start-up time is disregarded when XTALEN is 0.

**Measured, and it is *not* a boot-time problem.** The device reaches `main()` **5 ms** after reset
either way. The driver does spin on `XOSC32KRDY` via
`WAIT_FOR(..., TIMEOUT_XOSC32KCTRL_RDY, NULL)`, with the constant at `1000000` and `WAIT_FOR`
documented in microseconds, which reads like a one-second stall — it is not. Keep any boot-delay claim
out of the report.

(An earlier attempt to measure this by timing reset to first console byte gave ~1.6 s, which is
pyOCD's own startup and SWD connect, not the device. Timing a target through a debugger measures the
debugger.)

### Suggested fix

Set `xosc32k-en = <0>` (and drop `xosc32k-cfd-en`) in the board devicetree, with a comment pointing at
the straps for users who solder the crystal on. A devicetree should describe the board as shipped, and
as shipped this crystal is not attached.

### Before submitting

- Already confirmed by the A/B above, so no further bench work is needed for the Zephyr fix itself.
- **Raise the §13.4.2.2 discrepancy with Microchip separately.** The Zephyr PR does not depend on it,
  but a datasheet that says XTAL32K2 stays usable in EXTCLK mode, on a part where it does not, will
  cost someone else the same afternoon.
- Check the sibling PIC32CM/PIC32CK boards for the same pattern — if `pic32cm_jh01_cpro` and friends
  enable XOSC32K with equally disconnected crystals, this is one fix across several boards rather
  than one board's slip.

### Commit message

```
boards: microchip: pic32cm_pl10_cnano: do not enable XOSC32K by default

The board devicetree enables the external 32.768 kHz oscillator, but the
board leaves its crystal disconnected: the user guide states the crystal is
not connected to the target MCU by default because the GPIO lines are routed
to the edge connector, and attaching it requires cutting straps J107/J108.

Enabling it silently costs PA24 and PA25. Both are automatically configured by
OSC32KCTRL whenever XOSC32K is enabled (datasheet 13.5.1), so a pinctrl
assignment to either is accepted and then ignored, with no diagnostic, because
the override happens in the oscillator rather than in PORT.

Confirmed by a controlled comparison on hardware: with lamps on TCC0 WO0 (PA24)
and WO1 (PA25) driven to full brightness, neither produces output with
xosc32k-en = 1, and both work with it set to 0. Identical firmware and wiring
otherwise.

Note this happens in External Clock mode - xosc32k-xtal-en defaults to 0 and
the board does not set it - even though 13.4.2.2 states that in that mode only
XTAL32K1 is overridden and XTAL32K2 remains available as a GPIO. The observed
behaviour follows 13.5.1 instead.

The configuration is also internally inconsistent: xosc32k-startup-time is set
to "16K" while xosc32k-xtal-en is 0, and 13.6.7 states the start-up time is
disregarded when XTALEN is 0.

Nothing in the board configuration uses XOSC32K; gclkgen0 is sourced from
oschf. Disable it by default and leave a pointer to the straps for users who
fit the crystal.

Found while driving three TCC0 PWM outputs on this board, two of which land on
PA24 and PA25 and produced nothing.

Signed-off-by: Your Name <you@example.com>
```

This one is **not** carried in `firmware/patches/` — wigwag disables XOSC32K in its own board overlay
(D82), which is the right place for an application-specific clock choice regardless of what upstream
does.

## If the binding fix is accepted

`firmware/patches/0001-*.patch` becomes redundant once the pin moves past the merge. At that point:

1. Move the Zephyr revision in `firmware/west.yml` to a commit containing the fix.
2. Delete the patch and its mention in `firmware/patches/README.md`.
3. Confirm `wigwag: polarity flags 0x1` still appears at boot — that line exists precisely so a
   silent regression here cannot hide (D74).
4. Record the old and new SHA in `JOURNAL.md`, per the pin-moving rule in `firmware/README.md`.

---

## Bug 4 — `hwinfo_mchp_g1.c` misreads `RCAUSE` on PIC32CM PL

**The worst of the four**, because it does not fail — it *answers*, and the answer is wrong. Bug 1 is
silent; this one is confidently incorrect, which is harder to catch and worse to trust. A device
asking "did the watchdog just reboot me?" gets "no, someone pressed reset."

### Evidence

The driver takes RSTC's devicetree `reg` address and reads a byte from offset 0, then decodes bits
using `enum rstc_g1_rcause` from `include/zephyr/drivers/reset/mchp_rstc_g1.h`:

```c
volatile uint8_t *rcause_reg = (uint8_t *)(DT_REG_ADDR(RSTC_INST));
uint8_t rcause = *rcause_reg;
...
if ((rcause & BIT(RSTC_G1_RCAUSE_WDT)) != 0) {	/* RSTC_G1_RCAUSE_WDT == 5 */
	result |= RESET_WATCHDOG;
}
```

That matches the JH revision of the peripheral exactly, and matches PL's not at all:

| | PIC32CM JH (`pic32cm_jh00/…/rstc.h`) | PIC32CM PL (`pic32cm_pl10/…/rstc.h`) |
|---|---|---|
| `RCAUSE` offset | `0x00` | **`0x04`** — `0x00` is `CTRLA` |
| `RCAUSE` width | 8-bit (`__I uint8_t`) | **32-bit** (`__I uint32_t`) |
| register mask | `0x77` | **`0x7B`** |
| POR | 0 | 0 |
| brownout | `BODCORE` 1, `BODVDD` 2 | **`BORVDD` 1 only** |
| `EXT` | 4 | **3** |
| `WDT` | 5 | **4** |
| `SYST` | 6 | **5** |
| bit 6 | — | **`LOCKUP`** |
| bit 7 (`BACKUP` in the enum) | — | — (reserved on both) |

Confirmed against the PL10 datasheet §16.6.2 register diagram, which also states each reset "sets
the bit corresponding to the Reset source to 1 and all other bits are written to 0" — so `RCAUSE`
needs no clearing, and the absent `z_impl_hwinfo_clear_reset_cause()` is correct behaviour, not an
omission.

Two consequences, both observed:

1. The read lands on `CTRLA`, which reads back 0 in normal operation, so **every** reset cause comes
   back as "none" — `hwinfo_get_reset_cause()` returns 0 with a success status.
2. Even at the right address the mapping is off by one bit from `EXT` upward, so a watchdog reset
   (PL bit 4) would decode as `RESET_PIN | RESET_USER`, and a CPU lockup (PL bit 6) as
   `RESET_SOFTWARE`.

### How it was found (useful in the issue)

A deliberately wedged thread was used to make a real watchdog reset happen on a
`pic32cm_pl10_cnano`. The reset fired exactly on schedule, and the boot that followed reported no
cause at all. Reading `0x40000c04` directly returned `0x10` — bit 4, `WDT` — which is what the
datasheet predicts and what the driver never looks at.

### Suggested fix

The two revisions are different enough that a per-family compile-time split is needed, in the shape
`mchp_rstc_g1.h` already uses for `RSTC_UNSUPPORTED_RCAUSE`:

- keep the offset in devicetree rather than assuming 0 — either a second `reg` entry for `RCAUSE` or
  a `#define` per family;
- select the bit positions on `CONFIG_SOC_FAMILY_MICROCHIP_PIC32CM_PL` (POR 0, BORVDD 1, EXT 3, WDT
  4, SYST 5, LOCKUP 6) versus the existing JH/SAM set;
- map PL's `LOCKUP` to `RESET_CPU_LOCKUP`, which the current code never reports for any family;
- correct `z_impl_hwinfo_get_supported_reset_cause()`, which currently claims
  `RESET_LOW_POWER_WAKE` and `RESET_BROWNOUT` unconditionally.

Note also that `wdt_mchp_g1.c` line 23 defines `WDT_FLAG_ONLY_ONE_TIMEOUT_VALUE_SUPPORTED` from
`DT_PROP(DT_NODELABEL(wdog), …)` — node label `wdog`, which exists in no in-tree devicetree; the
label everywhere is `wdt`. It compiles only because the macro is never expanded: the `#if
defined(...)` guarding its use tests an object-like macro that is *always* defined, so the block is
compiled unconditionally and the flag it was meant to gate on is never consulted. Worth folding into
the same issue as a drive-by.

### Commit message

```
drivers: hwinfo: mchp: fix RCAUSE access on PIC32CM PL

The driver read RCAUSE as an 8-bit register at offset 0 with the bit
positions used by PIC32CM JH.  On PIC32CM PL, RCAUSE is a 32-bit
register at offset 0x04 -- offset 0x00 is CTRLA -- and the flags from
EXT upward sit one bit lower, with LOCKUP occupying bit 6.

The result was not a failure but a wrong answer: reads landed on CTRLA
and returned 0, so hwinfo_get_reset_cause() reported no cause for any
reset, and at the correct address a watchdog reset would have decoded
as RESET_PIN.

Verified on pic32cm_pl10_cnano by forcing a watchdog reset with a
wedged thread: RCAUSE reads 0x10 (bit 4, WDT), matching the PL10
datasheet section 16.6.2, and hwinfo now reports RESET_WATCHDOG.

Fixes #NNNN

Signed-off-by: Your Name <you@example.com>
```

This one is **not** carried in `firmware/patches/`: wigwag reads `RCAUSE` itself in
`firmware/src/wdog_wdt.c` and leaves the `rstc` devicetree node disabled, so that no driver binds and
answers wrongly. When the fix lands, enable that node, set `CONFIG_HWINFO=y`, and replace the direct
read with `hwinfo_get_reset_cause()`.

---

## Bug 5 — `flash0` has no controller parent, so partitions cannot resolve to a device

### Evidence

`dts/arm/microchip/pic32c/pic32cm_pl/common/pic32cm_6408_pl.dtsi` declares the flash as a direct
child of `/soc`:

```
soc {
	flash0: flash@c000000 {
		reg = <0x0c000000 DT_SIZE_K(64)>;
		ranges = <0x0 0x0c000000 DT_SIZE_K(64)>;
	};
```

Every other Zephyr platform makes the `soc-nv-flash` node a **child of its flash controller** —
`st,stm32-flash-controller`, `atmel,sam0-nvmctrl`, `microchip,nvmctrl-g1-flash` and so on — because
`DT_MTD_FROM_FIXED_PARTITION()` walks *upward* from the partition:

```c
#define DT_MTD_FROM_FIXED_PARTITION(node_id)                                  	COND_CODE_1(DT_NODE_EXISTS(DT_MEM_FROM_FIXED_PARTITION(node_id)),      		    (DT_PARENT(DT_MEM_FROM_FIXED_PARTITION(node_id))),         		    (DT_GPARENT(node_id)))
```

With `flash0` under `/soc`, that resolves to `/soc` — not a device. So `FIXED_PARTITION_DEVICE()` and
everything built on it cannot work on this family, even though `pic32cm_pl10_cnano.dts` defines a
`slot0_partition` and a `storage_partition` and sets `zephyr,code-partition`. The partitions look
complete and are unusable for their main purpose.

`PARTITION_OFFSET()` and `PARTITION_SIZE()` are unaffected, being pure devicetree arithmetic, so the
workaround is for consumers to pass the flash device explicitly. That is what wigwag does (ADR-0017).

### How it was found (useful in the issue)

Writing a flash driver for the family (there is none — see the contribution note below) and finding
there was no correct way to hand NVS a device for `storage_partition`. An overlay cannot fix it:
devicetree has no way to reparent an existing node.

### Suggested fix

Introduce the controller node in `pic32cm_pl.dtsi` and move `flash0` inside it, matching the shape
used by `samd5xe5x.dtsi`:

```
nvmctrl: nvmctrl@41004000 {
	compatible = "microchip,pic32cm-pl-nvmctrl";
	reg = <0x41004000 0x30>;
	#address-cells = <1>;
	#size-cells = <1>;
	ranges = <0x0 0x0c000000 DT_SIZE_K(64)>;
	status = "disabled";

	flash0: flash@c000000 { ... };
};
```

This is a devicetree-structure change with no behavioural effect on existing users — nothing in tree
binds a PL flash controller today, precisely because there is no driver — so it is safe to land ahead
of the driver.

### Commit message

```
dts: microchip: pic32cm_pl: put flash0 under a controller node

The soc-nv-flash node was a direct child of /soc, so
DT_MTD_FROM_FIXED_PARTITION() resolved to /soc rather than to a flash
device, and FIXED_PARTITION_DEVICE() could not be used for any
partition on this family -- including the slot0 and storage
partitions that pic32cm_pl10_cnano.dts already defines.

Add the NVMCTRL controller node and move flash0 inside it, matching
every other platform's structure.  No functional change for existing
users: no in-tree driver binds a PIC32CM PL flash controller yet.

Fixes #NNNN

Signed-off-by: Your Name <you@example.com>
```

---

## Contribution — a flash driver for PIC32CM PL

Not a bug: mainline has no flash driver for this family at all, and the two Microchip drivers it does
have target different peripheral revisions. `microchip,nvmctrl-g1-flash` names module `U2409` in its
own binding and drives it through a page buffer using the `PBC` and `EB` commands; PL10's NVMCTRL has
no page buffer and neither command.

`firmware/modules/pic32cm-pl-nvmctrl/` is deliberately shaped as an upstreamable module — driver,
binding and Kconfig in in-tree layout — so submitting it is a file move plus the devicetree change in
bug 5 above. It is written from the datasheet's own sequence (§26.4.2.3.4), verified on a
`pic32cm_pl10_cnano`, and documents the trap that a naive implementation falls into: issuing an
*enable* command such as `FLWR` or `FLPER` is itself a command that clears `INTFLAG.READY`, and
storing to the array before it completes sets `STATUS.PROGE` while silently doing nothing.

Send it after bug 5, and after the PL10 devicetree node additions below, since it depends on both.

## Missing PL10 devicetree nodes — a contribution, not a bug

Separately from the five bugs, this project has now had to create **seven** devicetree nodes that PL10
lacks entirely, in each case with the driver and binding already in mainline and sibling families
already instantiating them: `tcc0`, `sercom0`'s UART configuration, its two `gclkperiph` channels,
`wdt`, `rstc`, and `nvmctrl` — the last of which needed a driver written as well, since no in-tree
driver matches this peripheral revision. These are additive SoC-level enablement rather than fixes, and belong in
`dts/arm/microchip/pic32c/pic32cm_pl/common/pic32cm_pl.dtsi` with `status = "disabled"`, following
the JH dtsi's shape. `firmware/boards/pic32cm_pl10_cnano.overlay` holds all of them, with every
address, IRQ number and limit cited to its source — that overlay is the raw material for the PR.

Do not send this until the watchdog and PWM work is settled on hardware, and send it as one PR per
peripheral.
