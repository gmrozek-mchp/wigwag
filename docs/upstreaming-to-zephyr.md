# Upstreaming to Zephyr mainline

Two mainline bugs were found during Phase 2 (see `JOURNAL.md`, 2026-08-14). Both are fixed locally
in `firmware/patches/` or in the board overlay, and both should go upstream — ADR-0006's ladder
says fix it locally, then contribute it, rather than living on a fork.

This file holds everything needed to submit them, so the work does not have to be re-derived. All
process details below were read from the Zephyr documentation on 2026-08-14, not recalled.

Tree state when the bugs were found: mainline pinned at
`357467a011cd2557a1a3f0b4be83d817c4addc9b` (`main`, 2026-08-14), Zephyr 4.4.99.

## Status

| # | Bug | Issue | PR | State |
|---|---|---|---|---|
| 1 | `microchip,{tc,tcc}-g1-pwm` name the third `pwm-cell` `polarity`, not `flags` | — | — | not submitted |
| 2 | `pic32cm_pl10_cnano` declares LED0 `GPIO_ACTIVE_HIGH`; hardware is active low | — | — | not submitted |

Searched upstream on 2026-08-14: **no existing issue or PR covers either.** Possibly adjacent, open:
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
  Hence two PRs here, not one.

## Routing

Both files fall under the same `MAINTAINERS.yml` area:

```
Microchip PIC32 Platforms:
  maintainers: ArunMCHP, NhMchp
  collaborators: fabin-mchp, Farsin-Nasar-Microchip, mchp-asif,
                 sunil-abraham, AzharMCHP, nandojve
  files: boards/microchip/pic32*/          <- bug 2
         dts/bindings/*/microchip,*-g*     <- bug 1
  labels: "platform: Microchip PIC32"
```

Both are Microchip-authored files (© 2026 Microchip), so there may be an internal review path in
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

## If the binding fix is accepted

`firmware/patches/0001-*.patch` becomes redundant once the pin moves past the merge. At that point:

1. Move the Zephyr revision in `firmware/west.yml` to a commit containing the fix.
2. Delete the patch and its mention in `firmware/patches/README.md`.
3. Confirm `wigwag: polarity flags 0x1` still appears at boot — that line exists precisely so a
   silent regression here cannot hide (D74).
4. Record the old and new SHA in `JOURNAL.md`, per the pin-moving rule in `firmware/README.md`.
