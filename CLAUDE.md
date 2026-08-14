# wigwag — instructions for AI-assisted sessions

A Wi-Fi desk stoplight showing AI coding session state, on Microchip silicon, running Zephyr.
Read `CONTEXT.md` for the vocabulary before writing code or docs, and `docs/PLAN.md` for the
plan and its numbered decision register. `docs/adr/` holds the reasoning behind each durable
decision; `JOURNAL.md` (newest entry first) is where the project stands right now, including
what was tried and rejected.

**Orienting in a fresh session:** read the top entry of `JOURNAL.md`, then the decision
register in `docs/PLAN.md`, then only the ADRs relevant to what you are about to touch.

## Rule 1 — keep the journal

**Every working session appends an entry to `JOURNAL.md` before it ends.** This is the
project's first rule, not an afterthought. Use the `/journal` skill; it has the template and
the rules for when something graduates to an ADR.

Record what was *tried and failed*, not just what worked. The dead ends are the expensive
knowledge — a future session that doesn't know them will pay for them again.

## Rule 2 — decisions become ADRs

When a choice is durable, constrains future work, or you'd be annoyed to have to re-derive
it, write `docs/adr/NNNN-short-title.md`. Do not bury decisions in the journal; the journal
is chronological and unsearchable by intent. ADRs are addressable — cite them as `ADR-0003`.

Never rewrite the decision in an accepted ADR. Supersede it with a new one and mark the old
one `Superseded by ADR-NNNN`.

## Rule 3 — never break Claude Code

`host/hooks/cg-notify` runs inside Claude Code's own hook path. It must:

- always `exit 0`, even on total failure;
- write **nothing** to stdout — on `UserPromptSubmit` stdout is injected into Claude's
  context, and on `SessionStart` it is displayed to the user;
- work correctly when the daemon is down and the broker is missing;
- stay under ~10 ms. No Python, no Node, no network calls in the hook path.

A status light is a convenience. Breaking the tool it observes is never an acceptable
trade for a nicer light.

## Rule 4 — fail-visible, never lie

If the device can't confirm the current state, it must look obviously wrong (amber flicker)
rather than confidently display a stale one. See ADR-0007. This applies to the host too:
prefer surfacing "I don't know" over a plausible guess.

## Rule 5 — respect the 8 KB footprint budget

The target part has **8 KB of SRAM**, chosen deliberately (ADR-0008). That makes footprint a
gated requirement, not an aspiration:

- No dynamic allocation anywhere in the AT path. Bounded static buffers only, one per direction.
- Measure, don't assume: `west build -t ram_report` and `rom_report`, with the numbers recorded
  in the journal at each milestone.
- Before adding a Kconfig option, subsystem, or thread, state what it costs in RAM.
- If the budget is genuinely exceeded, the escape hatch is the larger part (D20) — but only on
  *measured* evidence, never on a hunch.

## Rule 6 — never commit without explicit permission

**Do not run `git commit` or `git push` unless explicitly told to in that message.**

A question about committing is not permission. "Ready for the initial commit?" is a request for
a status report, not an instruction — answer it and stop. Permission never carries forward from
one commit to the next, and approving a plan or its results never implies approval to commit.

Finish the work, then report what *would* be committed: the file list, a proposed message, and
confirmation the tree is clean. Then wait. Amend, rebase, reset and force-push are held to the
same rule and deserve more caution, since they rewrite history that already exists.

## Verify against real sources

This project sits on fast-moving vendor and RTOS surfaces where memory is unreliable:

- **Microchip parts** — use the Microchip MCP tools (`search_products`,
  `get_full_product_profile`, `search_datasheet`) for part numbers, memory sizes, packages,
  stock, and electrical limits. Do not recall them from memory; several near-identical part
  numbers differ in RAM.
- **Zephyr** — check `docs.zephyrproject.org` and the actual source tree for board support
  and devicetree bindings. Supported-feature tables vary a lot between sibling boards, and
  drivers get deprecated (see ADR-0002).
- **Claude Code hooks** — check `code.claude.com/docs/en/hooks` for event names, matchers,
  and stdin fields before changing the hook wiring.

Record what you verified in the journal, with the number or table you relied on.

### Distinguish three separate layers before claiming something is unsupported

This cost a wrong hardware decision once already (see ADR-0001 and the 2026-08-13 journal
entry). A Zephyr **board** doc's supported-features table says only what *that board's port*
currently enables. It does not tell you what the silicon has, nor what drivers exist:

| Layer | Where to check |
|---|---|
| Does the silicon have the peripheral? | the device datasheet |
| Does a Zephyr driver exist? | `drivers/<class>/` in the source tree, and the DT bindings index |
| Is it enabled for this board? | the board's devicetree and its supported-features table |

Only "no driver anywhere" is a real blocker. A gap at the board layer is devicetree work — and
worth upstreaming. Say which of the three layers you actually checked.

## Conventions

- Firmware: Zephyr/Linux kernel C style, tabs, `snake_case`. One subsystem per file in
  `firmware/src/`. Bounded buffers only — no dynamic allocation in the AT path.
- Host: Python 3 stdlib + `paho-mqtt` for the daemon; POSIX `sh` for anything in the hook
  path. No new runtime dependencies without an ADR.
- Names: daemon `wigwagd`, CLI `wigwag`, hook client `wg-notify`, topics `wigwag/*`, socket
  `/tmp/wigwag.sock`.
- States are `IDLE`/`BUSY`/`WAIT`/`ERROR` everywhere, uppercase, no synonyms.
- Secrets live in gitignored `firmware/credentials.conf` and `host/.env`. Never commit them,
  never echo them into logs or the journal.
