# ADR-0014 — The Zephyr workspace lives at the repo root, pinned and deliberately minimal

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

Phase 2 needed a Zephyr workspace, and nothing Zephyr-related existed on the development machine
— no `west`, no `ZEPHYR_BASE`, no SDK. Three things had to be decided before a single line of
firmware could be built, and all three are the kind of choice that is annoying to reverse once
build directories, CI and documentation depend on it:

1. **Where the workspace lives.** `west` supports several topologies. The repository is a mixed
   one — `host/` (a uv Python project), `firmware/`, `hardware/`, `enclosure/`, `docs/` — so the
   Zephyr checkout cannot simply *be* the repository.
2. **How much gets fetched.** Zephyr's manifest has ~80 projects covering every vendor and
   architecture. A default `west update` pulls gigabytes, almost all of it irrelevant to a
   Cortex-M0+ Microchip part with no networking (ADR-0002 puts the IP stack on the module).
3. **How the version is pinned.** ADR-0006 committed to mainline "pinned to a known-good
   revision and moved deliberately, not floating", which rules out both tracking a branch and
   the convenience of whatever `west init` defaults to.

## Decision

**T2 topology with the workspace top directory at the repository root.**

`firmware/west.yml` is the manifest repository (`self: path: firmware`) and `firmware/` is also
the Zephyr application. `west init -l firmware` therefore places `.west/`, `zephyr/`, and
`modules/` beside `firmware/` at the repo root, all gitignored. This keeps the application paths
that `docs/PLAN.md` already specifies — `firmware/src/`, `firmware/prj.conf`,
`firmware/boards/` — and keeps the pinned revision inside the repository where it is diffable
and reviewable.

**Pinned to an explicit mainline commit**, not a branch and not a release tag:
`357467a011cd2557a1a3f0b4be83d817c4addc9b` (mainline `main`, 2026-08-14).

**Minimal by allowlist.** The manifest imports Zephyr with
`import: name-allowlist: [cmsis, cmsis_6, hal_microchip, picolibc]`, fetched with
`west update --narrow -o=--depth=1`. Measured result: `zephyr/` 622 MB, `modules/` 409 MB.

**Toolchain:** Zephyr SDK 1.0.1 (the version `zephyr/SDK_VERSION` requires), installed with
`west sdk install -t arm-zephyr-eabi` — one toolchain, 1.4 GB, at `~/zephyr-sdk-1.0.1`. Host
tools come from Homebrew (`cmake`, `ninja`, `dtc`, `gperf`), which is not merely a preference:
**SDK 1.0.1 has no macOS host tools at all**, and the installer says so outright — *"SKIPPED:
macOS host tools are not available yet."*

Python tooling lives in a repo-root `.venv` created with `uv` (matching `host/`'s tooling) with
`west`, `pyocd`, and `zephyr/scripts/requirements-base.txt` — base only, not the test or
documentation sets.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| **Workspace top directory at `firmware/`** (`firmware/zephyr`, `firmware/modules`) | Keeps the repo root tidier, which is genuinely nicer. Rejected because the application would have to move down to `firmware/app/`, breaking every path already written into `docs/PLAN.md` and the ADRs for a purely cosmetic gain. |
| **Zephyr outside the repo** (`~/zephyrproject`, freestanding app via `ZEPHYR_BASE`) | Smallest repository, and the most common tutorial setup. Rejected because the pinned revision would then live outside version control, which directly contradicts ADR-0006's "pinned, moved deliberately" commitment — a fresh machine would get whatever Zephyr happened to be there. |
| **Pin a release tag (v4.4.2 was current)** | Better stability story, and tempting. Rejected per ADR-0006: PIC32C enablement is landing continuously, and this part needs it — the PL10 board port itself is dated 2026. A SHA gives the same reproducibility as a tag with none of the lag. |
| **Default `west update` (no allowlist)** | One less thing to maintain. Rejected on measurement: it fetches every vendor HAL, all the babblesim components, TF-M, OpenThread, hostap and more — gigabytes, none of it reachable from this build. |
| **Blobless clone (`--filter=blob:none`)** instead of `--depth=1` | Smaller initial fetch. Rejected because a build reads most of the tree anyway, so the blobs arrive lazily one round-trip at a time, turning a one-off download into recurring build latency and a network dependency for every clean build. |
| **Homebrew `gcc-arm-embedded` as the toolchain** (already installed) | Would avoid a 1.4 GB SDK download. Rejected because Zephyr's ARM builds expect the SDK's `arm-zephyr-eabi` with its bundled picolibc; the `gnuarmemb` variant ships newlib and needs C-library Kconfig deviations. Not worth diverging from the supported toolchain to save disk. |
| **Install the whole SDK** | One command, no thinking. Rejected: several gigabytes of toolchains for architectures this project will never build. |

## Consequences

**Accepted costs**
- `zephyr/`, `modules/`, `.west/` and `.venv/` sit at the repo root and must stay gitignored;
  `git status` cleanliness now depends on that, which is a mild footgun for a new contributor.
- The allowlist is a maintenance surface: enabling a subsystem later (a filesystem, mbedTLS, a
  Bluetooth stack) means adding its module and re-running `west update`, with a confusing CMake
  error as the first symptom rather than a clear "module missing" message.
- `--depth=1` means no local history: `git log` and `git bisect` inside `zephyr/` are
  unavailable, so investigating an upstream regression needs a deeper fetch first.
- Moving the pin is a manual, deliberate act. That is the point, but it does mean the tree ages
  silently until someone moves it.

**Benefits**
- A fresh machine reproduces the exact tree from the repository alone, which the alternatives
  cannot promise.
- ~1 GB of source and one toolchain instead of many gigabytes.
- The application paths in `docs/PLAN.md` and the ADRs stay true.
- Everything Zephyr-related is invisible to `git`, so firmware review diffs contain only wigwag's
  own code.

**Revisit if** the allowlist becomes a recurring source of build confusion (then take the full
import and accept the size), or if the pinned revision needs local patches for more than one
feature — at which point the manifest should carry a Zephyr fork explicitly rather than
accumulating local edits to a pinned tree.
