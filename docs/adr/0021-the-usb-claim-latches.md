# ADR-0021 — The USB claim latches until reset, and `USBCFG` is not used

- **Status:** Accepted
- **Date:** 2026-08-15
- **Supersedes:** ADR-0018's transport-*selection* mechanics and its `USBCFG`/`SSPND` pin decision.
  ADR-0018's decision to fit the `MCP2221A` and carry the console over it stands unchanged.

## Context

ADR-0018 decided one transport at a time, chosen from evidence, and implemented it (D104, D112) as a
*soft hold*: a live USB host outranked Wi-Fi, a quiet one went untrusted after 10 s, and after a
further 5 s release window the device handed back to Wi-Fi. `USBCFG` shortened that window when the
bridge saw enumeration drop.

That design treated the two transports as interchangeable carriers of one truth. **They are not.** The
daemon aggregates the sessions of *its own machine* (D30), so the MQTT publisher and the serial
publisher are potentially different computers reporting different work. Falling back from USB to Wi-Fi
therefore does not restore the display — it silently starts answering a different question, swapping
"what is my laptop doing" for "what is some other machine doing", with nothing on a three-lamp device
to mark the substitution. That is confidently displaying the wrong thing, which is the failure ADR-0007
exists to prevent. Amber, meaning "I do not know what your laptop is doing", is the more honest answer.

Two facts settle the objections that would normally weigh against latching:

- **The device is USB-powered** (ADR-0009). Pulling the cable is a power cycle, so the obvious physical
  gesture already clears any latch. There is no need to teach a recovery procedure.
- **A crash-looping daemon can no longer ping-pong the device** between two sources, which the soft
  hold made possible every 15 s.

Separately, examining `USBCFG` properly showed it buys less than ADR-0018 claimed. It cannot detect an
unplugged cable, because that is a power-off. It cannot detect a sleeping host, because suspend does not
*unconfigure* a device — only `SSPND` sees that, and `SSPND` is not GP0's factory default (`LED_URx`
is), so it would need per-unit chip-settings programming. Its one real case was accelerating the release
window, and with a latch there is no release to accelerate.

## Decision

**Once a host has spoken over the wire, the device belongs to USB for the rest of the boot.** The whole
rule:

```c
if (t->host_seen) {                        /* latched for this boot */
        t->active = TRANSPORT_USB;
        t->trusted = host_fresh(t, now_ms);
        return;
}
select_wifi(t, now_ms);
```

**Trust still comes and goes; the choice does not.** A host silent for more than
`TRANSPORT_HOST_TTL_MS` (10 s) makes the lamps fail-visible, and a returning host restores trust
immediately. What never happens is a switch back to Wi-Fi.

**Escaping the latch requires a reset** — a power cycle, the `reboot` command, or a watchdog reset,
because it is per-boot state. Unplugging the cable is a power cycle.

**An orderly `host off` drops trust without releasing the latch.** "I am going away" says nothing about
whether the Wi-Fi source reports the same machine's work, so releasing would be the same silent
substitution by a politer route. What `host off` buys is *immediate* loss of trust rather than waiting
out the 10 s timeout.

**Module service stops entirely once latched.** Both halves — no `rnwf_uart_poll()` and no
`rnwf_at_tick()`. Not merely to save the retries an end-to-end session showed accumulating (947 AT
timeouts), but because draining the UART without ticking would keep delivering MQTT states to the
lamps, which is exactly the substitution the latch exists to prevent. The display must follow the
transport that owns it. The module may remain associated and connected; nothing reads it, and that is
the point.

**`USBCFG` and `SSPND` are not wired to the MCU.** Received bytes are both necessary and sufficient.
`TRANSPORT_RELEASE_MS` and the `usb_cfg` input are removed rather than left as dead configuration.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **The soft hold from ADR-0018** (10 s untrusted, then hand back to Wi-Fi) | What this supersedes. Its flaw is that it treats a *subject* change as a *route* change: after the window it shows another machine's work as though nothing happened. It also lets a crash-looping daemon ping-pong the device every 15 s. |
| **Latch, but release on `host off`** | Reads well — a clean shutdown as an explicit handover — and reintroduces the substitution on the orderly path. A daemon shutting down knows it is leaving; it does not know that the broker's publisher is the same machine. |
| **Release only if Wi-Fi was trusted before USB claimed** | Uses "Wi-Fi was demonstrably live" as evidence of intent. Rejected as conditional state that still cannot establish the thing that matters: whether it is the *same machine*. |
| **Identify the host so equivalence is knowable** | The honest fix for the underlying ambiguity: have the daemon send an identity and carry the same one in MQTT payloads, then the device could tell whether a fallback preserves meaning. Genuinely better, and disproportionate for a desk light — it adds an identity concept to the protocol, the config and both publishers. Noted as the escape hatch if multi-source setups ever become normal. |
| **Keep `USBCFG` for faster loss of trust** | Would shave up to 10 s off going amber when a dock is unplugged while still powering the device. Rejected: it is a latency improvement inside a budget D34 already accepts, it needs a pin, a trace, a devicetree binding and its own tests, and the firmware input path can be restored in a devicetree change if it ever earns its place. |
| **Keep the two GP pins for activity LEDs instead** | Not rejected, just not an MCU concern: `GP0` and `GP1` default to `LED_URx`/`LED_UTx`, so two LEDs give a human "the host is talking / the device is talking" with no MCU pins and no configuration. Worth considering on the PCB purely as an indicator. |

## Consequences

**Accepted costs**
- A device whose host disappears while still powered — a mains-powered dock unplugged from the laptop,
  or a laptop asleep on a charging port — sits amber indefinitely rather than using an available Wi-Fi
  source. Useless but truthful, which is the side Rule 4 already picked.
- Reconfiguring from wired back to wireless needs a reset, and if the daemon is still running on serial
  it will reclaim immediately after that reset. Stop the daemon or unplug. Worth documenting for
  anyone who configures Wi-Fi over the console and wonders why the lamps stay on the wire.
- Behaviour now depends on one bit of history rather than being a pure function of present evidence.
  Bounded to a single boot, and `transport_init()` is the reset.
- While latched, `wigwag/online` and button presses stop reaching MQTT subscribers, because the module
  is no longer serviced. Acceptable: a wired device's peers should be looking at the wire.

**Benefits**
- The device can no longer answer a question about the wrong machine.
- Simpler than what it replaces: the release window, `usb_claimed_ms` and the `usb_cfg` input all
  disappear, and firmware size went *down* (33 220 → 33 212 B) despite gaining the module-stop logic.
- No ping-pong is possible, so the crash-loop caveat noted against the soft hold is gone.
- Q3 resolves as a corollary rather than a separate judgement: with no possible fallback, retrying
  Wi-Fi is unambiguously waste.
- Two fewer pins on the PCB, and no per-unit chip-settings programming step.

**Revisit if** multi-source setups become normal — at which point the honest fix is host identity in
the protocol, not a cleverer fallback rule.
