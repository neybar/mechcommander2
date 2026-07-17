# MC2 — MechCommander 2 Modern Port

A cross-platform rebuild of MechCommander 2 (FASA Interactive / Microsoft, 2001),
targeting **macOS (Apple Silicon) first**, then Linux and Windows. Based on the
2006 Microsoft shared-source release, via the community OpenGL port.

See `docs/PROJECT_BRIEF.md` for vision and goals, `docs/TECHNICAL_NOTES.md` for
architecture decisions and codebase evaluation, and `docs/ROADMAP.md` for milestones.

## Current status

M0 complete; M1 mostly complete (2026-07-17): game boots on macOS ARM64,
main menu renders and is fully interactive, intro skip works cleanly
(ESC/space/click cut straight to the menu), clean exit. Game dir:
`~/Games/mc2-port` (built from mc2srcdata; deployment recipe in
ENGINEERING_LOG). `MC2_AUTOQUIT_SECS=N` quits cleanly after N seconds for
unattended testing.

Next session: M1 exit criterion — load and play a Solo Mission start to
finish.

## Key decisions (context for all work)

- **Base codebase:** fork of [alariq/mc2](https://github.com/alariq/mc2) — the
  community port that already replaced DirectX with OpenGL and runs on
  Windows/Linux 64-bit with SDL2 + CMake.
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
- Local git repo, no remote yet. Commit in small, buildable increments.
- **Work on feature branches, not main** (`fix/...`, `feat/...`, `m1/...`).
  Merge to main only after the user has seen the result working (or reviewed
  the diff for non-runnable changes). Docs-only commits may go straight to
  main. Never rebase/amend anything already merged to main.
- No remote means no backup — worth revisiting (private GitHub remote or a
  bare repo on another disk).
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
