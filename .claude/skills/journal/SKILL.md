---
name: journal
description: Keep an engineering journal (devlog) and promote durable decisions to ADRs. Use when the user says "journal", "devlog", "worklog", "log what we did", "write up this session", "record this decision", "write an ADR", or when a working session is wrapping up and its findings would otherwise be lost. Also use when starting work in an unfamiliar repo to catch up on recent history.
---

# Journal

Two artifacts, two jobs:

- **`JOURNAL.md`** — append-only chronological narrative. What happened, why, what failed.
- **`docs/adr/NNNN-*.md`** — one durable decision per file, addressable and citable.

The split exists because a journal is unsearchable by intent. Six months on, nobody can find
"why are we using X" in 4,000 lines of chronology, but they can read a directory of decisions.

## Catching up (read mode)

When starting work in a repo that has a journal, read the **newest entries first** — the file
is reverse-chronological, so the top is current. Read `docs/adr/` index-style (titles alone
are often enough) and open only the ADRs relevant to what you're about to touch.

## Writing an entry

1. Locate the journal: `JOURNAL.md` at the repo root, else `docs/JOURNAL.md`. If none exists,
   create it at the root with the header from `templates/journal-header.md`.
2. Get today's real date — `date +%Y-%m-%d`. **Never guess the date**, and never reuse a date
   from earlier in the conversation without checking.
3. Insert the new entry **at the top**, directly under the file header, using
   `templates/entry.md`. Newest-first keeps the useful part cheap to read.
4. If the day already has an entry and you're continuing the same thread of work, extend it
   rather than adding a second same-day entry.

### What makes an entry worth writing

Write for a competent stranger — often a future session with none of your context.

- **Record failures and dead ends.** This is the highest-value content and the most commonly
  omitted. "Tried X, it doesn't work because Y" saves someone the same day of work. An entry
  with no failures in it is usually an entry that's hiding something.
- **Record what you verified and how.** Cite the datasheet table, doc page, or command output
  you relied on. "Checked the vendor docs" is worthless; "supported-features table lists no
  PWM driver" is durable.
- **Convert relative dates to absolute.** "Next week" is meaningless in an archive.
- **Note surprises.** Anywhere reality differed from the obvious assumption is exactly where
  the next person will trip.
- **Keep it proportional.** A one-line fix gets a one-line entry. Don't pad.
- **Never** paste secrets, tokens, keys, or credentials into the journal.

Do not narrate the conversation ("the user asked me to…"). Record the *work*: decisions,
findings, changes, and what's still open.

## Promoting a decision to an ADR

Write an ADR when **any** of these hold:

- it constrains future work (a platform, protocol, dependency, or interface choice);
- you rejected credible alternatives, and the reasons would be re-litigated otherwise;
- someone reasonable would look at the result later and ask "why on earth is it like this";
- reversing it would be expensive.

Skip the ADR for: routine implementation detail, anything easily reversed, and style choices
that belong in the project's instructions file instead.

Procedure:

1. Next free number in `docs/adr/` (zero-padded, four digits). Create the directory if needed.
2. Copy `templates/adr.md`. Filename `NNNN-kebab-case-title.md`.
3. State the alternatives you rejected **and why** — an ADR without rejected alternatives is
   just a note.
4. Reference it from the journal entry as `ADR-NNNN`, and cite it in code comments where the
   decision is non-obvious from the code.

An accepted ADR is not edited to change its decision. Write a new one and mark the old one
`Status: Superseded by ADR-NNNN`.

## Bundled files

| File | Use |
|---|---|
| `templates/journal-header.md` | header when creating a new journal |
| `templates/entry.md` | one journal entry |
| `templates/adr.md` | one ADR |
