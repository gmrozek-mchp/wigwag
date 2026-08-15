# Journal

Append-only development log. **Newest entry first.** Durable decisions live in
[`docs/adr/`](docs/adr/) and are referenced from here as `ADR-NNNN`.

Entries record what was done, why, and — importantly — what was tried and rejected.

---

## 2026-08-14 — the watchdog, and why feeding it from the AT loop would have been worthless

**Done** — `wdog.c` (pure liveness accounting) + `wdog_wdt.c` (the WDT and the reset-cause report),
two new devicetree nodes, and 6 159 host checks. **ADR-0016**, D93–D95. Plan item 17 is complete.

**The insight the whole thing turns on.** The obvious implementation — feed the watchdog from the AT
service loop, which already runs every 10 ms — is worse than no watchdog, because it would certify
half the system while silently vouching for the other half. The render thread could be dead with the
lamps frozen on a stale state, the AT loop would keep feeding, and the device would stay up
indefinitely displaying a lie. That is D75's lesson arriving from a new direction: *feeding must be
earned*. Both contexts check in, and the feeder refuses unless both are recent.

Each task beats **after** doing its work, not on waking — the render thread after the frame reaches
the PWM hardware, the AT loop after draining the UART, ticking the state machine, sampling the button
and re-evaluating the link. A beat that only proves the scheduler ran would happily certify a thread
wedged inside a driver.

500 ms of allowed staleness (fifty missed turns at 10 ms, and 20× the ~24 ms the AT loop can block in
a full-length transmit) plus a 2 s hardware window: ~2.5 s worst case, inside D34's 10 s.

**Demonstrated, not assumed.** A temporary `k_sleep(K_FOREVER)` in the render thread after 15 s:
```
33.1  wigwag: NOT FEEDING, lamp task stale 506 ms
33.3  wigwag: NOT FEEDING, lamp task stale 766 ms
 ...
35.1  *** Booting Zephyr OS build 357467a011cd ***
35.1  wigwag: RESET BY WATCHDOG (rcause 10)
```
Refusal at the first stale sample, reset exactly 2.0 s later, repeating every ~17.6 s. The AT loop was
running normally throughout — `at RESETTING`/`BACKOFF` transitions continue right up to the reset —
which is the point: an unconditional feed from there would have kept this device alive forever.

**Then the other direction**: fake module attached, full 15-step connect, `link LINKED` at 1.9 s, then
**128 s of silence** — no refusal, no reset, through every long transmit and 5 s keepalive poll. An
earlier 95 s run without the module confirmed the same across the reset/backoff cycle; the AT timeout
counter climbing 3→7 monotonically is the proof no reboot happened.

### The bug this uncovered: `hwinfo` answers wrongly on this part

The first fault-injection run reset perfectly and printed **nothing** about the cause. `hwinfo` was
configured, the driver was compiled, the `rstc` node was enabled, and `hwinfo_get_reset_cause()`
returned success with a cause of 0.

`hwinfo_mchp_g1.c` was written against the **JH** revision of RSTC, and PL10's is a different
peripheral:

| | JH (assumed) | PL10 (actual, datasheet §16.6.2) |
|---|---|---|
| RCAUSE offset / width | `0x00`, 8-bit | **`0x04`, 32-bit** — `0x00` is `CTRLA` |
| EXT / WDT / SYST | 4 / 5 / 6 | **3 / 4 / 5** |
| bit 6 | — | **LOCKUP** |

So the driver read `CTRLA`, got 0, and matched nothing. Reading `0x40000c04` directly returns `0x10`
— bit 4, WDT — exactly as the datasheet predicts. **Even at the right address the answer would have
been wrong**, not absent: a watchdog reset would decode as `RESET_PIN | RESET_USER`. That makes this
the worst of the four upstream bugs found so far — bug 1 fails silently, this one fails
*confidently*. Written up as bug 4 in `docs/upstreaming-to-zephyr.md`.

The fix here: read `RCAUSE` in `wdog_wdt.c`, at the address the devicetree node gives, and leave that
node **disabled** so no driver binds and answers wrongly. `CONFIG_HWINFO` dropped entirely, which also
reclaimed 48 bytes of flash. Also noted for the same issue: `wdt_mchp_g1.c` derives a flag from
`DT_NODELABEL(wdog)`, a label that exists in no in-tree devicetree — harmless only because the macro
is never expanded, and its `#if defined(...)` guard tests an always-defined object macro, so the check
it was meant to gate runs unconditionally.

### Dead ends and small things

- **`wdt` and `rstc` are the fifth and sixth devicetree nodes PL10 lacks.** Driver and binding both
  upstream, JH/SG/SAM all instantiate them, PL10 has nothing. Addresses and IRQ verified from
  `hal_microchip` (WDT `0x40002000` IRQ 1, RSTC `0x40000c00`) and they happen to match JH exactly —
  which is precisely the coincidence that makes the RCAUSE difference easy to miss.
- **No callback on the timeout.** A callback would run in interrupt context on a device already known
  to be unreliable, to print through a UART that may be what wedged. `RCAUSE` carries the same
  information one boot later, safely.
- **Nothing clears `RCAUSE`.** §16.6.2: each reset sets its own bit and writes all others to 0. My
  first draft called `hwinfo_clear_reset_cause()` — which is both unimplemented for this family and
  conceptually wrong.
- **`WDT_OPT_PAUSE_HALTED_BY_DBG` is requested explicitly** even though the peripheral does it by
  default and the driver applies nothing. This part is halted by a debugger constantly; the
  requirement should be visible rather than inherited by luck.
- Boot-order trap: arming before `lamp_pwm_init()` would trip on the 1.6 s lamp selftest and produce a
  reset loop that looks exactly like a hardware fault. Armed last, after every task is beating.
- `wdog.c` needed `<stddef.h>` for `NULL` — it includes no Zephyr headers by design, so it gets
  nothing for free.
- The reset report stays **silent** for POR, EXT and SYST. Those are the ordinary ways this board
  starts, and a line on every boot trains the eye to skip the one line that matters.

**Measured** — flash 23 272 B (37.88 %), RAM **4 528 B (55.27 %)**. The watchdog costs **16 bytes of
SRAM**: 8 for the timestamps, 8 for the arm flag and gripe rate-limiter. Cheapest safety mechanism in
the firmware by a wide margin.

**Host tests** — 7 730 checks total, 0 failures: rnwf_at 100, link 25, lamp 1 423, button 23, wdog
6 159.

---

## 2026-08-14 — wigwag/online: the birth message, and the Last Will finally verified

**Done** — `main.c` publishes `1` retained to `wigwag/online` once per connection, closing the last
gap in `CONTEXT.md`'s topic table.

**Why it needed to exist.** `AT+MQTTLWT` had registered `0` as the will since the AT client was
written, so the broker would report an unclean death — but a will with no positive counterpart is
useless. A subscriber could not distinguish "device never seen" from "device connected", because both
look like an absent topic.

**Keyed on AT `READY`, deliberately not on the link condition.** This topic means "connected to the
broker", which stays true when the host daemon dies. Publishing `0` because *the host* went away would
be the device misreporting itself. The two facts are separate and now have separate topics:
`wigwag/online` is the device's own liveness, `wigwag/host_online` is the daemon's, and `link.c`
combines them into whether the lamps can be trusted.

Retained, so a late subscriber sees the current truth rather than a stale `0` from a previous death.

**Verified on hardware, including the will**
```
wigwag/online stale-from-earlier   <- retained junk planted first, delivered on subscribe
wigwag/online 1                    <- birth message
wigwag/online 1                    <- second connection
wigwag/online 0                    <- the will, after killing the module uncleanly
```
**This is the first verification that the Last Will actually fires**, which the AT client's own journal
entry had listed as unverified. Two `1`s is correct rather than a bug: a backoff retry connected once
the fake module started, then a reset reconnected, and the birth message fires once per connection —
`announced_online` resets whenever the client leaves READY.

**The caveat worth stating.** The will fired because the *fake* module held the MQTT session and was
killed. That verifies the whole path — `AT+MQTTLWT` parsed, carried into the broker's CONNECT, and
honoured on an unclean drop — but through our model of the module. Whether the real RNWF02 honours
`AT+MQTTLWT` identically is still untested, and belongs with the `EV72E72A` bring-up.

**Also open**: a *clean* `AT+MQTTDISCONN` would not fire the will, so a deliberate disconnect would
need an explicit `0` first. Nothing disconnects deliberately today, and the code does not pretend to
handle it.

**Measured** — flash 21 472 B (34.9 %), RAM 4 480 B (54.7 %), unchanged.

---

## 2026-08-14 — brightness, in two layers that deliberately do not share a mechanism

Asked whether brightness should be one value or per bulb. The answer turned out to be both, at
different layers, because they are different problems:

| | what it is | where it lives | changes |
|---|---|---|---|
| `wigwag/brightness` | **preference** — "too bright for my desk at night" | MQTT, retained, 0-255 | at runtime |
| per-lamp `gain` | **calibration** — LEDs of different colours are not equally efficient at the same duty | devicetree, beside polarity | per board |

Sharing one mechanism would give the worst of both: calibration clobbered whenever somebody turns
the device down for the evening, and a pile of per-colour topics to carry a fixed hardware fact.
Recorded as D88.

**Done**
- `firmware/dts/bindings/led/wigwag,lamps.yaml` — an **app-local binding**. `pwm-leds` cannot express
  this: its child binding allows only `label` and `pwms`. Zephyr's docs confirm bindings are found in
  `dts/bindings` under the application source directory, so no CMake change was needed; a local
  `dts/bindings/vendor-prefixes.txt` registers the `wigwag` prefix so the build does not warn about an
  unknown vendor.
- `lamp_scale()` and `lamp_brightness_parse()` in `lamp.c` — pure, tested, on the *core* side of the
  boundary this time. The gamma bug earlier this phase came from putting arithmetic in the renderer
  where no test could reach it; not repeating that.
- Subscribed as a fourth script step, so the device adopts the desk's setting on every connect. Being
  retained on the broker means no NVM and nothing to persist locally.

**Both scales act on the perceptual level, not the duty (D89)**
Halving brightness has to *look* half. Since gamma cubes, scaling the duty would land at about 79 %
apparent brightness — technically dimmer, visibly not half. Scaling the level first and then cubing
gives what a brightness control should mean.

**Brightness cannot silence the fail-visible pattern (D90)**
The tension worth naming: if brightness 0 dimmed everything, a device that had lost its link would go
dark, which is indistinguishable from switched off — the exact silent lie Rule 4 and ADR-0007 exist to
forbid. So the flicker floors at `LAMP_FAULT_MIN_BRIGHTNESS` (96). Brightness is a preference about
lamps that *report*; the flicker is the device admitting it does not know, and that is not
negotiable. A smoke alarm you cannot turn down to zero.

**Verified on hardware**
- Seven-step sweep, 255 → 192 → 128 → 64 → 32 → 8 → 0, each applied in order and confirmed dimming.
- **With brightness at 0, dropping `host_online` left green dark while yellow and red flickered.**
  The floor holds where it counts, on the device, not just in a test.
- `gain 255/255/255` read from devicetree and printed at boot, alongside channels and polarity flags.

The boot self-test deliberately ignores both gain and brightness: it is a wiring test, and a lamp that
fails to light because someone dimmed the device would defeat the point.

**Measured** — flash 21 372 B (34.8 %), RAM **4 480 B (54.7 %)**, unchanged: gain lives in flash and
brightness is one byte. Tests now **1571 checks, 0 failures** across four suites.

**Open**
- All three gains are 255, i.e. no correction. Deliberately: any number picked now calibrates *bench*
  parts — an SMD yellow with a fixed resistor against two hand-wired LEDs — and the PCB will run
  10 mm diffused lamps from 5 V through FETs at 20-60 mA, a completely different operating point. The
  mechanism persists; the numbers will not.
- Where dimming stops being useful was not established, and is not worth establishing on bench LEDs.

---

## 2026-08-14 — button.c: presses published, and the device finally talks back

**Done**
- `firmware/src/button.{h,c}` — pure debounce and press classification, no Zephyr. 20 ms settling,
  duration measured between debounced edges, and a long-hold threshold at 3 s.
- `firmware/src/button_gpio.{h,c}` — the polled GPIO binding, which is a pin read and nothing else.
- `firmware/tests/test_button.c` — 23 checks. Four suites now total **1430, 0 failures**.
- SW0 in the overlay as `gpio-keys` on PB03, `GPIO_PULL_UP | GPIO_ACTIVE_LOW`, alias `sw0`.
- `main.c` samples it in the existing 10 ms loop and publishes `wigwag/button`.

**Polled, not interrupt-driven — a deliberate deviation from plan item 17 (D86)**
Plan item 17 says "GPIO IRQ + debounce". The GPIO driver does support interrupts, but only via
`CONFIG_INTC_MCHP_EIC_G1`, and **PL10's devicetree has no `eic` node** — the driver
(`intc_mchp_eic_g1.c`) and binding exist, and the JH, SG and SAM families instantiate it, but this
family does not. That is the same three-layer gap as TCC0: silicon has it, driver exists, board/SoC
layer missing.

Polling won on the merits rather than to dodge the work:
- a press lasts ~100 ms and sampling is every 10 ms, so nothing can be missed;
- debounce needs tens of milliseconds of settling anyway, so **polling *is* the debounce** where an
  interrupt would only start a timer;
- wigwag is USB-powered and never sleeps (D24), so wake-from-sleep — the real reason to want a pin
  interrupt — buys nothing;
- no EIC node, no interrupt controller, no ISR-safe handoff. Rule 5.

The EIC gap remains available as a separate upstream contribution if anything ever needs it.

**Verified on hardware, first try** — 14 presses over 30 s, all published, no duplicates:
```
wigwag: published press 350 ms
wigwag: long press at 3000 ms (provisioning, not yet built)
wigwag: published press 4520 ms
...
wigwag/button {"event":"press","ms":350}
wigwag/button {"event":"press","ms":4520}
```
Durations 190–530 ms for taps and 4520/6670 ms for two deliberate holds. The long-hold fires at
exactly 3000 ms **while still held** and the release still publishes its true duration — the D35/D58
split intact: the host gets the raw fact, the device keeps its local gesture.

**This is the first time the device→host direction has worked.** Everything before was host→device.
The chain is SW0 → debounce → `AT+MQTTPUB` → SERCOM0 → fake module → mosquitto → subscriber.

Two things confirmed rather than assumed. **PB03 is shared with the on-board debugger** (DBG2) and
caused no spurious presses in 30 s with the debugger attached — it had been flagged as the first
suspect if presses looked erratic. And a deliberate tap bottoms out around **200 ms**, so the 20 ms
debounce has ample margin; that 200 ms floor is the number that would matter if double-tap ever
became a gesture.

**Details worth keeping**
- The payload is built without `printf`. `main`'s stack is the tight one at 860 of 1024 B (D78) and
  already carries `printk` plus the AT script's `vsnprintf`; adding another varargs frame to the
  publish path is the wrong direction, so `press_payload()` copies a fixed prefix, reverses decimal
  digits into place, and appends the suffix.
- Presses are published **not retained**. A press is an event, not a state — a retained one would be
  replayed to every future subscriber, including this device after a reboot.
- A press that cannot be sent is **dropped with a log line, not queued**. D35 says the host decides
  what a press means, and one delivered minutes late would be a lie about when it happened.
- The first sample after boot is adopted silently, so a device that starts with the button held —
  or reads a debugger-driven level on that shared pin — cannot manufacture a press it never saw begin.
- `CONFIG_GPIO=y` was needed and was missing: the link failed with `undefined reference to
  __device_dts_ord_5`, the `portb` device, because the lamps are PWM and nothing had needed GPIO
  until now. Costs 40 bytes of RAM.

**A test helper with the exact bug its test was hunting**
`test_clock_wrap_is_survivable()` failed, and the fault was in the helper, not the code under test:
`hold()` computed `end = now + ms` and looped `while (now < end)`, which is not wrap-safe — at
`0xFFFFFFFA + 100` the end wraps below the start and the loop body never runs. `button.c` itself uses
wrap-safe subtraction throughout and was correct. Helper now counts iterations. Pleasing in a small
way: writing the wrap test caught a wrap bug, just not the one it was aimed at.

**Measured** — flash 20 940 B (34.1 %), RAM **4 480 B of 8 KB (54.7 %)**. The button costs 40 bytes.

**Open**
- Provisioning mode (D58) is where `BUTTON_EVENT_LONG` will go; today it only logs.
- The watchdog is the last of plan item 17, and it pairs with link supervision: a wedged AT loop is
  exactly what supervision cannot catch by itself, since it is the thing doing the supervising.

---

## 2026-08-14 — "PA24 isn't working": one pin conflict, one gamma bug, and a test that couldn't reach it

Reported from the bench: flickering on PB02 and PA25, nothing at all on PA24. Two independent
faults, and the flicker was not one of them.

**The flicker was correct.** The exercise script ended by killing the fake module, so the keepalive
timed out, the link went untrusted, and the lamps went to the fail-visible pattern — yellow and red
stuttering, **green deliberately held off**. Green means "idle, ready for you", and a blind device
must never imply that. So the report was actually evidence that PA25 had started working.

**Fault 1: XOSC32K was silently overriding the pinmux.** PA24 and PA25 are XTAL32K1 and XTAL32K2, and
the board devicetree enables the 32.768 kHz oscillator (`xosc32k-en = <1>`). Datasheet §13.5.1 is
unconditional: "the XTAL32K1 and XTAL32K2 pins are automatically configured when the XOSC32K
oscillator is enabled". TCC0's mux on both pins was accepted and then ignored — the same shape of
failure as the `polarity`/`flags` binding bug, and just as invisible, because the override happens in
the oscillator rather than in PORT.

Disabling it is safe here: the board's crystal is **not connected by default** (its user guide routes
those I/O lines to the edge connector; attaching it means cutting straps J107/J108), nothing consumes
XOSC32K since `gclkgen0` comes from the internal 24 MHz OSCHF, and there is no RTC. Recorded as D82.

**This took three attempts to state correctly, which is the more useful story.**

1. First version: "it takes both pins and costs a one-second boot delay". The pin claim came from
   §13.4.2.2's crystal-mode sentence; the delay came from reading `TIMEOUT_XOSC32KCTRL_RDY = 1000000`
   with `WAIT_FOR` documented in microseconds.
2. **Measured the boot delay: there isn't one.** The device reaches `main()` **5 ms** after reset,
   with or without the oscillator enabled, so a one-second stall was never possible. The claim would
   have gone into a bug report as fact.

   Worth recording how that number was nearly missed. The first attempt timed reset to first console
   byte and got ~1.6 s, which looked alarming for a 19 KB image on a 24 MHz part. Almost all of it is
   **pyOCD**: interpreter startup, pack load, SWD connect and teardown, measured at 0.93-1.68 s on its
   own, varying run to run. The banner was already sitting in the OS buffer before `pyocd reset` even
   exited. Timing a device through a debugger measures the debugger.
3. Chasing why, I found `xosc32k-xtal-en` defaults to 0 — External Clock mode — where §13.4.2.2 says
   *only* XTAL32K1 is overridden and XTAL32K2 stays usable. So I narrowed the report to one pin.
4. **The bench said otherwise.** Both pins had been floating. A controlled A/B settled it: identical
   firmware and wiring, only `xosc32k-en` changed, lamps driven to full by the power-on test — both
   dead with the oscillator on, both alive with it off. XTALEN really is 0, confirmed in
   `devicetree_generated.h`, so **§13.4.2.2 does not describe this silicon** and §13.5.1 does.

Two lessons worth more than the fix. **Measure before asserting** — one inference was wrong, one was
right, and neither was knowable without the bench. And **where the datasheet and the board disagree,
believe the board, and record which one you actually tested.** The §13.4.2.2 discrepancy is now flagged
in `docs/upstreaming-to-zephyr.md` as a question for Microchip, separate from the Zephyr fix.

Bonus confirmation from the same session: holding WAIT on hardware showed red steady and then a slow
blink, so the **30 s escalation works on the device**, not just in host tests.

**Fault 2: my gamma arithmetic threw away the low end.** `gamma_pulse()` computed `level³ / 255²`
first, which truncates:

| level | old | fixed |
|---|---|---|
| 40 | **0.000 %** | 0.386 % |
| 48 | 0.392 % | 0.667 % |
| 128 | 12.549 % | 12.648 % |

**Every perceptual level below 41 rendered as fully off**, and 41–48 all collapsed to one value. IDLE
green ran at half its intended duty. Rewritten to scale in three steps so intermediates stay large
and inside 32 bits, with no 64-bit helpers pulled in.

**Why 1281 host checks missed it: the function was on the wrong side of the boundary.**
`gamma_pulse()` lived in `lamp_pwm.c`, the Zephyr renderer, where no host test can reach — even
though it is pure integer arithmetic with no hardware dependency. Moved to `lamp.c` as
`lamp_gamma_pulse()` and covered by tests for monotonicity, endpoints, and **absence of a dead zone**.
The lesson generalises: the core/adapter split only protects what is actually on the core side, and
"is this pure?" is the test, not "does it feel like hardware?".

**Added a power-on lamp test**, because both faults would have been obvious in the first second if
anything had ever driven the lamps at full brightness. Each lamp to 100 % for 400 ms, then all three.
Worth keeping permanently on an appliance whose only output is light — a 0.4 %-duty steady green
proves nothing, and the plan already wants an all-lamps pattern for provisioning mode anyway.

**IDLE brightness, chosen by eye rather than by argument.** Swept 48/64/80/96/128/160 on hardware at
4 s each; **128 (12.6 % duty)** picked. `LAMP_IDLE_DIM` now carries the caveat that the bench
understates the product — the Curiosity Nano drives an LED through a series resistor at a few mA,
while the PCB drives 10 mm diffused lamps from 5 V through FETs at 20–60 mA — so this wants
revisiting with real lamps, and `wigwag/brightness` is the proper home for per-desk trimming.

Verified independently: the measured pulse width at IDLE was **~13 µs**, against a computed 13 339 ns
for level 48. That match confirmed the pin, the mux, the gamma and the PWM path in one measurement.

**A test that encoded the wrong thing.** `test_idle_is_green_only_and_dim()` asserted
`level < 128` — and the value chosen by eye was exactly 128, so it would have rejected the right
answer. Rewritten to assert on rendered *duty* being under a quarter of the period, because the level
is on a cube-root scale and a number that looks like "half" is nothing of the sort.

**Process note.** I twice ran a visual hardware test and only then told the user to look, and I was
also holding the serial port so the console was invisible to them. Both are bad habits for
hardware work: announce observable tests *before* triggering them, and hand over the port
(`python -m serial.tools.miniterm /dev/cu.usbmodem… 115200`) rather than capturing it.

---

## 2026-08-14 — Why the AT loop stays on the main thread

Asked whether main holding the app logic is normal in Zephyr, or whether it should be a named task.

**It is normal.** `main()` is invoked from `bg_thread_main()` in `zephyr/kernel/init.c` — the main
thread *is* the kernel's initialization thread, running at `CONFIG_MAIN_THREAD_PRIORITY` (default 0),
and the Kconfig help notes "main() can then change its priority if desired". Using it as a service
loop is idiomatic; plenty of Zephyr applications never create a thread at all.

**But the interesting part is what splitting would and would not buy**, and the measurements from the
stack work answered it:

- **It costs ~600 bytes, about 7 % of this part's SRAM.** main would keep a stack for init while a
  new `at` thread carried the loop: ~512 + ~1024 + 512 (lamp) against today's 1024 + 512, plus ~112 B
  of thread struct.
- **It would not relieve the pressure it looks like it should.** main's measured peak is
  `max(init depth, loop depth)`, not their sum, because init completes before the loop starts.
  Splitting duplicates capacity rather than lowering the maximum. main sits at 860 of 1024 B because
  of `printk` formatting and the AT script's `vsnprintf`; the cure is narrower format strings, not
  another stack. Worth writing down because "the stack is 83 % full, give it its own thread" is a
  tempting and wrong conclusion.

**Decision (D81): keep the loop on main, and name it honestly.** The body is extracted into
`at_service()` so `main()` reads as orchestration, and `k_thread_name_set(k_current_get(), "at")`
makes reports say `at` instead of a thread-object address. That call compiles out entirely when
`CONFIG_THREAD_NAME` is off, which it is in the shipping build — it exists for the measurement
overlay, where a report full of hex addresses is nearly useless.

Verified: after the refactor the analyzer prints `at : STACK: unused 164 usage 860 / 1024 (83 %)` —
identical depth, so extracting the function cost nothing, and the name lands. Flash +28 B, RAM
unchanged at 4 440 B.

**What would change the decision**, recorded in a comment above `at_service()` so it is found when
it matters rather than rediscovered:
- a second context needing the AT client concurrently — `button.c` will want to publish from an
  interrupt, and while a flag consumed by this loop suffices, a message queue feeding a dedicated
  thread is the textbook shape if it grows;
- the watchdog needing independent evidence that more than one thread is alive;
- credentials lengthening the AT commands, since that deepens `vsnprintf` on this very stack.

Also noted: ADR-0008 budgeted three threads at 768 B each, but named *merging* the link supervisor as
its fallback if tight. Evidence agrees — link supervision is event-driven with one grace timer and
needs no thread of its own, so the plan's three-thread shape is now two by choice rather than by
omission.

---

## 2026-08-14 — lamp.c: three lamps, and CONTEXT.md's table becomes executable

**Done**
- `firmware/src/lamp.{h,c}` — pure animation: `(state, link condition, time) -> three perceptual
  levels`, plus a state parser. No Zephyr, so every behaviour is host-testable. Same split as
  `rnwf_at.c` against `rnwf_uart.c`, which is now the established pattern in this codebase.
- `firmware/src/lamp_pwm.{h,c}` — the Zephyr renderer: gamma, per-lamp polarity, and its own thread.
- `firmware/tests/test_lamp.c` — **511 checks**, encoding CONTEXT.md's lamp table as assertions.
  Three suites now total **636 checks, 0 failures**.
- Three lamps in the overlay, and `main.c` shrinks to orchestration now that animation has a home.

**The pin assignment, and a useful accident**
| Lamp | Channel | Pin | Wiring | Polarity |
|---|---|---|---|---|
| green | WO0 | PA24 | external LED + 470R–1k | active **high** |
| red | WO1 | PA25 | external LED + 470R–1k | active **high** |
| yellow | WO2 | PB02 | the board's own yellow LED | active **low** |

PA24/PA25 are the 32.768 kHz crystal's I/O lines, which the board routes to the edge connector by
default (straps J107/J108 intact), so they were free without rework. PB08/PB09 would also have
carried WO0/WO1 but are the touch button and *not* connected to the edge connector by default.

The accident worth keeping: **this board genuinely mixes polarity**, and the boot line proves it is
resolved per lamp — `lamps on tcc@42001800 ch0/2/1, flags 0/1/0`. The PCB will be uniformly active
high through low-side FETs (D26), so a design that assumed one polarity would have worked on the
bench and failed on the product, or vice versa. Mixed hardware forced the right shape.

**Two subtleties the tests pinned down**
- **WAIT's 30 s escalation measures how long you have been waiting, not time since the last
  message.** `lamp_pwm_set_state()` is therefore idempotent: re-setting the current state does not
  restart the clock. Without that, the host's repeated BUSY heartbeats would be harmless but any
  repeated state would hold its own escalation off forever. Tested both ways, including that a fresh
  WAIT on a device up for an hour shows steady red rather than blinking immediately.
- **An unparseable payload keeps the previous state.** Inventing one is worse than showing a
  slightly old one, and link supervision is what catches a host that has stopped making sense. The
  parser matches the `"state"` key before its value, so `"reason":"WAIT for permission"` cannot be
  read as a state — one of eight rejection cases under test.

**Fail-visible, made concrete**
The flicker holds **green off** deliberately. Green means "idle, ready for you", and the one thing a
blind device must never imply is that everything is fine. Tests assert green stays dark while
unlinked in all four states, that the pattern is not static, and that it does not track the BUSY
breathe closely enough to be mistaken for it. The device also **boots untrusted**, so the lamps
flicker from the first frame until something proves otherwise rather than looking idle while
connecting.

**Why the renderer has its own thread**
`uart_poll_out()` blocks its caller for up to ~24 ms on a full-length AT command — two and a half
frames, visible as a hitch. The render thread runs at cooperative priority −1 so it preempts the AT
loop's blocking transmit. This is the concrete reason the plan's three-thread structure exists,
rather than a stylistic choice.

**Verified on hardware** — full sequence, driven by publishing to the real broker:
```
wigwag: lamps on tcc@42001800 ch0/2/1, flags 0/1/0
wigwag: link UNLINKED (starting) ... at READY ... state IDLE
wigwag: host_online = 1 ... link LINKED (ok)
wigwag: state BUSY ... state WAIT ... state ERROR
wigwag: host_online = 0 ... link UNLINKED (host gone)
```

**Measured** — flash 19 436 B (31.6 %), RAM **5 464 B of 8 KB (66.7 %)**. The 632 B increase is the
render thread: 512 B stack, ~112 B thread struct, ~12 B shared state. Stacks now total 3 840 B of
the 5 464 — still the whole story of this budget, and still untuned.

**Then the stacks got measured, and the guesses were wrong in an instructive direction**

`firmware/prj_stacks.conf` is a measurement-only overlay: `CONFIG_THREAD_ANALYZER` (which selects
`CONFIG_INIT_STACKS`) fills every stack with `0xaa` — threads *and* the interrupt stack — and prints
where the pattern stops, every 10 s, via `printk` rather than pulling in the logging subsystem.

After an exercise covering the connect script, all four lamp behaviours, an unparseable payload, a
link loss and recovery, a keepalive timeout and a full reconnect:

| Stack | Size | Peak | Free |
|---|---|---|---|
| **main** | 1024 | **860** | **164 (16 %)** |
| lamp | 512 | 348 | 164 (32 %) |
| idle | 256 | 92 | 164 |
| **ISR0** | 2048 | **264** | **1784 (87 % idle)** |

**The lamp stack I flagged as a risky guess was fine. `main` is the tight one at 84 % full** — and
`main` is the stack nobody had questioned. It runs `printk` formatting and the AT script's
`vsnprintf`, and it is the one to watch: if credentials with a password are ever configured, that
`vsnprintf` builds a longer command and this needs re-measuring. Exactly the argument for measuring
rather than reasoning about which number looks suspicious.

**Acted on the biggest one:** `CONFIG_ISR_STACK_SIZE=1024`, still 4× the measured 264 B, because the
only interrupt sources here are systick and SERCOM0 receive and neither nests. **RAM 5 464 → 4 440 B,
66.7 % → 54.2 %** — 1 KB returned, 12.5 % of all the SRAM on the part, on evidence. Verified the
tuned image still runs the full sequence on hardware afterwards.

**A report that looked like a bug and was not.** The first run showed `unused 164` for three
different stacks, which is too neat to believe. Raising the lamp stack from 512 to 768 moved `unused`
to 420 while `usage` stayed at 348 — so the tool was honest and 512−348, 256−92 and 1024−860 really
do all equal 164. Cheap discriminating experiment; worth repeating whenever a measurement looks
suspiciously tidy. Also added `CONFIG_THREAD_NAME=y`, without which the analyzer prints
thread-object addresses instead of names and the mapping is guesswork.

Recorded as **D78** (measure stacks with the overlay, never guess) and **D79** (ISR stack 1024 on
measured evidence).

**Open**
- `main` at 84 % is the live risk, and it grows if a Wi-Fi password or MQTT credentials are
  configured. Re-measure then; the alternative is moving the AT loop off `main` into its own thread.
- **There is no MPU on this part**, so `CONFIG_HW_STACK_PROTECTION` is unavailable.
  `CONFIG_STACK_SENTINEL` is in the measurement overlay only — it catches an overflow after the fact
  rather than preventing it, and it is not in the shipping build.
- No watchdog yet, and the device still never publishes `wigwag/online = 1`.

---

## 2026-08-14 — link.c: the device stops being able to lie

Built before `lamp.c` deliberately. Until link supervision exists, a renderer can only produce a
confident display of something nobody is confirming — so building the lamps first would have meant
building the ability to lie and fixing it afterwards (Rule 4).

**Done**
- `firmware/src/link.{h,c}` — the link condition in `CONTEXT.md`'s sense, LINKED or UNLINKED, with a
  diagnostic reason. Zephyr-free and clock-injected like the AT client, so it unit-tests on the host.
- AT client keepalive: while READY it sends a bare `AT` every 5 s and requires `OK` within 2 s.
- AT client now handles `+MQTTCONN:<CONN_STATE>` — 0 means the module lost the broker.
- Subscribes to `wigwag/host_online` as a second script step.
- `firmware/tests/test_link.c`, 25 checks. Suites now total **125 checks, 0 failures**.
- A provisional fail-visible lamp pattern in `main.c`, so an untrusted link actually *looks* wrong.

**Three failure domains, three detectors — the design point**
`+MQTTCONN:<CONN_STATE>` was the last unread piece of the spec, and reading it made the structure
obvious. There are three independent ways to lose the truth and no single signal covers them:

| Failure | What the device sees | Detector |
|---|---|---|
| Module or UART dies | nothing at all | keepalive `AT` goes unanswered |
| Broker or Wi-Fi lost | module is healthy and answers | `+MQTTCONN:0` from the module |
| Host daemon dies | module *and* broker healthy | `wigwag/host_online` = 0 via its Last Will |

The middle and last cases are why supervision could not simply live inside the AT client: a module
happily connected to a broker that nothing is publishing to is not a link worth trusting, and only
the application knows that. Hence `link.c` — the AT client reports what it can see, and `link.c`
decides whether the device may believe its own lamps.

**The policy is deliberately pessimistic.** LINKED requires positive evidence of every hop;
everything unproven is UNLINKED. Two consequences worth stating because they look like bugs
otherwise:
- The device does **not** go LINKED when the AT client reaches READY. It waits for `host_online`.
- A reconnect **discards** the previous `host_online` reading. A new AT session means a new
  subscription, so the old value is not evidence about the new one — trusting it would reintroduce
  the same bug one layer up. There is a 3 s grace period for the retained value to arrive, so a
  normal connect does not flash UNLINKED on its way up.

**Verified on hardware, all three cases**
```
wigwag: link UNLINKED (starting)          <- never claims a link before proof
wigwag: at READY ... host_online = 1
wigwag: link LINKED (ok)                  <- only once both hops are proven
wigwag: host_online = 0
wigwag: link UNLINKED (host gone)         <- module and broker healthy, daemon dead
wigwag: host_online = 1
wigwag: link LINKED (ok)
wigwag: at link down
wigwag: at BACKOFF (errors 0 timeouts 1 polls 5 overruns 0)
wigwag: link UNLINKED (module/broker down)
```
`polls 5 timeouts 1` is the line that matters: the module was killed with no AEC and no warning —
the exact scenario that previously produced fourteen seconds of silence and a device claiming
LINKED forever. Worst-case detection is the poll interval plus its timeout, 7 s, inside D34's 10 s
budget by construction rather than by measurement.

**A test that caught the right thing for the wrong reason**
Adding the `host_online` subscribe broke `test_connack_prefix_not_confused_with_connstate`, which
counted `OK`s to reach READY. The failure was correct — the script *is* one step longer — but the
test was over-specified. Rewritten to drain OKs until the state changes, so the next script step
does not break it again. Worth noting that three tests already used loops and only this one counted.

**Measured** — flash 18 616 B (30.3 %), RAM 4 832 B of 8 KB (**59.0 %**). Link supervision costs
**32 bytes** of RAM, which is `struct link`; the rest of the growth is code.

**Open**
- `flicker_pulse()` in `main.c` is provisional. With three lamps it becomes the amber flicker
  proper, and it belongs to `lamp.c`; it exists now because a supervisor whose only output is a
  console the device cannot reach is not fail-visible at all.
- The keepalive interval and timeout (5 s / 2 s) are chosen, not tuned. They cost one `AT` round
  trip per 5 s, which is negligible traffic, but the tradeoff against D34's 10 s has not been
  examined with a real module's latency.
- Still no watchdog. Plan item 17 pairs the WDT with link supervision, and a wedged AT thread is
  exactly the case supervision cannot catch by itself.
- The device still never publishes `wigwag/online = 1`, so its own Last Will has no positive
  counterpart.

---

## 2026-08-14 — SERCOM0 and the Zephyr transport: the AT client now runs on silicon

**Done**
- SERCOM0 enabled in `firmware/boards/pic32cm_pl10_cnano.overlay` — pinctrl group, `gclkperiph`
  channel and a `microchip,sercom-g1-uart` node — plus a `wigwag,module-uart` chosen node so the AT
  client never names a board-specific label.
- `firmware/src/rnwf_uart.{h,c}` — the Zephyr half of `struct rnwf_at_io`: interrupt-driven receive
  into a 256-byte static ring, drained in thread context; polled transmit.
- `rnwf_at.c` and `rnwf_uart.c` are now in `firmware/CMakeLists.txt`, and `main.c` runs the client
  alongside the lamp. The connect script executes on the real part.

**Pins, settled by elimination**
**PA04 = SERCOM0 PAD0 (TX), PA05 = PAD1 (RX)**, peripheral function C. Everything else was taken or
unwise: the debugger holds PB00/PB01 (CDC console), PA20 (SWDIO), PA31 (SWCLK), PB03 (SW0) and PA30
(RESET); PA24/PA25 are the 32.768 kHz crystal footprint; PB08/PB09 the touch button; and **PA08–PA15
are MVIO**, powered from VDDIO2 — a needless dependency for a UART. `txpo = 0`, `rxpo = 1`, matching
the board's own sercom1 console.

**Verified on hardware**
```
wigwag: at RESETTING (errors 0 timeouts 1 overruns 0)
wigwag: at BACKOFF   (errors 0 timeouts 2 overruns 0)
wigwag: at RESETTING (errors 0 timeouts 2 overruns 0)
```
With no module attached this is exactly right: `AT+RST`, 5 s for `+BOOT`, timeout, backoff, retry.
It proves on real silicon that SERCOM0 initialises, the ISR is wired, and the timeout/backoff path
works — `errors 0, overruns 0` throughout.

**Measured** — flash 17 816 B (29.0 %), **RAM 4 800 B of 8 KB (58.6 %)**. The 920 B the AT client
added is attributed exactly: `at_client` 624 B, `rnwf_uart.c` 268 B (256-byte ring plus indices),
44 B of UART driver state for both SERCOM instances. The whole application is ~900 B against
3 766 B of kernel, which remains the entire story of this budget.

**Fixed while here: D70, the breathe drift**
`main.c` now schedules on absolute deadlines (`k_sleep(K_TIMEOUT_ABS_MS(deadline))`) instead of
`k_msleep()`. A relative sleep times the gap *between* iterations, so every microsecond of render
and AT work was added to the period — the measured 1297 ms against an intended 1250 ms. Absolute
deadlines make the rate the clock's business rather than the workload's.

**Tried and rejected**
- **Polled receive.** Would have avoided `CONFIG_UART_INTERRUPT_DRIVEN` and the ring entirely. At
  115200 a byte arrives every 87 µs and the SERCOM has no deep FIFO, so a loop that also renders
  lamps would lose characters mid-line. Interrupt-driven receive with a counted-overrun ring
  instead; `overruns` is reported on the console precisely so "too slow" is visible rather than
  silently corrupting lines.
- **Enabling `porta`.** The instinct, since the UART pins are on port A. Unnecessary: the pinctrl
  driver builds its port address table with `MCHP_PORT_ADDR_OR_NONE`, which tests
  `DT_NODE_EXISTS` rather than status, so muxing works while the GPIO node stays disabled — and we
  avoid instantiating a device we never use. Worth knowing that the same macro means a *missing*
  port node would silently shift the table's indices.
- **A silent device.** The first integrated build printed nothing at all for 16 s and looked dead. It
  was correct — I had dropped the per-cycle heartbeat, and a client with no module retries in
  silence. Replaced with state-transition reporting, which is the right granularity: rare, and it
  distinguishes "backing off" from "crashed". A device whose only output is a lamp needs this.

**A wrong assumption, caught by the compiler**
`uart_irq_update()` returns **void** in this Zephyr version; I wrote it as a condition
(`if (!uart_irq_update(dev))`) from memory. `-Werror` turned it into "invalid use of void
expression" immediately, which is the argument for building the transport against the real headers
early rather than writing it blind.

**Then a USB-UART adapter arrived, and the whole path ran on silicon**

A passive listen settled the pin question in one shot — read the adapter with nothing else running:

```
bytes received: 8
repr: b'AT+RST\r\n'
```

Three things at once: bytes really do leave PA04, the adapter's RX was on the right pin, and **the
CR LF command termination taken from the specification is correct on the wire.**

Then the full chain, with `fake_rnwf02.py --port /dev/cu.usbserial-… --broker localhost`:

```
wigwag: module UART up, connecting to "wigwag-test"
wigwag: at RESETTING ... at SCRIPT ... link LINKED ... at READY (errors 0 timeouts 0 overruns 0)
wigwag: wigwag/state = {"state":"IDLE","reason":"start","sessions":0}
wigwag: wigwag/state = {"state":"BUSY","reason":"live","sessions":3}
wigwag: wigwag/state = {"state":"WAIT","reason":"live","sessions":3}
wigwag: wigwag/state = {"state":"ERROR","reason":"live","sessions":3}
```

- The entire connect script executes on the real part over real SERCOM0: `ATV3`, `+WSTAC` ×3,
  `AT+WSTA=1`, `+WSTAAIP`, `+MQTTC` ×4, `AT+MQTTLWT`, `AT+MQTTCONN`, `+MQTTCONNACK`, `AT+MQTTSUB`.
- **A retained state published before the device existed arrives the moment it subscribes** —
  ADR-0003's load-bearing claim, now proven on hardware rather than in a host loop.
- Live updates arrive intact, JSON commas and quotes and all.
- `errors 0 timeouts 0 overruns 0` throughout, so the 256-byte ring and the 10 ms drain cadence are
  comfortable at 115200.

**The important finding: the device does not notice a silent death**

With the device in `READY`, its module process was killed and a new state published. For fourteen
seconds the console printed **nothing** — no state change, no link report. The device sat in
`READY`, reporting `LINKED`, holding a stale `ERROR` on its lamp, with nothing on the other end of
the wire.

That is precisely the confidently-wrong display ADR-0007 exists to forbid, and Rule 4 with it. The
cause is structural rather than a bug: **the AT client learns of link loss only from asynchronous
event codes** — `+WSTALD`, `+WSTAERR`, a failed `+MQTTCONNACK`. Absence of bad news is treated as
good news, so a module that crashes, a pulled UART wire, or a broker that vanishes without the
module noticing all leave the client believing it is linked indefinitely.

D34 already requires "> 10 s without broker → amber flicker", so the requirement was written down;
what is missing is any *positive* liveness evidence. Recorded as **D75**, and it is now the first
job of `link.c` rather than a theoretical concern. Two complementary signals, both cheap:

- **Poll the module.** A bare `AT` every few seconds, requiring `OK` within a timeout. Catches a
  dead module, a dead UART, and a wedged AT interface — but says nothing about the broker.
- **Watch `wigwag/host_online`.** `CONTEXT.md` already defines it as the daemon's retained liveness
  marker with `0` as its Last Will. Subscribing to it catches a dead host or broker, which the
  module poll cannot see.

Neither alone is sufficient, which is the point worth remembering: the two failure domains are
independent, so liveness needs a signal in each.

**Tried and rejected: reconnecting the fake without tearing down the old session**

A confusing symptom cost time and is worth recording, because it looks exactly like a firmware bug.
After the device connected twice — once before a reset, once after — later publishes stopped
arriving, while the broker held them correctly and the device sat happily in `READY`.

`Broker.connect()` in the fake built a **new paho client per `AT+MQTTCONN`** without closing the
previous one. Both used the same MQTT `client_id`, and a broker resolves that by evicting whichever
connected first; paho's `loop_start()` then reconnects it, which evicts the second, and delivery
turns flaky. A real module has exactly one MQTT session. Fixed by tearing down the old client first.

The lesson is about test infrastructure: the fake produced a failure mode the real module cannot
have, and I nearly went looking for it in `rnwf_at.c`. Worth checking the fake's own assumptions
before suspecting the code under test.
- Credentials in `main.c` are placeholders; Kconfig from a gitignored `credentials.conf` is still to
  come (D56, D37).
- The lamp does not yet respond to the received state — that is `lamp.c`, and it is also where the
  renderer gets its own thread, since a long AT command currently blocks the breathe for up to
  24 ms.
- **The device never publishes.** `at_host.c` sends a retained `wigwag/online = 1` birth message;
  `main.c` does not, so the online topic is only ever written by the Last Will. Harmless today,
  wrong later — `1` on connect is half of what makes the will meaningful.
- Nothing has verified the Last Will actually fires from the device's own session. The fake registers
  it with paho (`will_set`), and killing the fake should publish `wigwag/online = 0`; that was not
  checked while the fake's session handling was still suspect.

---

## 2026-08-14 — The lamp was inverted, and two upstream bugs were hiding behind each other

Chasing SERCOM0 pin assignments in the Curiosity Nano user guide turned up something that
invalidated part of what I had already recorded as verified, so it took priority.

**Bug one: the cnano's LED0 is active low, and mainline says it is active high**
The board user guide is explicit — Table 4-1 lists LED0 as "active low" and §4.1 says "driving the
connected I/O line to GND will activate the LED". Mainline's `pic32cm_pl10_cnano.dts` declares
`gpios = <&portb 2 GPIO_ACTIVE_HIGH>`.

I had reported "LED0 breathes" as D49's verification without ever establishing polarity, because
**neither blinky nor a symmetric fade can distinguish it** — an inverted triangle is still a
triangle. What distinguishes it is the *cubic* gamma curve, which parks the duty cycle near zero
for most of the cycle: active-high should look mostly dark with a brief bright peak, active-low the
inverse. Observed on hardware: **mostly lit, with a brief dark dip.** Active low, confirmed, and
our lamp had been running inverted the whole time.

Consequence recorded as D72: the overlay now sets `PWM_POLARITY_INVERTED`, but **only for the dev
board** — the PCB drives its lamps through low-side N-FETs (D26, ADR-0009) where a high gate lights
the lamp, so `lamp.c` must read polarity from devicetree and never inherit the cnano's.

**Bug two, found because the fix did not work**
Setting `PWM_POLARITY_INVERTED` changed the generated devicetree —
`pwms = <&tcc0 0x2 0x1e8480 0x1>` — and then **pyOCD reported the freshly built image as
byte-identical to the one already on the device.** I dismissed that as a pyOCD reporting quirk. It
was the actual evidence: a devicetree change that cannot alter the binary is a devicetree change
nothing reads.

Root cause: Zephyr resolves PWM flags with

    #define DT_PWMS_FLAGS_BY_IDX(node_id, idx) DT_PHA_BY_IDX_OR(node_id, pwms, idx, flags, 0)

which looks for a cell named **`flags`** and **defaults to 0 when it is absent**. Mainline's
`microchip,{tc,tcc}-g1-pwm.yaml` name their third `pwm-cells` entry **`polarity`**. So
`PWM_DT_SPEC_GET()` silently discarded the polarity — no warning, no error, and
`PWM_POLARITY_INVERTED` in a `pwms` cell simply did nothing.

Checked rather than assumed that `flags` is the convention: **52 of the in-tree PWM bindings use
`channel, period, flags`.** These two Microchip bindings are the only outliers, so it is a naming
slip, not an interface choice. One-line fix in `firmware/patches/`, to be upstreamed — ADR-0006's
rung (b), the first time this project has needed it. `firmware/patches/README.md` documents
re-application, since `west update` reverts it silently.

**Verified**
- Before the patch: `wigwag: polarity flags 0x0 (normal, active-high lamp)`.
- After: `wigwag: polarity flags 0x1 (inverted, active-low lamp)`, and the flash write covered
  6 656 bytes across 13 sectors — the binary changed, where the devicetree-only change had not.

**The lesson worth keeping:** a devicetree property that is silently ignored is invisible. The
firmware now prints its resolved PWM flags at boot (D74), which is four lines of code and would
have caught this in seconds instead of via a flash-size anomaly.

**Also learned along the way**
- **`pyocd flash -e chip` faults on this part** — `FlashEraseChip` dies with `FAULT ACK` even with
  DFP 1.5.437, after erasing. It left the device blank and needed a sector-erase reflash to
  recover. Use `-e sector`, which is what the board's own `board.cmake` specifies anyway.
- The TCC driver only ever *sets* `DRVCTRL.INVENx` and never clears it, so polarity is effectively
  one-way per boot. Harmless for a fixed assignment; a trap for anything that flips polarity at
  runtime.
- SERCOM0 pin choice for the module UART is settled by elimination: the debugger holds PB00/PB01
  (CDC), PA20 (SWDIO), PA31 (SWCLK), PB03 (SW0), PA30 (RESET); PA24/PA25 are the crystal footprint;
  PB08/PB09 the touch button; and **PA08–PA15 are MVIO**, powered from VDDIO2. That leaves
  **PA04 = SERCOM0 PAD0 (TX), PA05 = PAD1 (RX)**, mux C — outside the MVIO domain and clear of
  everything else.

**Confirmed by eye, and it validates the gamma choice**
After the fix the lamp fades "very smooth in brightness change up and down, no prolonged time at any
brightness". That is the *correct* result and it is worth understanding why, because I predicted the
wrong thing.

I had expected correct polarity to look "mostly dim with a brief bright peak". That was reasoning
about how long the **duty cycle** sits near zero — which is true — while ignoring that perceived
brightness is roughly the cube root of duty. With `duty = level³` the two cancel: perceived
brightness tracks `level` almost linearly, so a triangle in `level` produces a triangle in
*perceived* brightness — an even fade with no dwell. That cancellation is the entire purpose of the
gamma correction, so predicting a skewed appearance contradicted the code I had just written.

The polarity conclusion never depended on it. That rests on the earlier observation, which is
airtight: with `flags = 0` the active level is HIGH, the gamma curve keeps duty near zero for most of
the cycle so the pin is LOW most of the time, and the lamp was observed **lit** most of the time.
Lit when low is active low.

So two things are now established rather than assumed: the lamp polarity, and that a cubic curve is
a good enough perceptual correction for a diffused lamp on this hardware — measured by eye instead of
taken from a table. `lamp.c` can keep the integer cube.

**Open**
- D49 stands as a PWM-enablement result — TCC0 works, 500 Hz on a scope — and its lamp evidence is
  now polarity-aware rather than polarity-blind.

---

## 2026-08-14 — End-to-end: the real AT client against a real broker, no hardware

**Done**
- `firmware/sim/fake_rnwf02.py` — AT server plus `paho-mqtt` bridge to a real broker. Serves either
  a PTY (host-only) or a serial port (for the cnano later). Framing is faithful to the spec,
  including the **leading CR on every AEC** and the rule that AECs queue behind a command in flight
  rather than interleaving with it.
- `firmware/sim/at_host.c` — POSIX runner that drives the **real `rnwf_at.c`** over the PTY, with
  `make -C firmware/tests sim`. This is the substitute for `native_sim` that ADR-0015 promised, and
  it works on macOS.
- Failure injection the real module will never do on command: `--no-connack`, `--connack-reason`,
  `--fail <PREFIX>`, `--drop-link-after`, `--slow`.

**Verified end to end, against a real `mosquitto`**
- Full bring-up: `RESETTING → SCRIPT → READY`, LWT registered, `wigwag/state` subscribed at QoS 1,
  and the birth message `wigwag/online = 1` published retained.
- **The JSON payload survives the round trip intact** —
  `{"state":"WAIT","reason":"permission_prompt","sessions":2}` arrives with every comma and quote in
  place. That was the specific risk in taking the payload as the tail after the fourth comma.
- **ADR-0003's load-bearing claim, now verified from the device side.** Published a retained
  `ERROR` state with *no device connected*, then started the client: it received the retained
  message immediately on subscribing, with no host involvement. This is the whole reason the
  transport is retained MQTT.
- **ADR-0007, as a sequence rather than an intention.** Injected a Wi-Fi drop: `+WSTALD` →
  `on_link(false)` → `BACKOFF` → reset → full script → `LINKED` again, and the retained state
  re-delivered on resubscribe. Three complete loss-and-recovery cycles in twelve seconds.
- **A withheld `+MQTTCONNACK` never produces a false `LINKED`.** The module answered `AT+MQTTCONN`
  with `OK` and then said nothing; the client timed out, backed off and retried, and `on_link(true)`
  was never called. That is exactly the "OK means accepted, not done" rule earning its keep — a
  client that advanced on `OK` would have lit a confident lamp with no broker behind it.

**A diagnostic that lied, and got fixed**
A healthy run reported `dropped=3`, which reads like packet loss. It was three `+WSTALU` events —
well-formed link-up notifications carrying a BSSID and channel we have no use for. Conflating "an
event we don't model" with "a malformed or oversized line" would have sent a future debugger chasing
nothing, so they are now separate counters: `aecs_ignored` and `lines_dropped`. Four bytes of RAM,
justified under Rule 5 by the fact that on this device the only other output is a lamp.

**Measured after the change** — 2 040 bytes of flash unchanged, `sizeof(struct rnwf_at)` now
**624 bytes** (0x270, up 4 for the counter). 82 host checks still pass.

**Open**
- The fake encodes *our model* of the module. It proves framing, sequencing, timeout and backoff,
  and it cannot settle what the spec leaves unsaid — the quote-escaping question stands until real
  hardware answers it.
- `paho-mqtt` 2.x needs `CallbackAPIVersion`; the fake now guards for it exactly as
  `host/wigwagd/publisher.py` already did. Worth noting that the convention was already in the repo
  — checking first was cheaper than inventing a second one.
- Still to do: the Zephyr UART adapter and SERCOM0 devicetree, which is the same pattern D49
  established for TCC0.

---

## 2026-08-14 — The AT client core, with host tests that need neither Zephyr nor hardware

**Done**
- `firmware/src/rnwf_at.{h,c}` — line assembler, request/response engine, AEC dispatch, connect
  state machine with capped exponential backoff, and publish. **No Zephyr headers**, no allocation,
  one bounded buffer per direction (D66, ADR-0002, Rule 5).
- `firmware/tests/{test_rnwf_at.c,Makefile}` — **82 checks, 0 failures**, plain clang, ~1 s to run.
  `-Wall -Wextra -Werror -Wshadow -Wconversion` plus AddressSanitizer and UBSan.

**Why the connect sequence is a script, not a state per command**
The sequence is linear — `ATV3 → WSTAC×3 → WSTA=1 → +WSTAAIP → MQTTC×n → MQTTLWT → MQTTCONN →
+MQTTCONNACK → MQTTSUB` — so it is expressed as an array of steps, each with a command builder and
an *optional AEC to await*. That makes the specification's most dangerous rule structural rather
than remembered: **`OK` means accepted, not done.** A step with `await_aec` set does not advance
when `OK` arrives; it keeps waiting on the same deadline. Encoding it once in the engine means no
future step can forget it.

Optional configuration falls out for free: a builder returning 0 means "skip me", which is how a
NULL username, a NULL password, an open network and a zero keep-alive are handled with no branching
in the state machine. Tested — the username and password commands must not appear at all when the
config omits them.

**Measured on the target** (`arm-zephyr-eabi-gcc -Os -mcpu=cortex-m0plus`, clean under `-Werror
-Wconversion`)
- **2 040 bytes of flash**, 0 data, 0 bss for the module itself.
- **`sizeof(struct rnwf_at)` = 620 bytes** — both buffers and all state.

ADR-0008's estimate allowed ~0.5 KB for "UART RX ring + AT line buffer" and ~0.5 KB for "MQTT
payload parse + lamp/link state". 620 bytes for the entire client sits inside that, which is the
first evidence that the 8 KB target is not merely survivable but comfortable.

**Two bugs the tests caught, one mine and one a bad test**
- **`field()` used `strrchr` to find a quoted field's closing quote.** For
  `+MQTTSUBRX:0,1,1,"wigwag/state","{...}"` that returned the *last* quote in the whole line, so
  the topic came out as `wigwag/state","{"state":"WAIT","sessions":2}`. `strchr` is correct: the
  closing quote is the next one, not the final one. Would have been invisible on a topic containing
  no quotes and fatal on the real payload.
- The prefix-collision test asserted `READY` immediately after `+MQTTCONNACK`, but the client still
  has to send `AT+MQTTSUB` and await *its* `OK`. That was the **test** being wrong about the
  protocol, and fixing it made the test better: it now asserts the subscribe was issued, then that
  `OK` completes the link.

**Verified by test, not by inspection**
- Leading-CR AEC framing works, including split byte-at-a-time across `feed()` calls.
- `+MQTTCONN` (connection state) is **not** mistaken for `+MQTTCONNACK`. Matching requires `:` or
  end-of-line after the name, so a shared prefix cannot satisfy a wait.
- A JSON payload containing both commas and double quotes survives intact.
- `ERROR:12` backs off — the `ATV3` form, since bare `ERROR` is not what we will see.
- `+MQTTCONNACK:0,130` (protocol error) backs off rather than proceeding as if connected.
- A boot timeout backs off, does not retry early, and re-sends `AT+RST` when it does.
- Backoff grows and holds its 30 s cap over 20 consecutive failures.
- `+WSTALD` while `READY` reports `on_link(false)` — Rule 4 and ADR-0007 in a test.
- Publish is refused unless `READY`, and emits the exact `AT+MQTTPUB=0,0,<retain>,...` text.
- An oversized line is dropped **whole** — never wrapped and parsed as two lines — and the
  assembler still works afterwards.
- Empty lines, non-`+` junk, unknown AECs and a malformed `+MQTTSUBRX` leave the client in `READY`.

**The find that may change the wire protocol**
`AT+MQTTLWT` gave the device a real Last Will, but the reverse direction has a problem worth
raising before it bites: **the specification does not say how the module escapes a double quote
inside a quoted AEC field.** Our `wigwag/state` payload is JSON — `{"state":"WAIT","sessions":2}` —
so it is full of them. The client sidesteps this by taking the payload as everything after the
fourth comma rather than parsing quotes, which is correct for any escaping scheme that does not
rewrite bytes. But if the module *does* escape or truncate, a JSON payload is the worst possible
choice for the one message the device must never misread.

Cheap insurance would be a payload with no quotes and no commas at all — `WAIT` or `WAIT:2` — which
costs nothing on the host side and removes the failure mode entirely. Not changed unilaterally:
`wigwag/state` is specified in `CONTEXT.md` and the host already publishes JSON. Flagged for a
decision, and it is answerable the moment the module arrives.

**Open**
- The Zephyr UART adapter and SERCOM0 devicetree work are not written yet, so the client has never
  run against anything but the test harness.
- `fake_rnwf02.py` still to come; it must be generated from `rnwf_at_cmds.h`'s vocabulary so the
  fake and the client cannot drift apart.
- `rnwf_at.c` is not yet in `firmware/CMakeLists.txt` — deliberately, since nothing calls it and a
  dead module would distort the footprint numbers recorded for the D49 spike.

**Next**
`fake_rnwf02.py` plus the two adapters — Zephyr UART for the target, POSIX for the host — then the
end-to-end run against a real broker.

---

## 2026-08-14 — The RNWF02 AT wire protocol, verified from the specification

**Done**
- `firmware/src/rnwf_at_cmds.h` — the module's entire wire vocabulary in one file, every string,
  parameter ID and length limit cited to the **AT Command Specification, Network Controller 3.1.0,
  Revision 58a15dc2, August 19 2025**. Nothing inferred, nothing borrowed from a sibling part.

**Why this took a detour**
The plan said to read the AT command reference before writing `fake_rnwf02.py`, because a fake that
mirrors invented syntax passes its own tests and proves nothing. That turned out to matter more
than expected: **the MCP documentation tools cannot see the AT specification at all.** Every query
for MQTT AT commands returns the **Harmony 3 C wrapper** — `SYS_RNWF_MQTT_SrvCtrl`,
`SYS_RNWF_MQTT_CONFIG` — which documents *semantics* but contains no wire text. The RNWF02
Application Developer's Guide §9 "AT Commands" is a stub that says to fetch a separate PDF from the
product page.

Two partial sources were genuinely useful and are worth remembering:
- **RNWF02 Supplemental User Guide v3.0.0** carries a real transcript (`AT+WSTAC=1,"SSID"` / `OK` /
  `+WSTALU:1,"AA:BB:CC:DD:EE:FF",1`), which established the framing before the spec was in hand.
- **The RNWF11 guide** documents the full AT reference inline, including `+WSTAC`'s parameter IDs.
  Tempting, and *not used*: assuming a sibling module's command set is a good way to encode a
  plausible fiction. Recorded here as the trap it was.

The specification PDF was supplied directly. `WebFetch` could not read it — the content is
FlateDecode streams — but it saves the binary to disk, so `pdftotext -layout` on the saved file
produced 10 531 lines of searchable text. Worth knowing as a general technique.

**Verified — the four framing details that would each have caused a bug**
1. **A command line is terminated by CR LF**, not CR alone.
2. **AECs carry a *leading* CR**: `<CR>+AECNAME:INFO<CR><LF>`, present "to clearly identify the
   start of the AEC". So the line assembler must treat a bare CR as a delimiter and tolerate empty
   lines instead of treating one as a malformed response.
3. **`ERROR` is not safe to match on.** The success/error text depends on the `ATV` verbosity level:
   level 0 is `0`/`1`, level 2 is `OK`/`ERROR`, level 3 adds `ERROR:<STATUS_CODE>`, levels 4–5 add
   prose. The default is unspecified, so the client sets **`ATV3`** first — machine-readable codes,
   no vendor prose to parse.
4. **A command can succeed and then fail.** "If a command requires longer to process, the success
   response indicates the command was accepted. Command processing continues asynchronously."
   Late failures arrive as `+CMDNAME:ERROR:<code>` — the spec's own example is `+SOCKBR:ERROR:4`.
   So `OK` means *accepted*, never *done*, and the state machine must not treat it as completion.

Also verified: **AECs are never sent during command execution**, but may arrive while the host is
mid-transmit. That removes the need to handle an AEC interleaved inside a response, which is a real
simplification for an 8 KB part.

**Verified — the commands wigwag actually needs**
`AT+RST`, `AT+GMR` (the D62 firmware check), `ATV3`; `AT+WSTAC=<ID>,<VAL>` with SSID=1,
SEC_TYPE=2 (3 = WPA2-Personal, 5 = WPA3-Personal), CREDENTIALS=3, then `AT+WSTA=1`;
`AT+MQTTC=<ID>,<VAL>` with BROKER_ADDR=1, BROKER_PORT=2, CLIENT_ID=3, USERNAME=4, PASSWORD=5,
KEEP_ALIVE=6, TLS_CONF=7, PROTO_VER=8; `AT+MQTTCONN=<CLEAN>`;
`AT+MQTTSUB=<TOPIC>,<MAX_QOS>`; `AT+MQTTPUB=<DUP>,<QOS>,<RETAIN>,<TOPIC>,<PAYLOAD>`.

AECs: `+BOOT`, `+WSTALU`, `+WSTAAIP` (IP assigned — **this**, not link-up, is the cue to start
MQTT), `+WSTALD`, `+WSTAERR`, `+MQTTCONNACK:<FLAGS>,<REASON>` (0 = success), and
`+MQTTSUBRX:<DUP>,<QOS>,<RETAIN>,<TOPIC>,<PAYLOAD>` — which is how `wigwag/state` arrives.

**Two findings that change the design rather than just informing it**
- **`AT+MQTTLWT=<QOS>,<RETAIN>,<TOPIC>,<PAYLOAD>` exists.** So the *device* can register its own
  MQTT Last Will, which is exactly what `wigwag/online` = `0` needs (CONTEXT.md). That was listed
  as a topic with no implementation path; it is now a single command issued before `AT+MQTTCONN`.
- **The spec's maximum field lengths size our static buffers**, replacing guesswork: SSID 32,
  credentials 128, broker address 64, client ID 48, username 128, password 256. The longest command
  we ever emit is therefore a password set at 273 bytes — a real number for Rule 5 instead of the
  plan's assumed 256-byte round figure.

**Open**
- `+MQTTCONN` also exists as an AEC ("Connection state") distinct from `+MQTTCONNACK`. Its field
  layout has not been read yet; the client will match `+MQTTCONNACK` and log anything else.
- Numeric mode exists (`MM:NN`, MQTT is module ID 8) and would shrink parsing further. Not used —
  verbose text is debuggable from a terminal, which is half of why ADR-0002 chose this module.
- The specification is a vendor PDF and is **not** committed to the repo; `rnwf_at_cmds.h` cites
  its revision so the source of every value is traceable without redistributing it.

**Next**
Write the AT core against this vocabulary: bounded line assembly tolerating the leading-CR AEC
framing, a request/response engine where `OK` means *accepted*, AEC dispatch, and the connect state
machine `reset → ATV3 → WSTAC → WSTA → +WSTAAIP → MQTTC → MQTTLWT → MQTTCONN → +MQTTCONNACK →
MQTTSUB` with backoff. Zephyr-free, so it unit-tests under plain clang on macOS (D66).

---

## 2026-08-14 — Phase 2 opened: Zephyr workspace, and **D49 passes on hardware**

**Done**
- Zephyr workspace from nothing: `west` 1.5.0 in a repo-root `uv` venv, `firmware/west.yml` as a
  pinned minimal manifest, SDK 1.0.1 `arm-zephyr-eabi` only. ADR-0014, D64/D65/D67.
- `firmware/` is now a real Zephyr application: `CMakeLists.txt`, `prj.conf`, `src/main.c`,
  `boards/pic32cm_pl10_cnano.overlay`, plus `firmware/README.md` as a reproducible runbook.
- **The D49 spike, in devicetree only — and it works on hardware.**
  `firmware/boards/pic32cm_pl10_cnano.overlay` creates TCC0, its generic-clock channel and its
  pinctrl group; `pwm_mchp_tcc_g1` binds. Mainline needed no patch, no Zephyr4Microchip fallback,
  no new driver code. **LED0 breathes steadily and the 500 Hz carrier was confirmed on an
  oscilloscope.** D49 is `settled`; TCC0 WO0/WO1/WO2 is safe to commit to the PCB.
- ADR-0014 (workspace topology), ADR-0015 (module simulation on hardware, not `native_sim`).
- `docs/PLAN.md`: D39 amended, D49 annotated, D64–D68 added, Phase 2 items 12/13/15 updated, and
  the footprint section now carries measurements instead of only estimates.

**Why**
- The plan's sequence put D49 first because it gates the PCB, and the board is on the desk.
- ADR-0006 said pin mainline deliberately, so the manifest carries an explicit SHA
  (`357467a011cd2557a1a3f0b4be83d817c4addc9b`) rather than a branch or a release tag.
- "Minimum Zephyr" was an explicit instruction, hence the 4-module `name-allowlist` and the
  single-toolchain SDK.

**The finding that reshaped Phase 2: `native_sim` cannot run on macOS**
Zephyr's POSIX-architecture documentation says it *"is known to **not** work on macOS due to
fundamental differences between macOS and other typical Unixes."* `native_sim` is a
POSIX-architecture board, so `docs/PLAN.md` step 15 and D39 — fake module over a PTY, no hardware
— were unbuildable on this machine. Found while planning, before any code was written, which is
the only reason it cost nothing.

Replacement (ADR-0015): the fake module runs on the Mac against the **real cnano** over SERCOM0,
the same peripheral the PCB uses for the module (D46), with the console left on `sercom1`. That is
strictly more faithful than the original plan — real SoC, real SERCOM driver, real 8 KB part — and
it exercises SERCOM0 before the layout commits to it. The AT core will be written free of Zephyr
headers so the parser and state machine still unit-test on any host under plain clang, which the
`native_sim` plan never provided.

**Verified**
- **`microchip,tcc-g1-pwm` binds to PL10.** `CONFIG_DT_HAS_MICROCHIP_TCC_G1_PWM_ENABLED=y`,
  `pwm_mchp_tcc_g1.c.obj` compiled, and `build/wigwag/zephyr/zephyr.dts` shows
  `/soc/tcc@42001800` resolved with every property attributed to the overlay.
- **The pin is right, decoded not assumed.** The resolved `pinmux = <0x5221>`; against
  `MCHP_PINMUX` in `mchp_pinctrl_pinmux_pic32c.h` (port bits 0–3, pin 4–8, func 9–11, mux 12–15)
  that is port b, pin 2, func `periph`, mux `f` — **PB02 / function F / TCC0 WO2**.
- **PB02 is both LED0 and TCC0 WO2** — datasheet §2.3 pinout and multiplexing, cross-checked
  against `PB2F_TCC0_WO2` in hal_microchip's generated pinctrl header. So the spike breathes the
  board's own LED with no jumpers, and does not collide with the console on PB00/PB01.
- **All four TCC values taken from the source of truth**, `hal_microchip`'s
  `pic32cm6408pl10048.h` and `component/tcc.h`: `TCC0_BASE_ADDRESS 0x42001800`, `TCC0_IRQn 12`,
  `TCC_CC[4]`, and `TCC_COUNT_Msk 0x0000FFFF`.
- **Both clock IDs already existed** for this family — `CLOCK_MCHP_GCLKPERIPH_ID_TCC0` (PCHCTRL11)
  and `CLOCK_MCHP_MCLKPERIPH_ID_APBC_TCC0` in `mchp_pic32cm_pl_clock.h`. Nothing upstream was
  missing except the nodes themselves.
- **The generic-clock channel must be declared in devicetree.**
  `clock_control_mchp_pic32cm_pl.c` does `DT_FOREACH_CHILD(DT_NODELABEL(gclkperiph), ...)` at
  init, so without a `tcc0_gclk` child the TCC binds cleanly, has its bus clock, and produces
  nothing. Read the driver rather than trusting the node to work.
- Blinky builds for `pic32cm_pl10_cnano`: flash 12 576 B, RAM 3 872 B.
- D49 spike builds: flash 14 132 B (23.0 %), **RAM 3 880 B of 8 KB (47.4 %)**.

**Verified on hardware** (`EV10P22A`, programmed over the on-board nEDBG)
- **LED0 breathes steadily, and the 500 Hz carrier was confirmed on an oscilloscope** — not
  inferred from the console. This is the D49 success criterion and it is met.
- **The clock assumption in the overlay was right, measured from the device**:
  `pwm_get_cycles_per_sec` returns **24 000 000**, exactly GCLK0 at 24 MHz with `prescaler = <1>`,
  so the 2 000 000 ns period is a real 500 Hz and the duty has 48 000 counts of resolution.
- Duty reaches the hardware across the full range: peak pulse 1 999 965 ns of 2 000 000 ns
  (`gamma_pulse(255)`), i.e. essentially 100 %, and the LED goes fully dark at the trough.
- Polarity is correct as `PWM_POLARITY_NORMAL` — the board's `GPIO_ACTIVE_HIGH` LED0 needed no
  inversion.
- `west flash` resets and runs the target: a capture immediately after flashing starts at
  `breathe cycle 1 at 1292 ms`.

**The breathe rate is 3.8 % slow, and the reason matters for `lamp.c`**
The device's own uptime puts consecutive cycles **1296–1297 ms** apart against an intended 1250 ms
(125 × 10 ms) — 0.771 Hz, not 0.800 Hz. Checked rather than guessed:
`CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000`, so a 0.1 ms tick and the usual one-tick round-up on a
relative timeout accounts for only ~0.1 %. The rest is the loop's own work — roughly 0.3 ms per
iteration, mostly `pwm_set_pulse_dt()` waiting on TCC `SYNCBUSY` across the clock-domain boundary,
plus the once-per-cycle `printk` amortised over 125 steps.

`k_msleep` measures the gap *between* iterations, so any work inside the loop is added to the
period and the error accumulates by construction. **`lamp.c` must schedule on absolute deadlines**
— `k_timer`, or `k_sleep(K_TIMEOUT_ABS_MS(...))` — so the rate is set by the clock and not by how
long a render takes. Recorded as D70. Left unfixed in the spike, whose job was TCC PWM, but the
code comment now states the measured figure rather than the intended one.

**The 8 KB answer, first real data (ADR-0008)**
`ram_report` attributes **3 766 of 3 878 B to `kernel/init.c`**: `z_interrupt_stacks` 2 048 B
(52.8 %), `z_main_stack` 1 024 B (26.4 %), `z_idle_stacks` 256 B, threads ~336 B. Every driver
combined — clock, PWM, serial, systick — is **66 B**.

So the estimate's shape was wrong in an encouraging way: it budgeted ~2.5 KB for "kernel + main
thread" and ~2.3 KB for three application threads, but the real budget is three tunable stack
sizes. A what-if build at 512/512/128 measures **1 704 B, 20.8 %** — 2 176 B recovered from
configuration alone. Not adopted: shrinking a stack without peak-usage evidence trades a number
for an overflow. That happens when the real threads exist, with `CONFIG_INIT_STACKS` to justify
each value.

Also noted, not acted on: `printk("%llu", cycles)` drags in `__l_vfprintf` (1 156 B) plus
`__aeabi_uldivmod` and `__udivmoddi4`. Flash is at 22 %, so it stays for now, but 64-bit formats
are not free on an M0+.

**Tried and rejected**
- **Copying the JH01 family's TCC node wholesale.** `pic32cm_5164_jh.dtsi` has exactly the node
  shape needed, and it is the right template — but three of its numbers are wrong for PL10: base
  `0x42002400` vs `0x42001800`, IRQ 17 vs 12, and `max-bit-width = <24>` where **PL10's TCC
  counter is 16-bit** (`TCC_COUNT_Msk == 0x0000FFFF`). The width one is the trap: it would have
  built, bound and run, then silently accepted periods the counter cannot represent. Recorded as
  D68.
- **A Linux VM for genuine `native_sim`.** The documented workaround, and it matches the original
  plan. Rejected because it is the most setup for the *least* faithful test now that the correct
  silicon is on the desk.
- **QEMU `mps2/an385` with uart1 on a TCP socket.** The strongest runner-up: native on macOS, real
  `arm-zephyr-eabi` build, CI-able with no hardware. Rejected because it is a different SoC —
  different UART driver, different clock tree, and `ram_report` numbers that say nothing about the
  8 KB question. Pre-analysed in ADR-0015 as the fallback if hardware testing proves insufficient.
- **Homebrew `gcc-arm-embedded`** (already installed) instead of the 1.4 GB SDK. Rejected: Zephyr's
  ARM builds expect `arm-zephyr-eabi` with its bundled picolibc, and `gnuarmemb` means newlib plus
  C-library Kconfig deviations — a poor trade for disk space.
- **Blobless clone (`--filter=blob:none`)** for a smaller fetch. Rejected: a build reads most of
  the tree, so the blobs arrive lazily one round-trip at a time, converting a one-off download
  into recurring build latency. `--narrow -o=--depth=1` instead.
- **A gamma lookup table, and `powf()`.** The table costs flash and the float costs a soft-float
  library. Cubing the level in 32-bit integer arithmetic tracks the eye closely enough for a
  diffused lamp; dividing the period before multiplying keeps every intermediate inside 32 bits.
- **Writing a throwaway blinky into `firmware/`.** Used `zephyr/samples/basic/blinky` to prove
  toolchain → build instead, so the repository's own app went straight to being the D49 spike.

**The expensive trap of the session: a stale pack index installs a DFP that cannot flash this part**
This burned real time and looked exactly like broken hardware, so it is worth the detail.

`pyocd pack install pic32cm6408pl10048` installed `Microchip.PIC32CM-PL_DFP` **1.4.418**, and
every connection attempt then failed identically:

```
E Error attempting to create component SCS: Memory transfer fault
  (SWD/JTAG communication failure (FAULT ACK)) @ 0xe000ed00-0xe000ed03
C Memory transfer fault (Error while running debug sequence 'ResetCatchSet' ...)
```

The debug port enumerates and then core debug space faults. Ruled out in order: SWD clock (50 kHz,
100 kHz and 1 MHz all identical), and `--connect=under-reset`, which made it *worse* — `No ACK` at
`DebugPortSetup`. `pyocd list` also shows the target with a `✖︎` even once the pack is installed,
which is a red herring; the pack was installed and the target was resolvable
(`pyocd pack find` → `Installed: True`).

The clue came from the *fallback* working: MPLAB IPE programmed the part first time and logged
`DFP Version Used : PIC32CM-PL_DFP,1.5.437` — a **newer pack than pyOCD had**.

**Root cause: `pyocd pack install` resolves versions from a locally cached index that it never
refreshes.** `~/Library/Application Support/cmsis-pack-manager/index.json` was dated **Jun 11**,
two months old, and the only cached descriptor was `Microchip.PIC32CM-PL_DFP.1.4.418.pdsc`. So
`pack install` behaved correctly and installed the newest version *it knew about*. It reported
`Downloading descriptors (001/001)`, which reads like an index refresh but is just that one pack's
descriptor.

`pyocd pack update` rebuilt the index (1 812 descriptors, 32 MB), after which `pack find` offered
**1.5.437**, `pack install` fetched it, and **plain `west flash` works with no options at all** —
connect, erase, program, reset, run.

So D25 (pyOCD runner) holds and is now verified on hardware; the whole episode was a stale cache.
Recorded as D69 and documented in `firmware/README.md`, because the failure mode gives no hint of
the cause.

**Worth being honest about:** the first diagnosis was that the public index did not serve a working
pack, and the first fix was to rezip MPLAB's unpacked 1.5.437 (pyOCD rejects a bare `.pdsc` —
`File is not a zip file`) and pass `--tool-opt=--pack=…`. That worked, but it was a workaround for
a misdiagnosis, and it would have left every future machine doing something strange and
unnecessary. The real question — *why did it install an old version when the index has the new
one?* — is what produced the one-line fix. The rezipped pack has been deleted.

Two smaller mechanical traps found alongside:
- **`west flash` invokes `pyocd` by name**, so installing it into `.venv` is not enough — `PATH`
  must include `.venv/bin`, or the runner reports `required program pyocd not found`.
- **The console port and the debug interface are one USB device.** With a serial capture open,
  `ipecmd` failed outright — `java.lang.RuntimeException: Comm error`, `Programming Target Failed`
  **mid-erase**, leaving the part partially programmed (recovered by reflashing with the port
  closed). pyOCD, tested afterwards, tolerates it but drops to 0.18 kB/s from 0.52 kB/s. Close the
  capture before flashing.

**MPLAB X / `ipecmd` is not a dependency — that is now a requirement (D71)**
`ipecmd` was used only to break the deadlock: it proved the board and probe were fine while pyOCD
failed, and its log line `DFP Version Used : …1.5.437` was the clue that identified the stale
index. Once pyOCD worked it stopped being needed, and the requirement is that the toolchain stays
`west` + Zephyr SDK + pyOCD with the DFP from the public CMSIS index — no vendor IDE.

Verified rather than assumed, because **every pyOCD flash until this point had reported
`programmed 0 bytes … identical`** — which only exercises the verify path, not erase-and-write.
Forced a real round trip with pyOCD alone: spike → blinky → spike, each step erasing and
programming 12 800 bytes, with the console confirming the right image ran each time
(`LED state: ON/OFF` for blinky, `breathe cycle 1` for the spike). So the no-vendor-IDE claim is
tested, not hoped for.

**A trap that will bite on a fresh machine**
**Zephyr SDK 1.0.1 has no macOS host tools.** `west sdk install` prints *"SKIPPED: macOS host
tools are not available yet"* and carries on, so the build silently depends on Homebrew's `cmake`,
`ninja`, `dtc` and `gperf`. It works here because those were already installed. On a clean Mac the
failure would look like a broken SDK rather than a missing prerequisite. Documented in
`firmware/README.md` and recorded as D67.

**A change made to the spike to make it observable**
The first version printed a banner at boot and then looped silently, which made "is it running?"
unanswerable: confirming it needs a reset, and the debugger cannot reset the target while the
console port is open on the same USB device (see the trap above). Added a one-line-per-cycle
heartbeat carrying uptime, carrier frequency, clock rate and peak pulse. That is what produced the
timing measurement above — the drift would otherwise have gone unnoticed until `lamp.c`. Cost:
80 bytes of flash, no RAM.

**Open**
- A 3.3 V USB-UART adapter is now a required bench item for the AT client (ADR-0015). Fallback if
  there isn't one: move the AT link to the CDC port and build with the console off — workable but
  blind.
- Whether 1.4.418 itself is broken for this part or merely incompatible with pyOCD's
  debug-sequence implementation is **not** diagnosed — 1.5.437 works, which was enough. A real
  unknown, but not worth chasing.
- Every flash now prints a `PIC32CM-JH_DFP … Overlapping memory regions` warning. **Benign**, and
  documented in `firmware/README.md` so it does not get mistaken for using the wrong pack: pyOCD
  has no part→pack lookup, so resolving `-t pic32cm6408pl10048` parses *every* installed pack and
  filters by part number afterwards (`populate_target()` → `get_installed_targets()`, `board.py`).
  Unrelated packs with malformed memory maps warn as they go past — that JH part's device-level
  `PERIPHERALS` encloses the family-level `HPB0/1/2` and `DIVAS`. The timestamps prove it is
  pre-connection: ~0.3 s, against ~1.0 s for `Loading … at 0x0c000000`.
- pyOCD never refreshes its pack index automatically, so this will recur silently the next time a
  part needs a DFP newer than the cache. `pack update` is cheap; it belongs in any setup runbook.
- Stack sizes untuned, on purpose. Needs `CONFIG_INIT_STACKS` evidence.
- The overlay is app-local. The proper home for the TCC0 node is
  `dts/arm/microchip/pic32c/pic32cm_pl/common/pic32cm_pl.dtsi` upstream, covering all four PL10
  packages — a genuine upstream contribution, and the ADR-0006 case for it is now strong since
  the change is purely additive devicetree.

**Next**
D49 is closed, so the PCB is unblocked on the lamp side. Next is `rnwf_at.c` with its Zephyr-free
core (D66) — but **read the RNWF02 AT command reference first**, via the Microchip MCP tools, so
`fake_rnwf02.py` mirrors the module's real syntax rather than an invented one. A fake that agrees
with an imagined protocol is worse than no fake, because it passes.

Alongside it, SERCOM0 needs the same devicetree treatment TCC0 just got — a pinctrl group and a
`gclkperiph` child — which is now a known quantity rather than a risk.

---

## 2026-08-14 — Phase 1 host software: daemon, CLI, hook client, 93 tests

Branch `phase1/host-software`.

**Done**
- `host/` is a **uv** project (`pyproject.toml`, `uv.lock`), Python 3.11+, one runtime dep
  (`paho-mqtt`) imported lazily so the pure logic and the whole test suite need nothing installed.
- `wigwagd`: loopback UDP listener → per-session store with TTL → priority aggregation →
  retained MQTT publish. Modules split so the core is I/O-free and clock-injected:
  `state.py`, `protocol.py`, `config.py`, `listener.py`, `publisher.py`, `daemon.py`, `paths.py`.
- `wigwag` CLI: `set`, `clear`, `status`, `watch`, `config` — the generic push API (D36).
- `host/hooks/wg-notify`: the hook client, shell + bash `/dev/udp`. **2.9 ms median, 3.7 ms p95.**
  Plus `wg-notify.ps1` for Windows without Git Bash.
- `host/settings.hooks.json` (hook wiring, exec form), `wigwag.example.toml`, `host/README.md`.
- **93 tests passing**, including 9 integration tests that run the *real* shell client over
  *real* loopback UDP against a live listener.
- ADR-0010 (cross-platform + UDP hook client), ADR-0011 (configurable broker, TLS policy).

**Why**
- Cross-platform and "local or outside MQTT server" were added as requirements, and both
  invalidated parts of the plan — see below.
- Config is layered defaults → TOML → env, so the zero-config case is a working local setup.
  TLS **infers on** for any non-loopback broker: changing one config line to a remote host must
  not silently start shipping credentials and session activity in the clear. Explicit plaintext
  is still allowed but warns loudly (Rule 4 applied to configuration).

**Tried and rejected**
- **AF_UNIX datagram socket** (the original plan's transport). Windows has no AF_UNIX *datagram*
  support, and bash's `/dev/udp` cannot address Unix sockets either — so it would also have
  ruled out the fast hook client. Replaced with loopback UDP.
- **`sh` + `nc`** (the original plan's client). `nc` is absent from Git Bash on Windows, and its
  flags differ across BSD netcat, GNU netcat and `ncat`. Replaced with bash `/dev/udp`, which
  needs no external binary at all.
- **Python hook client.** One implementation everywhere, but 30–50 ms of interpreter start-up on
  a script that runs on every tool call. Rejected on cost.
- **PowerShell as the primary Windows client.** 100–300 ms start-up, and unnecessary since Claude
  Code uses Git Bash by default. Demoted to a documented fallback.
- **Go/Rust compiled client.** Genuinely good — same ~3 ms, no runtime dep. Rejected because
  bash already hits that latency with zero build step, and a compiler toolchain is real cost in a
  repo already carrying Zephyr, KiCad and OpenSCAD. Pre-analysed as the fallback in ADR-0010.
- **A bare `2>/dev/null` on the `/dev/udp` redirect.** Does *not* suppress the shell's own
  redirection-failure message, so a stopped daemon leaked `connect: Operation not permitted` to
  stderr — which `SessionEnd` shows to the user. Fixed by wrapping the redirect in a subshell.
  Found by testing the daemon-down path, not by reading the script.
- **Coalescing on the full aggregate including `reason`.** Looked right, and the test suite caught
  it: `PreToolUse` and `PostToolUse` differ only in reason, so every tool call republished the
  retained message twice — precisely the burst coalescing exists to prevent. Now keyed on
  `state` + session count only (D54).
- **`PING` triggering a publish.** A liveness probe must not be able to cause broker traffic.

**Verified**
- Hook client latency: **2.9 ms median / 3.7 ms p95** over 30 runs. A test asserts median < 50 ms
  as a regression guard against reintroducing an interpreter.
- Rule 3 holds in every case tested: exit 0, empty stdout, empty stderr — with the daemon down,
  with junk on stdin (`""`, `not json`, `{}`, `{"session_id":}`), and with an unknown verb.
- `/bin/sh` on macOS is **bash 3.2.57**, so `/dev/udp` is available where hooks run.
- **No `CLAUDE_SESSION_ID` env var exists** — only `CLAUDE_PROJECT_DIR`, `CLAUDE_PLUGIN_ROOT`,
  `CLAUDE_PLUGIN_DATA`, `CLAUDE_EFFORT`, `CLAUDE_CODE_REMOTE`, `CLAUDE_CODE_BRIDGE_SESSION_ID`.
  So `session_id` must come from stdin JSON; that is why the client parses at all.
- Claude Code runs hooks under **bash on all platforms** (Git Bash on Windows, PowerShell only as
  fallback) — the fact the whole client design rests on.
- Live smoke test with `--dry-run`: two producers (a hook session and a `ci` CLI producer),
  `WAIT` correctly held while CI reported `BUSY`, fell back to `BUSY` when the session hit `Stop`,
  then to `IDLE` when CI was cleared. No duplicate publishes.

**Verified against a real broker**
Installed `mosquitto` via brew *without* registering it as a service (run on demand).

- Full publish path: `IDLE → BUSY → WAIT → IDLE` observed arriving at the broker via
  `mosquitto_sub -t 'wigwag/#' -v`, plus `host_online 1` on connect.
- **Retained messages behave as ADR-0003 requires** — the load-bearing claim of the whole
  transport choice. A *fresh* subscriber (i.e. the device booting after the fact) immediately
  receives `{"state":"WAIT",...}` with no host involvement, and repeatedly, not one-shot.
- **Last Will fires on an unclean death**: SIGKILL → `host_online 0`. Clean SIGTERM also
  publishes `0`.
- **The retained state survives the daemon's death**, which is exactly *why* the device must
  fail-visible on link loss (ADR-0007): the broker keeps serving a state whether or not anything
  is still producing it. Seeing that directly makes ADR-0007 feel less like caution and more like
  a requirement.
- **Hook block merged into `.claude/settings.json`, and this session now drives the light.**
  `wigwag status` shows this session's own id (`0fec7bb8-…`) as a live session in `BUSY` from
  `PreToolUse`. Real hooks → real bash client → real daemon → real broker.

**A test bug worth recording, since it nearly became a false bug report**
The first Last Will test showed `host_online 1` after SIGKILL, which looked like a broken will.
It was a flawed test: `kill -9 $!` killed the `uv run` *wrapper* and orphaned the Python child,
so the MQTT connection stayed open and the will correctly did not fire. `pgrep -fl` exposed the
survivor. Re-run against the real process, both SIGKILL and SIGTERM produce `0`.
Lesson: `uv run` is a wrapper — for signal-handling tests, exec the interpreter directly
(`.venv/bin/python -m wigwagd`) so `$!` is the process under test.

**Open**
- **`WAIT` has not been observed live** — it needs a real permission prompt. Covered by unit and
  integration tests, but not yet seen arriving from an actual `Notification` hook.
- **Windows is untested.** Portable by construction, but unrun. Stated in ADR-0010 rather than
  glossed over.
- Device-side TLS with Trust&Go remains future work.
- `mosquitto` runs on demand, not as a service, so the light only works while it is started.
  Worth revisiting once the device exists and this becomes daily-use rather than a test.

**Cross-platform operator documentation**
Rewrote `host/README.md` as a runbook covering macOS, Linux and Windows: prerequisites,
start-everything, broker install/run/service per platform, remote broker with TLS,
autostart at login, config reference, and a troubleshooting table. Root `README.md` gained a
getting-started section pointing at it. Added `host/deploy/` with a mosquitto config, a
launchd plist and a systemd user unit.

**The trap that would have cost hours later**
`mosquitto` 2.x with no config file **starts in "local only mode" and refuses every
connection not from the same machine.** Verified: a LAN publish to this host's own IP was
refused, and the broker log says so outright — *"Starting in local only mode… Create a
configuration file which defines a listener to allow remote access."*

This is insidious because it does not affect host development at all: `wigwagd` connects over
loopback and everything looks correct. It breaks only when the **device** tries to connect
over Wi-Fi, at which point the symptom is "the light never connects" with a working daemon.
Documented prominently in both READMEs and fixed by `deploy/mosquitto-wigwag.conf`.

Verified the fix rather than assuming it: `listener 1883` + `allow_anonymous true` → LAN
publish OK; `allow_anonymous false` + `password_file` → anonymous refused, authenticated OK;
and `wigwagd` connects to that authenticated broker over the LAN and publishes a retained
message. ADR-0011's plaintext warning fired for real in that last test, which was pleasing.

**Verified in the deploy files** — because shipping a config file is not the same as it working:
- `plutil -lint` clean on the launchd plist; the `sed` one-liner in the README leaves zero
  placeholders and the resulting interpreter path exists and runs.
- The systemd unit parses. Note `configparser` chokes on systemd's `%t` specifier unless
  interpolation is disabled — that was my *test script's* bug, not the unit's.
- Pinned `WIGWAG_STATUS_FILE=%t/wigwag/status.json` in the systemd unit. Without it the
  daemon derives the path from `XDG_RUNTIME_DIR`, and if that were unset it would fall back
  to a temp dir that `PrivateTmp=true` makes private to the service — so `wigwag status`
  would silently find nothing. `%t` is what the CLI computes, so both sides now agree.
- **Not** verified: the systemd unit on real Linux (`systemd-analyze` unavailable on macOS),
  and the Windows Task Scheduler steps. Both marked ⚠️ in a table in `host/README.md` rather
  than implied to work.

**Commissioning captured (ADR-0012) — it was only a passing line in the plan**
Raised as "how will we commission the wigwag — USB, Wi-Fi, Bluetooth, other?" The plan had one
line ("credentials via Kconfig for v1") and a vague future-directions note. It needed a real
decision, because it **constrains the PCB** and therefore had to be settled before Phase 3.

- **USB commissioning is impossible on this MCU.** PL10's peripheral summary (Table 8-1) lists
  SERCOM0/1, TC0/1/2, TCC0, ADC, AC, CCL, PTC, DMAC, EVSYS, RTC, WDT and so on — **no USB
  peripheral**. It would take an MCP2221A bridge, D+/D− routing, ESD, and reversing D24
  (USB-C power-only). Recorded as D57 because this is precisely the kind of thing that is free
  to decide now and a respin to discover later.
- **Bluetooth is out**: RNWF02 is Wi-Fi only (its PTA is for coexisting with an *external* BT
  radio) and PL10 has no radio.
- **RNWF02's provisioning support is much better than I assumed** — this is the find that made
  the decision easy. It has Soft-AP, an `AT+WPROV` **provisioning socket**, and a provisioning
  service that *"implements or handles all the required AT commands to start the module in
  Access Point mode and open up a TCP tunnel or serve a HTML web page to receive the Wi-Fi
  credentials."* Completion hands back `[Mode, SSID, Passphrase, Security, Autoenable]`. There
  is a **Microchip Wi-Fi Provisioning mobile app** and a `wifi_easy_config` reference demo.
  So the module serves the page and parses credentials; the host only sends AT commands — which
  is the only reason this fits in 8 KB.
- **Decision:** v1 compile-time Kconfig (fastest to a working light), v1.1 SoftAP provisioning
  triggered by a long-press on the button we already have, with all three lamps cycling so the
  mode cannot be confused with `WAIT` or the amber flicker.

**The distinction I nearly missed:** commissioning is *two* problems, not one — Wi-Fi credentials
**and** broker configuration. The module's provisioning service only knows about Wi-Fi. The broker
config has no path yet, so it stays compile-time even in v1.1 until the `AT+WPROV` socket is
extended. Logged as **D60, still open**, rather than quietly assumed solved.

**What this buys the PCB:** nothing new on the BOM. Break out the host UART (SERCOM0) to pads,
keep the button on an interrupt-capable GPIO, keep module `MCLR` under host control, keep the
`UART2_TX` debug pad. All pin assignments, all free now.

Noted but not binding: the EU **RED Delegated Act** has applied since 2025-08-01 to
network-connected radio equipment — no default passwords, secure credential storage,
authenticated updates. Microchip's own guidance says its RNWF02 reference apps ship with default
passwords that a product must remove. Irrelevant for a personal device, but commissioning is
where that work would land, and "provisioning AP with no password" would fail first.

**Broker auto-discovery investigated and rejected (ADR-0013), resolving D60**
Asked whether any auto-discovery mechanism exists for MQTT brokers. There is a real standard —
DNS-SD with registered service names `_mqtt._tcp` (1883) and `_secure-mqtt._tcp` (8883) — but
**neither end of this system speaks it**, and I verified both rather than assuming:

- **RNWF02 has no mDNS.** Network features are listed identically in the datasheet and the sell
  sheet: TCP, UDP, DHCP, ARP, HTTP, MQTT, IPv4/IPv6, TLS 1.2, DNS, SNTP. No mDNS, no Zeroconf.
- **mosquitto does not advertise.** Zero mDNS/avahi symbols in the installed binary, nothing in
  its usage output, and a live `dns-sd -B _mqtt._tcp local` browse on this LAN returned nothing.

Two naming collisions worth keeping straight, because both nearly misled me:
- Microchip's **Harmony 3 TCP/IP Library** *does* have `TCPIP_MDNS_ServiceRegister` and Zeroconf
  link-local support — but that is the host-side stack for parts like PIC32MZ that run their own
  IP stack. It is not RNWF02 module firmware and is not reachable over the AT interface.
- **Home Assistant "MQTT discovery"** is a device describing *itself* to HA once connected —
  not broker discovery. It is the thing people usually mean by "MQTT auto-discovery", and it is
  a plausible future feature, but it does not answer this question.

So mDNS would mean hand-rolling DNS-SD packet construction and parsing over a raw UDP socket in
8 KB, *plus* separately advertising the broker — to save typing one hostname.

**The find that settled it: `AT+CFGCP`.** Configuration Storage/Retrieval, added in RNWF02
firmware v3.0 — AT command configurations can be *"archived to non-volatile storage for later
retrieval… the commands re-played upon retrieving."* Since MQTT is configured *by* AT commands,
the module can persist **broker config, not just Wi-Fi**. Configuration becomes a once-ever
event, not once per boot, which removes most of the motivation for discovery. Recorded as D62,
with a note to verify the module ships firmware ≥ v3.0 at Phase 2 bring-up.

Decision: broker address is **entered during provisioning and persisted**, defaulting to a
**hostname rather than an IP** so it survives DHCP lease changes via router-registered local DNS.

Also rejected, with reasons in the ADR: a UDP broadcast beacon from `wigwagd` (trivially
spoofable, and it would make the device depend on a live host to find its broker — undermining
the independence retained messages buy), DHCP options, DNS SRV, fixed `.local` names, QR codes,
cloud rendezvous, and probing a candidate list. That last one is the worst option available: on a
shared network it could silently connect to someone else's broker, producing exactly the
confidently-wrong behaviour ADR-0007 exists to prevent.

**Next**
Phase 2 — starting with the **D49 TCC PWM spike**, which gates the PCB: get `pwm_mchp_tcc_g1`
bound to PL10 via devicetree and prove it with a breathing LED on an `EV10P22A`. Worth pairing it
with a provisioning-service spike on the same hardware, since both are RNWF02/Zephyr unknowns and
both feed the layout — and while there, record the module's firmware version to confirm
`AT+CFGCP` is available (D62).

---

## 2026-08-14 — Project scoped, named, and Phase 0 documentation built

**Done**
- Scaffolded the repo: `README.md`, `CONTEXT.md`, `CLAUDE.md`, `.gitignore`, directory tree for
  `host/`, `firmware/`, `hardware/`, `enclosure/`, `docs/adr/`. `git init` done, **nothing
  committed yet**.
- Built the reusable `journal` skill at `.claude/skills/journal/` — `SKILL.md`, three templates
  (`entry.md`, `adr.md`, `journal-header.md`), and `journal-reminder.sh`. Written with zero
  project specifics so it can be lifted to `~/.claude/skills/` (ADR-0005).
- Wrote ADR-0001 through ADR-0009.
- Mirrored the plan into `docs/PLAN.md` with a decision register (`D01`…`D51`).

**Why**
- Journaling first was an explicit instruction, so Phase 0 precedes any code.
- The name is deliberately vendor-neutral: Claude Code is the only producer today, but the design
  may serve other AI tools later, and nothing in the protocol or firmware is Claude-specific. A
  *wigwag* is the railroad grade-crossing signal with a swinging red lamp.
- Documentation split into journal + ADRs + `CONTEXT.md` + `CLAUDE.md` per ADR-0005. On
  terminology: there is **no** standard term for the chronological log — devlog, worklog and
  engineering journal are used interchangeably. **ADR is** the standard term, but only for the
  decisions subset. Adjacent conventions: "memory bank" (Cline), "handoff".

**Tried and rejected**
- **AVR64DU32 as the MCU.** Ideal on every other axis for a USB-tethered build — native
  crystal-less USB FS, bus-powered from 5 V VBUS via an internal 3.3 V USB VREG, no bridge chip.
  **Zephyr has no 8-bit AVR support**, so it was disqualified the moment Zephyr became a
  requirement. Whole design direction changed.
- **Zephyr's `winc1500` Wi-Fi driver.** The only in-tree Microchip Wi-Fi driver, and its Kconfig
  says it is **deprecated, scheduled for removal in Zephyr 4.6**, for lack of a maintainer. Dead
  end for a new 2026 design → ADR-0002 uses RNWF02 as a network co-processor instead.
- **WFI32E01 / PIC32MZ-W1** (single-chip Wi-Fi MCU, would collapse the BOM): MIPS, no Zephyr SoC
  support. **WBZ451 / PIC32CX-BZ2**: no upstream Zephyr SoC support either.
- **PIC32CM JH01 `PIC32CM5164JH01048`** as the target — first draft's choice. Superseded; see the
  error below. Retained as the 32-pin/16 KB escape hatch (D20).
- **Running the MCU at 5 V using MVIO.** Electrically fine and would delete three FETs, but no net
  BOM saving (the module needs 3.3 V regardless), extra Zephyr enablement work, pinout
  constraints, and `AVDD` is internally tied to `VDD` so VDD would be raw USB 5 V → ADR-0009.

**Verified**
- Zephyr Microchip board support — `boards/microchip/pic32c/` has ten boards including
  `pic32cm_pl10_cnano` and `pic32cm_jh01_cnano`; `soc/microchip/` has `{mec,miv,pic32c,pic64,sam,smartfusion2}`.
- `winc1500` deprecation — read directly from `drivers/wifi/winc1500/Kconfig.winc1500`. It is also
  the *only* Microchip entry in `drivers/wifi/`.
- **PL10 TCC/PWM** — datasheet §23.2: one TCC instance with NPWM, DPWM, dual-slope and critical
  PWM modes. Pinout table §2.3: TCC0 `WO0`–`WO3` reach the 28-pin package.
- **Mainline Zephyr PWM drivers for this silicon** — `drivers/pwm/` contains
  `pwm_mchp_tcc_g1.c`, `pwm_mchp_tc_g1.c` and `Kconfig.mchp`.
- **PL10 memory and packages** — `PIC32CM6408PL10` = 64 KB flash / 8 KB SRAM across 28/32/48/64
  pins. 28-pin available as SSOP (`-I/SS`, 1222 in stock), VQFN (`-E/3LW`, 2093) **and SPDIP
  through-hole (`-I/SP`, 285)**. A 32-bit M0+ in a DIP is a genuinely useful prototyping option.
- **PL10 electricals** (Table 37-1): VDD/VDDIO2 abs max −0.3 to +6.5 V; 50 mA max sink per I/O
  pin; 250 mA into VDD; 140 mA out of GND; 800 mW total. VOL/VOH characterized at 1.8/3.0/5.5 V.
- §3.2.3 — tie `VDDIO2` to `VDD` externally for single-supply mode. §2.2.1 — `AVDD` is internally
  connected to `VDD` on this part.
- **RNWF02** — ASCII AT commands over 2-wire UART; on-module TCP/IP, TLS 1.2, DHCP, DNS, WPA3 and
  **MQTT pub/sub**; so the host needs no network stack. Antenna rules: module at board edge,
  ground-plane edges aligned, ≥10 mm to plastic, ≥31.75 mm to metal.
- **Claude Code hook surface** — `Notification` has matchers `permission_prompt`, `idle_prompt`
  and `agent_needs_input`, which map exactly onto the `WAIT` lamp. `SessionEnd` hooks share a
  ~1.5 s budget. `UserPromptSubmit` stdout is injected into the model's context and `SessionStart`
  stdout is shown to the user — hence the never-write-to-stdout rule.
- Dev boards: `EV10P22A` (PL10 Curiosity Nano, `PIC32CM6408PL10048`, identical 64 K/8 K),
  `EV72E72A` (RNWF02 Add-on Board, 268 in stock).

**Mistake worth remembering**
I claimed PL10 had no PWM, and rejected it on that basis. The claim came from the Zephyr *board*
doc's supported-features table for `pic32cm_pl10_cnano`, which lists no PWM — but that table
describes only **what that board's port currently enables**, not what the silicon has and not
what drivers exist. The silicon has a full TCC; the driver has been in mainline all along.

Reviewer pushback — *"PL10 should be same TC and TCC peripherals as other SAM / PIC32C devices"* —
was correct, and checking the datasheet settled it in one query. The rule now in `CLAUDE.md`:
check **silicon / driver / board enablement** as three separate layers and say which you checked.
Only "no driver anywhere" is a blocker; a board-layer gap is devicetree work worth upstreaming.

This directly changed the hardware direction — from a 512 KB/64 KB, 48-pin part to a 64 KB/8 KB,
28-pin part, and turned footprint discipline into an explicit design goal (ADR-0008).

**Process note**
Lost ~a dozen inline plan annotations because VS Code's plan-review document is only live at the
approval gate — once implementation started, the buffer closed unsaved and the comments were
unrecoverable. Fix: the plan now lives in-repo at `docs/PLAN.md`, so annotations are durable and
diffable. The compact decision register (`D01`…`D51`) exists so a reviewer can comment per line
instead of hunting through prose.

**Open**
- **8 KB SRAM is unproven.** Estimate is ~6 KB. Must be measured with `ram_report` in Phase 2,
  before the PCB. Escape hatch is D20, on measured evidence only (ADR-0008).
- **D49 spike gates the PCB:** does `pwm_mchp_tcc_g1` bind to PL10 with devicetree work alone?
  Success = a breathing LED on an `EV10P22A`.
- TLS to the broker deferred; v1 is username/password on the LAN (ADR-0003). Trust&Go is on the
  chosen module variant so no respin is needed later.
- Hook block not yet installed into `.claude/settings.json`.

**Next**
Install and verify the `SessionEnd` journal reminder, then start Phase 1 — `wigwagd`, the
`wg-notify` hook client, and the `wigwag` CLI, all verifiable against `mosquitto_sub` with no
hardware present.
