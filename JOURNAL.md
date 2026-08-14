# Journal

Append-only development log. **Newest entry first.** Durable decisions live in
[`docs/adr/`](docs/adr/) and are referenced from here as `ADR-NNNN`.

Entries record what was done, why, and — importantly — what was tried and rejected.

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
