# MC2 — MechCommander 2 Modern Port

A cross-platform rebuild of MechCommander 2 (FASA Interactive / Microsoft, 2001),
targeting **macOS (Apple Silicon) first**, then Linux and Windows. Based on the
2006 Microsoft shared-source release, via alariq's OpenGL port (a
single-maintainer project — see Key decisions).

See `docs/PROJECT_BRIEF.md` for vision and goals, `docs/TECHNICAL_NOTES.md` for
architecture decisions and codebase evaluation, and `docs/ROADMAP.md` for milestones.

## Current status

**M0 and M1 complete (2026-07-17): the game is playable on macOS ARM64.**
Missions load and play start-to-finish (movement, combat, win triggers,
campaign progression user-verified on Training 1 & 2). Windowed mode
(`b FullScreen = FALSE` in options.cfg) and in-game resolution switching
work; menus stay 800x600 by design, chosen resolution applies at mission
load. Game dir: `~/Games/mc2-port` (built from mc2srcdata; deployment
recipe in ENGINEERING_LOG — note `data/missions/` and `data/campaign/`
must be unpacked on disk).

Dev hooks: `MC2_AUTOQUIT_SECS=N` (clean quit after N secs),
`./mc2 -mission mc2_01` (skip menus/logistics, quickstart lance),
`MC2_DEBUG_INPUT=1` (per-second mouse coordinate-chain dump),
`./mc2 -assetdir <path>` / `MC2_ASSET_DIR=<path>` (run from any CWD; see
BUILDING.md — AD-4, done 2026-07-18).

Next: M2 (Vulkan renderer), starting with the renderer abstraction audit
(gos/MLR boundary — where does a Vulkan backend plug in). GitHub remote is
live: github.com/neybar/mechcommander2 (public, `main` branch-protected,
PR required — done 2026-07-18, see ENGINEERING_LOG).

Resolution requests are now clamped to the desktop display's bounds
(2026-07-20, `CPrefs::applyPrefs` in `prefs.cpp`; see ENGINEERING_LOG). The
load-screen exit-animation/prefs-ordering item once carried forward here was
already fixed (commit `be5ec77`, predates this note) — removed.

## Key decisions (context for all work)

- **Base codebase:** fork of [alariq/mc2](https://github.com/alariq/mc2),
  which already replaced DirectX with OpenGL and runs on Windows/Linux
  64-bit with SDL2 + CMake. Note it is one person's work, not a "community
  port" — there is no community behind it, and no guarantee of continued
  upstream development. That fragility was a foundational motivation for
  forking it as an independent base rather than depending on it.
- **Graphics:** port the renderer to **Vulkan**, using **MoltenVK** on macOS.
  OpenGL is deprecated on macOS; Vulkan gives one modern backend for all three
  platforms. Interim: macOS's OpenGL 4.1 may be used to get a first boot before
  the Vulkan port lands.
- **Language/build:** C++ modernized incrementally, CMake, out-of-source builds.
- **Assets are user-provided, never committed.** The engine must load assets
  from a user-supplied directory (retail install or Microsoft's shared-source
  asset set). No copyrighted game data in this repo, ever — including in tests.
- **Modding matters.** Two decades of community content exists (see
  moddb.com/games/mech-commander-2). Don't break compatibility with existing
  mod formats without a documented reason.

## Licensing constraints (read before adding code)

- Original engine code: Microsoft **Shared Source Limited Permissive License**
  (non-commercial). This project is and stays free.
- New code in the alariq port and this project: **GPL v3**.
- **Do not submit code from this repo upstream to alariq/mc2** — that project
  prohibits AI-generated contributions. We are an independent downstream fork;
  credit upstream prominently, never push our patches to them.

## Workflow

- Claude drives implementation; the user (jalance) reviews. Explain non-obvious
  changes in terms of what they do to the game/engine, not just the code.
- Dev machine: Apple Silicon Mac. The original code assumes x86/little-endian
  and 32-bit-era Windows idioms — treat alignment, `long` size, and pointer
  truncation bugs as expected hazards on ARM64.
- GitHub remote `origin`: github.com/neybar/mechcommander2 (public). Commit in
  small, buildable increments.
- **Work on feature branches, not main** (`fix/...`, `feat/...`, `m1/...`).
  `main` is branch-protected (PR required, admin enforcement on) — merge via
  PR only after the user has seen the result working (or reviewed the diff
  for non-runnable changes). Docs-only commits may go straight to main.
  Never rebase/amend anything already merged to main.
- No `upstream` remote (alariq/mc2) — removed per user request; see
  Key decisions for why we don't push patches there anyway.
- Keep an engineering log at `docs/ENGINEERING_LOG.md` (practice borrowed from
  the Generals Mac port that inspired this project): one entry per significant
  bug hunt or porting battle — symptom, cause, fix. Append entries as part of
  the work, not after the fact.
- Prefer minimal diffs against the vendored base code; keep our changes
  separable from upstream's so provenance stays clear.

## Build

`cmake -B build && cmake --build build -j8` → `build/mc2`. Prereqs and
platform notes in BUILDING.md. When adding/porting code, watch for the Darwin
traps catalogued in docs/ENGINEERING_LOG.md — especially `unsigned long` vs
`uint64_t` overload identity and `<malloc.h>`.
