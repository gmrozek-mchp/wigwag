# wigwag firmware

Zephyr application for the PIC32CM PL10 host MCU. The RNWF02 module owns Wi-Fi, TCP/IP, TLS and
MQTT (ADR-0002), so this firmware has no networking subsystem at all — it drives three lamps over
TCC0 PWM, reads one button, and speaks AT commands over a UART.

Target part `PIC32CM6408PL10028` (64 KB flash / **8 KB SRAM**). Development board `EV10P22A`
(PL10 Curiosity Nano), which carries the 48-pin sibling with identical memory, so footprint
measurements transfer directly (ADR-0008).

## Layout

```
firmware/
├── west.yml                              pinned, minimal manifest (ADR-0014)
├── CMakeLists.txt  prj.conf
├── src/main.c
├── src/*.c                               one subsystem per file; pure logic split from its
│                                         Zephyr adapter (lamp/lamp_pwm, button/button_gpio,
│                                         wdog/wdog_wdt) so the logic is host-testable
├── tests/                                host unit tests, plain clang: `make -C firmware/tests`
├── modules/pic32cm-pl-nvmctrl/           out-of-tree flash driver for PL10's NVMCTRL (ADR-0017);
│                                         registered via ZEPHYR_EXTRA_MODULES in CMakeLists.txt
└── boards/pic32cm_pl10_cnano.overlay     TCC0 PWM (D49), SERCOM0, WDT, RSTC, NVMCTRL, lamps, SW0
```

The west workspace top directory is the **repository root**, so `zephyr/`, `modules/`, `.west/`
and `.venv/` are siblings of `firmware/` and are gitignored (ADR-0014).

## One-time setup

Prerequisites from Homebrew: `cmake`, `ninja`, `dtc`, `gperf`, `uv`.
The Zephyr SDK has **no macOS host tools** in 1.0.1 — the installer skips them — so these
Homebrew versions are what the build actually uses.

```sh
cd <repo root>

uv venv .venv --python 3.13
uv pip install --python .venv west pyocd

.venv/bin/west init -l firmware
.venv/bin/west update --narrow -o=--depth=1        # ~1 GB: zephyr 622 MB, modules 409 MB
uv pip install --python .venv -r zephyr/scripts/requirements-base.txt

.venv/bin/west sdk install -t arm-zephyr-eabi      # 1.4 GB, installs to ~/zephyr-sdk-1.0.1
```

Only the ARM toolchain is installed, and the manifest fetches only `cmsis`, `cmsis_6`,
`hal_microchip` and `picolibc`. Adding a Zephyr subsystem later may require adding its module to
`west.yml`'s `name-allowlist`; the symptom is a CMake error, not a helpful message.

## Build, flash, measure

```sh
.venv/bin/west build -p -b pic32cm_pl10_cnano firmware -d build/wigwag
.venv/bin/west build -d build/wigwag -t ram_report       # Rule 5 — record these in JOURNAL.md
.venv/bin/west build -d build/wigwag -t rom_report

# One-time, and see the trap below — refresh the pack index before installing.
.venv/bin/pyocd pack update
.venv/bin/pyocd pack install pic32cm6408pl10048

PATH="$PWD/.venv/bin:$PATH" .venv/bin/west flash -d build/wigwag
```

`west flash` shells out to `pyocd` by name, so `.venv/bin` must be on `PATH` — installing pyOCD
into the venv is not enough.

Running from inside `firmware/` with the default build directory works too, and is shorter:
`west build -b pic32cm_pl10_cnano && west flash` (output lands in `firmware/build/`, gitignored).
The explicit `-d` form above just keeps several build trees side by side.

Console: the Curiosity Nano's debugger exposes a CDC serial port at 115200 8N1 on `sercom1`
(PB00/PB01). On macOS it appears as `/dev/cu.usbmodem*`.

The build banner is worth a glance — `Zephyr version: 4.4.99 (…), build: 357467a011cd` is the
pinned revision from `west.yml`, so a surprise there means the pin moved.

### ⚠️ Run `pyocd pack update` first, or you get a DFP that cannot flash this part

**`pyocd pack install` resolves versions from a locally cached index that it never refreshes on
its own.** If that cache predates the pack you need, it silently installs the newest version *it
knows about* — here `Microchip.PIC32CM-PL_DFP` **1.4.418** from a two-month-old `index.json` — and
every pyOCD connection then fails identically:

```
E Error attempting to create component SCS: Memory transfer fault
  (SWD/JTAG communication failure (FAULT ACK)) @ 0xe000ed00-0xe000ed03
C Memory transfer fault (Error while running debug sequence 'ResetCatchSet' ...)
```

The debug port enumerates, then core debug space faults. Unaffected by SWD clock (50 kHz, 100 kHz
and 1 MHz all identical), and `--connect=under-reset` makes it worse (`No ACK` at
`DebugPortSetup`). It looks exactly like broken hardware or a dead board, and is neither.

`pyocd pack update` rebuilds the index (~1 800 descriptors, ~32 MB), after which
`pyocd pack install pic32cm6408pl10048` fetches **1.5.437** and `west flash` works with no extra
options. `pyocd pack find pic32cm6408pl10048` shows which version is on offer and whether it is
installed — check it before believing a connection failure.

Note also that `pyocd list` prints the target with a `✖︎` even when the correct pack *is*
installed. That is a red herring; trust `pack find`.

### The `PIC32CM-JH_DFP` warning on every flash is benign

```
W Overlapping memory regions in file …/PIC32CM-JH_DFP/1.7.296.pack (PIC32CM3204JH00032);
  deleting outer region. Further warnings will be suppressed for this file.
```

Nothing to do with this project, and not a sign the wrong pack is in use. To resolve
`-t pic32cm6408pl10048`, pyOCD has no part→pack lookup, so `ManagedPacks.populate_target()` →
`get_installed_targets()` parses **every** pack in the managed store and only then filters by part
number. Any unrelated pack with a malformed memory map warns as it goes past — here a JH part
whose device-level `PERIPHERALS` (`0x40000000`, 512 MB) encloses the family-level `HPB0/1/2` and
`DIVAS` regions, and whose family-level `PPB` encloses `SCS`. pyOCD keeps the finer regions,
deletes the enclosing one, and continues.

Confirm it is harmless by the timestamps: the warning lands at ~0.3 s, well before
`Loading … at 0x0c000000`, so it precedes any contact with the probe. Six PIC32CM DFPs happen to
be cached on this machine (GV, JH, LE, LS, MC, PL) from unrelated work. There is no per-pack
uninstall — `pyocd pack clean` removes the index and *all* packs — so the noise is best left alone.

### pyOCD is the only supported flashing path

**Requirement: nothing in this project may depend on MPLAB X or `ipecmd`.** The toolchain is
`west` + Zephyr SDK + pyOCD, all installable from the commands in this file, with the DFP fetched
by pyOCD itself from the public CMSIS index. A working setup must need no vendor IDE installed.

Verified as a genuine round trip, pyOCD only, no `ipecmd` and no `--tool-opt`:

| Step | Result |
|---|---|
| `west flash -d build/wigwag` | erased/programmed 12 800 B, spike runs from `breathe cycle 1` |
| `west flash -d build/blinky` | erased/programmed 12 800 B, `LED state: ON/OFF` on the console |
| `west flash -d build/wigwag` | erased/programmed 12 800 B, breathing resumes |

The middle step matters: repeat flashes of an unchanged image report
`programmed 0 bytes … identical`, which proves only the *verify* path. Flashing a genuinely
different image is what proves erase-and-write.

**Prefer to close the console before flashing.** The nEDBG's CDC and debug interfaces are one USB
device. pyOCD tolerates a serial capture being open but slows to 0.18 kB/s from 0.52 kB/s (71 s
against 28 s). The board's `board.cmake` also offers an `mplab_ipe` runner; it is **not** used
here, and with a capture open it fails outright — `java.lang.RuntimeException: Comm error`
mid-erase, which can leave the part partially programmed.

## The module UART

The RNWF02 lives on **SERCOM0**, the same peripheral the PCB uses (D46), reached through the
`wigwag,module-uart` chosen node. On the Curiosity Nano:

| Signal | Pin | Pinmux |
|---|---|---|
| TX (SERCOM0 PAD0) | **PA04** | `PA4C_SERCOM0_PAD0` |
| RX (SERCOM0 PAD1) | **PA05** | `PA5C_SERCOM0_PAD1` |

Chosen by elimination: the debugger holds PB00/PB01 (the CDC console), PA20 (SWDIO), PA31 (SWCLK),
PB03 (SW0) and PA30 (RESET); PA24/PA25 are the crystal footprint; PB08/PB09 the touch button; and
PA08–PA15 sit in the MVIO domain fed from VDDIO2.

With nothing attached, the client behaves exactly as it should — and says so:

```
wigwag: at RESETTING (errors 0 timeouts 1 overruns 0)
wigwag: at BACKOFF   (errors 0 timeouts 2 overruns 0)
```

It sends `AT+RST`, waits 5 s for `+BOOT`, times out, backs off and retries. `overruns 0` means the
receive ring is keeping up; a non-zero count would mean the poll loop is too slow.

To drive it for real, wire a 3.3 V USB-UART adapter to PA04/PA05 (TX→RX crossed, common ground) and
run the fake module against it:

```sh
.venv/bin/python firmware/sim/fake_rnwf02.py --port /dev/cu.usbserial-XXXX --broker localhost
```

**Do not** use a 5 V adapter: PA04/PA05 are on the VDD domain at 3.3 V.

## The watchdog

Armed at the end of `main()` with a 2 s window, and fed from the AT loop **only when both the AT loop
and the render thread have checked in within 500 ms** (ADR-0016). Two things this changes for anyone
working on the firmware:

- **Any task that blocks for more than 500 ms will reboot the device.** That includes a `k_msleep()`
  added while debugging, or a long busy-wait in either loop. The console says which task went quiet
  before the reset, and the next boot prints `RESET BY WATCHDOG (rcause 10)`.
- **Halting in a debugger is safe.** The WDT pauses while the core is halted, so breakpoints and
  single-stepping do not cause resets.

Adding a task whose liveness the lamps depend on means adding it to `enum wdog_task` **and** calling
`wdog_beat()` from it. Adding the enum entry alone stops the device feeding — the safe direction, but
it presents as a reboot loop.

To see the mechanism work, wedge the render thread deliberately: add `if (k_uptime_get() > 15000) {
k_sleep(K_FOREVER); }` after the `wdog_beat()` call in `lamp_thread()`, flash, and watch the device
refuse to feed and reboot every ~18 s. **Remove it afterwards.**

`CONFIG_HWINFO` is deliberately off: its Microchip driver misreads this part's `RCAUSE`, so
`wdog_wdt.c` reads the register directly (D95, and bug 4 in `docs/upstreaming-to-zephyr.md`).

## Flash

`firmware/modules/pic32cm-pl-nvmctrl/` provides self-programming, because mainline has no flash driver
for this family and its two Microchip drivers target other peripheral revisions (ADR-0017). Two things
to know before using it:

- **Erase stalls the CPU, interrupts included** — 10.1 ms per 512-byte page, measured. That is fine
  against the watchdog's 500 ms (ADR-0016), but it caps a single `flash_erase()` call at about **49
  pages (~24 KB)**. Erase more than that in one call and the device reboots mid-erase.
- **`FIXED_PARTITION_DEVICE()` does not work on this family** — PL10 declares `flash0` directly under
  `/soc`, so partition-to-device resolution finds `/soc`. Use the device explicitly with the
  offset from devicetree:

```c
const struct device *fl = DEVICE_DT_GET(DT_NODELABEL(nvmctrl));
off_t off = PARTITION_OFFSET(storage_partition);   /* 0xf000, 4 KB, 8 pages */
```

## The console

Commands arrive on the same UART the console prints on, `\r\n` terminated (ADR-0019). This is the
complete set — the device's own `help` prints the same list.

| Command | Effect |
|---|---|
| `help` (or `?`) | list the commands |
| `show` | print every setting; secrets shown only as `<set>`/`<unset>` |
| `set <key> <value>` | stage one setting — see the keys below. Not stored until `save` |
| `save` | persist the staged settings to flash |
| `clear` | forget everything stored; reverts to build-time defaults |
| `reboot` | restart, which is how Wi-Fi and `transport` changes take effect |
| `state IDLE\|BUSY\|WAIT\|ERROR` | drive the lamps. Counts as host activity |
| `brightness <0-255>` | master brightness, applies immediately, persists on `save` |
| `gain green\|yellow\|red <0-255>` | per-lamp calibration, applies immediately, persists on `save` |
| `echo on\|off` | stop echoing input — what a host program wants |
| `host on\|off` | host liveness. `on` must repeat within 10 s to keep the wire trusted; `off` is an orderly goodbye |
| `test wifi` | try the stored Wi-Fi and broker settings **without committing to them** — see below |

Keys for `set`:

| Key | Value | Notes |
|---|---|---|
| `transport` | `usb` or `wifi` | **which side owns the lamps.** Reboot to apply. Defaults to `usb` |
| `ssid` | up to 32 chars | |
| `pass` | up to 63 chars | **may contain spaces** — the rest of the line is taken verbatim. Never printed back |
| `sec` | 0–6 | `enum rnwf_sec_type`; 0 is open, 3 is WPA2 mixed personal |
| `broker` | hostname, up to 64 chars | a hostname by preference (ADR-0013) |
| `port` | 1–65535 | |
| `client` | up to 32 chars | MQTT client id |
| `user` | up to 32 chars | empty means no broker authentication |
| `mqttpass` | up to 64 chars | never printed back |

Things worth knowing:

- **Only `host` and `state` count as host activity.** Configuration commands deliberately do not, so
  someone setting up Wi-Fi over the console cannot be mistaken for a daemon driving the display
  (D117 → ADR-0022).
- **Over-long input is refused, not truncated** — `line too long, ignored` rather than silently storing
  a wrong credential.
- **Backspace works; arrow keys do nothing.** There is no history and no tab completion; Zephyr's shell
  does not fit on this part (ADR-0019).
- **`clear` does not reset `transport`** to anything other than the build-time default, which is `usb`.

Line editing is deliberately minimal, and `lineedit.c` is isolated behind a "bytes in, lines out"
interface so a richer CLI could replace that one file if it ever earned the RAM.

### Testing Wi-Fi before committing to it

`test wifi` runs the real connect script against the stored settings on a device that is still wired,
narrating each step, then leaves the transport setting and the lamps exactly as they were:

```
test: trying ssid "my-network" broker mqtt.example.lan:1883
test:   associate and get an IP
test:   resolve, connect and CONNACK
test: FAIL at "resolve, connect and CONNACK" — the module rejected it
test:   check the setting that step configures, then try again
```

Naming the step is the whole point: a wrong passphrase fails at *associate and get an IP*, a wrong
broker at *resolve, connect and CONNACK*, and a miswired module at *module responding*. Those are three
different problems that otherwise look identical.

It brings the module up on demand — a wired device never starts it otherwise (D118) — and runs
asynchronously, so the console stays responsive and the watchdog keeps being fed rather than a
30-second command starving it. States arriving from the broker during a test are **ignored**, and say
so, because a diagnostic must not change the display.

### Which transport owns the lamps

`transport` is the setting that decides what the device is (ADR-0022), and **nothing the outside world
does can change it**: no amount of traffic on the console takes the lamps from a wireless device, and no
Wi-Fi link takes them from a wired one. A configured transport that is not working shows the
fail-visible pattern rather than quietly answering with the other one — the two do not report the same
machine's work.

A fresh device defaults to `usb`, because that is the transport needing no configuration (D120). So:

```
# wired, out of the box: nothing to do but run the daemon
WIGWAG_SERIAL_PORT=auto wigwagd

# wireless
set transport wifi
set ssid MyNetwork
set pass correct horse battery staple
set broker mqtt.example.lan
save
reboot
```

Setting an SSID while `transport` is `usb` prints a note saying the network will not be used, and the
boot banner repeats it — the one easy mistake this design makes, called out where it happens.

## Moving the Zephyr pin

`firmware/west.yml` pins mainline to an explicit commit, deliberately, never floating (ADR-0006).
To move it: edit `revision:`, re-run `west update`, rebuild, re-measure `ram_report`, and record
both the old and new SHA in `JOURNAL.md`.

## Footprint

The 8 KB budget is a gated requirement, not an aspiration (ADR-0008, Rule 5). Measured for the
D49 spike:

| Build | Flash | RAM | of 8 KB |
|---|---|---|---|
| `samples/basic/blinky` | 12 576 B | 3 872 B | 47.3 % |
| D49 spike (lamp only) | 14 132 B | 3 880 B | 47.4 % |
| lamp + AT client + SERCOM0 transport | 17 816 B | 4 800 B | 58.6 % |
| **three lamps + link supervision, ISR stack tuned** | **19 708 B** | **4 440 B** | **54.2 %** |

The AT client's 920 B is attributed exactly: `at_client` (`struct rnwf_at`) 624 B, `rnwf_uart.c`
268 B (a 256-byte receive ring plus indices), and 44 B of UART driver state for both SERCOM
instances. The lamp renderer adds ~632 B (a 512 B thread stack plus its thread struct) and link
supervision 32 B. RAM went *down* between those last two rows despite gaining a thread, because the
ISR stack was tuned on measurement at the same time.

**Almost all of it is kernel stacks, not application code.** `ram_report` attributes 3 766 B of
3 878 B to `kernel/init.c` — `z_interrupt_stacks` 2 048 B, `z_main_stack` 1 024 B,
`z_idle_stacks` 256 B — against 66 B for every driver combined. So the interesting number is not
today's 47 %, it is that the three stack sizes are the entire budget and they are all tunable.

### Measuring stack utilisation

Stack sizes are the one part of this budget that is easy to guess and expensive to guess wrong, so
they are measured. `firmware/prj_stacks.conf` is a measurement-only overlay:

```sh
.venv/bin/west build -p -b pic32cm_pl10_cnano firmware -d build/stacks \
    -- -DEXTRA_CONF_FILE=prj_stacks.conf
.venv/bin/west flash -d build/stacks
# then exercise the device hard, and read the console
```

`CONFIG_INIT_STACKS` fills every stack with `0xaa` before use — threads *and* the interrupt stack —
and `CONFIG_THREAD_ANALYZER` walks each one to find where the pattern stops, printing every 10 s:

```
lamp            : STACK: unused  164 usage  348 /  512 ( 67 %); CPU:   3 %
idle            : STACK: unused  164 usage   92 /  256 ( 35 %); CPU:  93 %
main            : STACK: unused  164 usage  860 / 1024 ( 83 %)
ISR0            : STACK: unused 1784 usage  264 / 2048 ( 12 %)
```

**The number is only as good as the exercise.** A peak happens during the deepest call chain that
actually ran, so anything not provoked is not measured. Cover at minimum: the AT connect script, all
four lamp behaviours, an unparseable payload, a link loss and recovery, and a keepalive timeout with
a full reconnect.

**Sanity-check the report before believing it.** The first run showed `unused 164` for three
different stacks, which looked like a bug. Raising one stack from 512 to 768 moved `unused` to 420
while `usage` stayed at 348 — so the report was honest and the coincidence real. Cheap experiment,
worth repeating if a number looks too neat.

Two results worth keeping:

- **The ISR stack was 87 % idle** — 264 B used of 2048. Now `CONFIG_ISR_STACK_SIZE=1024` in
  `prj.conf`, still 4× the measurement, returning 1 KB.
- **`main` is the tight one at 860 of 1024 (84 %)**, not the thread whose size was guessed. It runs
  `printk` formatting and the AT script's `vsnprintf`. Do not shrink it; if credentials with a
  password are configured, `vsnprintf` builds a longer command and this should be re-measured.

There is **no MPU on PIC32CM PL10**, so `CONFIG_HW_STACK_PROTECTION` is unavailable.
`CONFIG_STACK_SENTINEL` is the software substitute and is enabled in the measurement overlay; it
detects an overflow after the fact rather than preventing it.

## Notes for a Linux host

`native_sim` does not work on macOS — Zephyr's POSIX architecture is documented as not supported
there — so the module simulator runs against real hardware instead (ADR-0015). On Linux,
`native_sim` builds and is worth wiring up as a CI runner; nothing in the application is
board-specific except `boards/pic32cm_pl10_cnano.overlay`.
