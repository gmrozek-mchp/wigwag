# Local patches to the pinned Zephyr tree

Patches here are applied to `zephyr/` by hand and are **not** preserved by `west update`, which
will silently revert them. Each one exists to fix a mainline bug this project hit, and each is
meant to go upstream — ADR-0006's ladder: fix it locally, then contribute it, rather than living on
a fork.

**Submitting these upstream:** [`docs/upstreaming-to-zephyr.md`](../../docs/upstreaming-to-zephyr.md)
has the verified process, the maintainer routing, and ready-to-use issue and commit text.

Re-apply after any `west update`:

```sh
cd zephyr && git apply ../firmware/patches/*.patch && cd ..
```

Check whether they are currently applied:

```sh
cd zephyr && git diff --stat
```

## 0001 — name the third Microchip PWM cell `flags`, not `polarity`

`dts/bindings/pwm/microchip,{tc,tcc}-g1-pwm.yaml` declare their third `pwm-cells` entry as
`polarity`. Zephyr resolves PWM flags through

```c
#define DT_PWMS_FLAGS_BY_IDX(node_id, idx) DT_PHA_BY_IDX_OR(node_id, pwms, idx, flags, 0)
```

which looks for a cell named **`flags`** and **defaults to 0 when it is absent**. So with these
bindings `PWM_DT_SPEC_GET()` silently discards whatever polarity the devicetree asked for. No
warning, no error: `PWM_POLARITY_INVERTED` in a `pwms` cell simply does nothing.

**52 of the PWM bindings in tree use `channel, period, flags`.** These two are the only outliers,
so this is a naming slip rather than a deliberate interface.

How it was found: the cnano's LED0 is active low, so the D49 lamp needed inverting. Adding
`PWM_POLARITY_INVERTED` changed the generated devicetree — `pwms = <&tcc0 0x2 0x1e8480 0x1>` — but
**pyOCD then reported the freshly built image as byte-identical to the one already flashed**, which
is only possible if the change could not reach the binary. Confirmed by printing
`lamp_yellow.flags` at boot: `0x0` before the patch, `0x1` after.

Worth remembering that the "identical" report was the actual evidence, and was initially dismissed
as a pyOCD quirk.
