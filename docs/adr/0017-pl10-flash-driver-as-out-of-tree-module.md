# ADR-0017 — PL10 flash support is our own driver, shipped as an out-of-tree Zephyr module

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

Three separate things the project wants all turn out to need the same missing capability —
self-programming the PL10's flash:

1. **Credentials out of the image.** D37/D56 put the Wi-Fi passphrase and broker details in a
   gitignored `credentials.conf` compiled in through Kconfig. That works, and it means changing a
   Wi-Fi password requires rebuilding and reflashing over SWD. For a device meant to sit on someone
   else's desk that is not a real answer.
2. **Settings that survive a reboot.** `wigwag/brightness` is retained on the broker, so it comes
   back moments after subscribing — but the lamps run at 255 until it arrives, and per-lamp `gain`
   calibration (D91) is a devicetree constant that cannot be trimmed on a built unit.
3. **A bootloader.** The chosen direction is a stripped-down Adafruit-style serial bootloader
   (`docs/usb-serial-and-bootloader.md`), which is bare-metal rather than Zephyr and so does not need
   this driver — but it needs the same NVMCTRL programming sequence, and getting that sequence right
   once, under a test framework, is worth more than deriving it twice.

The board devicetree already carves out a 4 KB `storage_partition` at `0xf000`, unused since day one,
which is exactly where 1 and 2 belong.

Mainline has two Microchip flash drivers and **neither fits**. `microchip,nvmctrl-g1-flash` names
module `U2409` in its own binding and drives it through a page buffer with the `PBC` and `EB`
commands. PL10's NVMCTRL has no page buffer and neither command: its set is
`FLWR / FLPER / FLMPER2..32 / LR / UR / EBOOTCFG / WBOOTCFG / WLOCKREGION / WROMCFG / CHER`, and
writes go 32 bits at a time straight into the array. Enabling the g1 driver here would issue reserved
commands at a peripheral that never implemented them.

This is the **third** time in one day that a `-g1` driver has turned out to target a different
revision of a peripheral than PL10 carries — after RSTC (ADR-0016, D95) and the TCC bit width (D68).
The naming implies a compatibility that does not exist.

## Decision

**Write the driver, and ship it as a real out-of-tree Zephyr module at
`firmware/modules/pic32cm-pl-nvmctrl/`** — `zephyr/module.yml`, `Kconfig`, `CMakeLists.txt`,
`drivers/`, `dts/bindings/`, registered via `ZEPHYR_EXTRA_MODULES` from the app's `CMakeLists.txt`.

**A module, not a patch to the pinned tree.** ADR-0006's ladder allows patching (rung b) and
`firmware/patches/` already carries one — but that patch also taught us that `west update` reverts
patches *silently*, which cost a debugging session when PWM polarity quietly stopped working. A
module survives `west update`, and upstreaming becomes a file copy rather than a re-derivation.

**Compatible string `microchip,pic32cm-pl-nvmctrl`, named after the family.** Our pack copy carries
no ATDF module id to name a peripheral group after (`version None`), and the family already sets this
precedent upstream: `microchip,pic32cm-pl-clock` in `pic32cm_pl.dtsi`. Inventing a `g2` would assert
a grouping we cannot verify.

**Geometry is read from `PARAM` at runtime**, not encoded in devicetree — `NVMP` pages of
`8 << PSZ` bytes — then cross-checked against the `zephyr,flash` node's size and the driver refuses
to initialise if they disagree. One binding then covers the 64 KB `6408PL` and the 128 KB `1216PL`,
and a `reg` pointing at the wrong peripheral fails loudly instead of erasing the wrong page.

**The controller node is a sibling of `flash0`, not its parent** — because devicetree cannot reparent
an existing node and PL10 declares `flash0` directly under `/soc`. The consequence is that
`DT_MTD_FROM_FIXED_PARTITION()` resolves to `/soc` rather than a device, so `FIXED_PARTITION_DEVICE()`
does not work for this family at all. Consumers pass the device explicitly alongside
`PARTITION_OFFSET()`, which is all NVS needs. Reparenting is part of the upstream PR.

**Erase and write timings are measured and written down**, because on this part they are a hard
CPU-stall blackout — the bus stalls on instruction fetch until the operation completes (§26.4.2.3.1),
which is *why* no RAM-resident routine is needed on an 8 KB part, and also why the stall blocks
interrupts. Measured at 24 MHz: **10.1 ms per page erase, ~0.13 ms per word write**. Against
ADR-0016's 500 ms watchdog budget that caps a single `erase()` call at roughly **49 pages (~24 KB)**;
the storage partition is 8 pages / 81 ms, a sixth of the budget.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **Add an `nvmctrl-g1-flash` node and use the in-tree driver** | The tempting one-line answer, and it would issue `PBC` and `EB` — commands this revision does not define. It would not fail cleanly; it would do something undefined to flash. This is exactly the mistake the RSTC bug made in the other direction. |
| **Patch the pinned Zephyr tree** (ADR-0006 rung b) | Legitimate, and it is what the existing PWM fix does because a binding cannot be overridden any other way. Rejected here because a whole driver is a large patch to carry, and `west update` reverting it silently is a failure mode we have already paid for once. A module has the same upstreaming path with none of that risk. |
| **App-local driver in `firmware/src/`** | Least ceremony, and there is precedent for app-local devicetree bindings (D91's `wigwag,lamps`). Rejected because this one is genuinely generic — it belongs to the SoC, not to wigwag — and burying it in the app would make the upstream contribution a rewrite rather than a move. |
| **Zephyr4Microchip branch** (ADR-0006 rung c) | Might already carry PL flash support. Rejected without checking in depth for the same reason ADR-0006 ranks it third: it trades mainline for a vendor fork across the *whole* tree to solve one driver, and we would still owe mainline the fix. Worth a look before submitting the PR, in case it can be adapted with attribution. |
| **Skip flash entirely; keep credentials compiled in** | Honest for a one-unit bench project, and it is the status quo. Rejected because it blocks all three motivating items at once, and because the driver turned out to be ~300 lines against a well-documented sequence. |
| **Use `FLMPER` multi-page erase from the start** | 8× faster, and measurement confirms it (80.6 ms for 8 single-page erases vs ~10 ms for one `FLMPER8`). Deferred, not rejected: a multi-page erase aborts with `LOCKE` if it straddles the boot/application boundary, and that boundary is wherever the `BOOTPROT` fuse puts it — which a bootloader will move. Correct-and-simple first, with the optimisation and its precondition written down in `erase()`. |

## Consequences

**Accepted costs**
- **748 bytes of flash and 40 bytes of RAM** measured (23 272 → 24 080 B flash, 4 528 → 4 568 B RAM),
  the RAM being the driver's mutex and cached geometry.
- One more out-of-tree component to maintain, and a build that no longer works from a bare
  `west build` on the Zephyr tree alone — the module path must be registered, which
  `firmware/CMakeLists.txt` does.
- `FIXED_PARTITION_DEVICE()` remains unusable on this family until the upstream reparenting lands, so
  consumers carry an explicit device reference.
- Any erase larger than ~49 pages in a single call will reboot the device via the watchdog. Nothing
  does that today; the number is documented in the driver and here so that it stays a decision rather
  than a discovery.

**Benefits**
- Credentials, brightness and calibration can move out of the image and into the 4 KB partition that
  has been sitting there unused.
- The NVMCTRL sequence is now correct, tested on silicon, and commented with the reason each step
  exists — which is the input the bare-metal bootloader needs.
- It is the highest-value upstream contribution the project has: no one has PL flash support, and
  landing it unlocks NVS, `settings`, MCUboot and DFU for the whole PIC32CM PL family.

**Revisit if** the upstream PR lands (then drop the module, enable the in-tree driver, reparent
`flash0` and switch to `FIXED_PARTITION_DEVICE()`), if something needs to erase enough flash to
approach the ~49-page ceiling (implement `FLMPER`, gated on reading `BOOTPROT`), or if a bootloader
takes the bottom of flash (then `LOCKE` becomes an expected answer for low addresses, which
`take_error()` already reports distinctly from `PROGE` for exactly this reason).
