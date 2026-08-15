# ADR-0016 — Watchdog feeding must be earned by every task the display depends on

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

ADR-0007 and Rule 4 make one promise: the device must never confidently display a state it cannot
confirm. `link.c` keeps that promise against the outside world, and D75 showed how much care it took
— positive liveness in both failure domains, because absence of bad news is not evidence.

None of that survives a firmware fault. Every detector in `link.c` is evaluated from the AT service
loop, so if that loop wedges, supervision wedges with it: nothing calls `link_tick()`, nothing
notices the missing keepalive, and `lamp_pwm_set_link()` is never called with `false`. The lamps keep
animating the last state they were told, forever, with total confidence. It is exactly the failure
ADR-0007 forbids, arrived at from the inside instead of the outside — and it is invisible, because
the mechanism that would report it is the mechanism that died.

The render thread fails the same way in reverse. If it stops while the AT loop keeps running, the
device continues talking to the broker, publishing `wigwag/online`, and answering keepalives, while
the lamps sit frozen on whatever duty they last held. From the desk it looks like a working device
displaying a state; nothing in the system disagrees.

A watchdog is the standard answer, and the standard implementation is worthless here. Feeding a
watchdog from a timer, an idle hook, or one service loop proves only that *that* context is alive.
On a device with two contexts that must both be alive for the display to be honest, a watchdog fed
from either one alone certifies half the system and silently vouches for the other half.

Two smaller facts shaped the mechanism:

- The PL10's WDT is real and usable, but **PL10's devicetree has no `wdt` node** — the fifth node
  this project has had to add — and its **RSTC differs from the JH revision the `hwinfo` driver was
  written against**, so the obvious way to report a watchdog reboot returns a confident wrong answer
  (bug 4 in `docs/upstreaming-to-zephyr.md`).
- This part is halted by a debugger constantly during development, so a watchdog that reboots on
  every breakpoint would be abandoned within a day.

## Decision

**Feeding is earned, not automatic.** Every task whose liveness the display depends on checks in with
`wdog.c` — `WDOG_TASK_AT` from the AT service loop, `WDOG_TASK_LAMP` from the render thread — and the
feeder refuses to feed unless *all* of them have checked in within `WDOG_MAX_AGE_MS` (500 ms). One
silent task stops the feed even though the other is running perfectly, and the hardware reboots the
device 2 s later.

**Each task beats after doing its work, not on waking.** The render thread checks in after the frame
reaches the PWM hardware; the AT loop checks in after draining the UART, ticking the state machine,
sampling the button and re-evaluating the link. A beat that only proves the scheduler ran would
certify a thread wedged inside a driver.

**The liveness accounting is pure logic, in `wdog.c`, with no Zephyr and no peripheral.** It is
host-tested under plain clang like `link.c` and `lamp.c` — including the case that matters, one task
beating while the other has gone quiet, and the 32-bit millisecond wrap. The Zephyr and hardware half
lives in `wdog_wdt.c`.

**Detection is 500 ms of staleness plus a 2 s hardware window**, so ~2.5 s worst case. Both numbers
are chosen against D34's 10 s "must not show a stale state" budget and against the 10 ms period both
loops actually run at: 500 ms is fifty missed turns, far outside any jitter, and comfortably past the
~24 ms the AT loop can block in a full-length transmit.

**Normal mode, no minimum window.** A closed window catches a task running too *fast*, which is not a
failure this device has, and it would make the feed itself capable of resetting a healthy device.

**`WDT_OPT_PAUSE_HALTED_BY_DBG` is requested explicitly**, even though this peripheral does it by
default and the driver applies nothing, because the requirement should be visible in the code rather
than inherited by luck.

**A refused feed is announced, and so is a watchdog reboot.** `NOT FEEDING, <task> task stale <n> ms`
goes out rate-limited before the reset, and the next boot prints `RESET BY WATCHDOG` — read from
`RSTC.RCAUSE` directly, at the offset and bit positions this part's datasheet gives (§16.6.2), since
the upstream driver misreads them. On an appliance whose only output is three lamps, a silent reboot
loop is indistinguishable from a slow connection.

**No callback on timeout: reset directly.** A callback would run in interrupt context on a device
already known to be unreliable, to print through a UART that may be what wedged. `RCAUSE` carries the
same information safely, one boot later.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **Feed from the AT loop unconditionally** | The obvious implementation, and it certifies the wrong thing. It would have let the render thread die with the lamps frozen on a stale state while the watchdog cheerfully kept the device alive — the precise failure this ADR exists to catch, now demonstrated on hardware. |
| **Feed from a kernel timer or the idle thread** | Worse still: it proves the *kernel* is alive, which it almost always is. Both application contexts could be wedged and the watchdog would never fire. |
| **One watchdog channel per task** | The PL10's WDT supports a single timeout (`max-installable-timeout-count = <1>`), so this is not available. It would also not improve the outcome: the reset is the same either way, and `wdog_stalest()` already names the culprit on the console. |
| **Let the render thread feed, since a frozen display is the visible failure** | Symmetrically wrong. The AT loop is what supervises the link, publishes `wigwag/online` and answers the button; a device that renders beautifully while deaf is still lying. Both matter, which is the whole point. |
| **Software-only watchdog: have each task cross-check the other and set the lamps to fault** | Cheaper, and it degrades instead of rebooting. Rejected because a task wedged with interrupts disabled, or corrupted state, cannot be talked out of it by other software — and this device has no operator to notice. The WDT is a hardware backstop precisely for the cases software cannot reach. `link.c` already covers everything software *can* reach. |
| **Windowed mode with a minimum period** | Would catch a runaway loop feeding too fast. Not a failure mode this design has (both loops are absolute-deadline scheduled, D70), and it introduces a way for the feed itself to reset a healthy device. |
| **Use `hwinfo_get_reset_cause()` for the reset report** | It answers 0 for every reset on this part, and would misreport a watchdog reset as `RESET_PIN` even at the right address. Using it would have meant the diagnostic silently didn't work — which is how the driver bug was found. Revisit when the upstream fix lands. |
| **`WDT_OPT_PAUSE_IN_SLEEP`** | Unsupported by this driver, and meaningless here: D24 says the device never sleeps. |
| **No watchdog; rely on `link.c`** | Rejected. `link.c` cannot supervise itself, and after D75 the project has already learned once that supervision needs positive evidence rather than an assumption that the supervisor is fine. |

## Consequences

**Accepted costs**
- **16 bytes of SRAM** and 896 bytes of flash, measured: 4 512 → 4 528 B RAM (55.27 % of 8 KB),
  22 440 → 23 336 B flash. Under Rule 5 this is the cheapest safety mechanism in the firmware. It
  later went *down* to 23 272 B by dropping `CONFIG_HWINFO`, which the reset report no longer needs.
- Any new task whose liveness the display depends on must be added to `enum wdog_task` **and** must
  check in. Adding the enum entry alone stops the device feeding — deliberately the safe direction,
  but it will present as a reboot loop, so the header says so.
- A genuine 500 ms stall anywhere in either loop now reboots the device. Long blocking work must move
  off these threads or beat while it runs.
- Two devicetree nodes (`wdt`, `rstc`) carried in the app overlay until upstreamed, and one more
  local workaround for a driver bug.

**Benefits**
- The last gap in fail-visible is closed. A wedge in either context now produces a reboot and an
  honest amber flicker on the way back up, instead of a confident stale display.
- The failure is diagnosable without a debugger: a named stale task before the reset, `RESET BY
  WATCHDOG` after it.
- D81's open question is answered. The watchdog was the one item on the AT loop's "revisit when"
  list that looked like it might force a second thread; it did not, because proving both contexts
  are alive is independent of how many threads exist.
- The accounting is pure and tested — 6 159 checks — so the part of this that is easy to get subtly
  wrong is not being verified by staring at hardware.

**Revisit if** a third context appears (add it to `enum wdog_task` and make it beat), if any task
legitimately needs to block longer than 500 ms, or if `CONFIG_PM` is ever enabled — a sleeping device
needs `WDT_OPT_PAUSE_IN_SLEEP` or a much longer window, and this driver supports neither.
