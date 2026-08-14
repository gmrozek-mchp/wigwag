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
└── boards/pic32cm_pl10_cnano.overlay     the D49 TCC0 PWM enablement
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
| D49 spike (this app, with the per-cycle heartbeat) | 14 132 B | 3 880 B | 47.4 % |
| D49 spike, stacks sized down (measurement only, not adopted) | 13 972 B | 1 704 B | 20.8 % |

**Almost all of it is kernel stacks, not application code.** `ram_report` attributes 3 766 B of
3 878 B to `kernel/init.c` — `z_interrupt_stacks` 2 048 B, `z_main_stack` 1 024 B,
`z_idle_stacks` 256 B — against 66 B for every driver combined. So the interesting number is not
today's 47 %, it is that the three stack sizes are the entire budget and they are all tunable.

They are *not* tuned yet: shrinking a stack without measuring peak usage trades a footprint
number for a stack overflow. Sizing happens once the real threads exist, with
`CONFIG_INIT_STACKS` / the thread analyzer used to justify each number.

## Notes for a Linux host

`native_sim` does not work on macOS — Zephyr's POSIX architecture is documented as not supported
there — so the module simulator runs against real hardware instead (ADR-0015). On Linux,
`native_sim` builds and is worth wiring up as a CI runner; nothing in the application is
board-specific except `boards/pic32cm_pl10_cnano.overlay`.
