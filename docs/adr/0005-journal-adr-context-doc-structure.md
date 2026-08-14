# ADR-0005 — Documentation is split into journal, ADRs, CONTEXT and CLAUDE.md

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

The project's stated first rule is to keep a journal of everything. The question was what shape
that should take, and whether a more standard term than "journal" exists.

Surveying the vocabulary honestly: **there is no single dominant term for the chronological
log.** "Devlog", "worklog" and "engineering journal" are used interchangeably and none has won.
The only layer with a genuinely standard name is the *decisions* subset — **ADR (Architecture
Decision Record)** — which is widely understood and has established conventions. Adjacent
patterns worth knowing: **memory bank** (the Cline/Roo convention for markdown files an agent
re-reads each session) and **handoff / session summary** for context transfer.

This project also spans firmware, host software, PCB and enclosure, and much of its hard-won
knowledge is *negative* — "PL10's board port doesn't enable PWM but the driver exists", "green
LEDs can't run from 3.3 V". That knowledge is expensive to rediscover and invisible in the code.

A single chronological file is the obvious starting point, but it degrades: it grows without
bound, and decisions buried in it become unfindable precisely when someone asks "why is it like
this?"

## Decision

Four documents, each with one job:

| File | Job | Shape |
|---|---|---|
| `JOURNAL.md` | chronological narrative — what changed, why, what failed | append-only, **newest first** |
| `docs/adr/NNNN-*.md` | one durable decision each | numbered, immutable once accepted |
| `CONTEXT.md` | ubiquitous language | living, renames propagate everywhere |
| `CLAUDE.md` | standing instructions for AI sessions | living, rules not history |

Reinforced by:

- a **`journal` skill** (`.claude/skills/journal/`) holding the entry and ADR templates, the
  ADR-promotion criteria, and a read mode for catching up on an unfamiliar repo. Written with no
  project specifics so it can be lifted to `~/.claude/skills/` and reused;
- a **`SessionEnd` hook** that warns on stderr when code changed but the journal didn't.

Three deliberate details:

**Newest-first journal.** Readers — human or agent — want recent context, and the top of the file
is the cheapest thing to read. Append-at-bottom optimises for the writer instead.

**ADRs must contain rejected alternatives.** The template requires the table. An ADR without one
is a note, not a decision record; the rejected options are what stop the question being
re-litigated.

**The reminder never writes an entry itself.** An auto-generated entry looks like a real one while
containing none of the reasoning, which is worse than an obvious gap.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **Single `JOURNAL.md`, nothing else** | Simplest possible thing that satisfies "keep a journal", with nothing to decide about where something goes. Rejected because decisions become unfindable in a long chronology, and there is nothing stable to cite — "see ADR-0002" has no equivalent. |
| **ADRs only, no journal** | Decisions stay addressable, but everything that isn't a decision — measurements, dead ends, partial progress, what we were mid-way through — has nowhere to live. That is most of the useful content. |
| **Git history as the journal** | Already exists and is free. But commit messages record *what changed*, not what was tried and abandoned, and nothing abandoned ever produces a commit. The most valuable content would be exactly what git can't hold. |
| **A "memory bank" directory of agent-facing context files** | The Cline convention; genuinely useful. Rejected as the *primary* structure because it is optimised for agent re-reading rather than human review, and `CONTEXT.md` + `CLAUDE.md` already cover the durable-context role at this project's scale. |
| **Auto-generated journal entries from a `Stop` hook** | Would guarantee entries with zero discipline required. Rejected: it produces plausible-looking noise, and it cannot record *why* or what was rejected — the only parts worth keeping. |
| **Issue tracker instead of files** | Better for coordinating multiple people. This is one engineer plus AI sessions; files in the repo are diffable, offline, reviewable alongside the code, and readable by the agent without an integration. |

## Consequences

**Accepted costs**
- Four places means judgement calls about where something belongs. Mitigated by the skill
  encoding the ADR-promotion criteria explicitly.
- Discipline is required at the end of a session, and the reminder is only a nudge — it can be
  ignored, and nothing enforces quality.
- Some duplication between journal entries and ADRs is unavoidable and acceptable; the journal
  says "we decided X today", the ADR says "X, and here's why not Y".

**Benefits**
- Decisions are citable (`ADR-0002`) from code comments, journal entries and conversation.
- Negative results have a home, which is the main reason the journal exists.
- `CONTEXT.md` keeps naming consistent across four very different disciplines.
- The skill is reusable across projects — the practice outlives this repo.

**Revisit if** the journal becomes unwieldy despite the split (year-based files, `docs/journal/`),
or if the ADR set grows enough to need an index.
