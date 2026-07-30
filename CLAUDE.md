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
load. Resolution requests are clamped to the desktop display's bounds
(`CPrefs::applyPrefs` in `prefs.cpp`). Game dir: `~/Games/mc2-port` (built
from mc2srcdata; deployment recipe in ENGINEERING_LOG — note
`data/missions/` and `data/campaign/` must be unpacked on disk).

**M2 in progress (2026-07-29): the Vulkan backend exists and is playable.**
Both backends build from one tree — `cmake --build build` → GL, `cmake --build
build-vk` → Vulkan (MoltenVK). Mission 1 has been completed and Mission 2
started on the vk build. Parity is close but not confirmed: of the four
task-4 findings from Mission 1, three are closed (the black quad =
cement/pavement holes, fixed; building glass = original-engine wart;
checkerboard wreck = not a bug) and **one is open** — a mech's team-colour
skin flipping blue↔red, which matches the descriptor-cache-collision class.
Later missions haven't been swept yet. No perf pass has been done.

Next up, in `docs/CREDIT_PLAN.md` order — **task 4** effects/transparency
parity sweep (needs user playtesting; one open finding), **task 19** swapchain
binary-semaphore reuse (a real vk sync bug the validation layer flags every
frame), **task 3's** leftover intermittent SIGSEGV in `SoundEngine::destroy()`
on shutdown, **task 10** clang-tidy warning audit, then **task 11** the vk
perf pass. Pick from the plan rather than inventing an order.

GitHub remote is live: github.com/neybar/mechcommander2 (public, `main`
branch-protected, PR required — see ENGINEERING_LOG).

### Dev hooks

Runtime hooks (all `#ifndef FINAL`, all off unless set):

| Hook | Effect |
|---|---|
| `./mc2 -mission mc2_01` | Skip menus/logistics, quickstart lance |
| `./mc2 -assetdir <path>` / `MC2_ASSET_DIR` | Run from any CWD (see BUILDING.md) |
| `MC2_AUTOQUIT_SECS=N` | Clean quit after N secs |
| `MC2_LOAD_SAVE=<path\|1>` / `MC2_LOAD_SAVE_SECS` | Auto-load a savegame N secs into a mission, no synthetic input. A value containing `/` loads that explicit `.ims`; `1` uses `savePath/testgame.ims` |
| `MC2_DEBUG_INPUT=1` | Per-second mouse coordinate-chain dump |
| `MC2_VK_DEBUG=1` | vk draw/state logging (incl. `BADTEX`, `EXTREME-ARGB`) |
| `MC2_VK_TRACEPX="x,y"` | Log every pass whose triangles cover that pixel |
| `MC2_VK_RIALOG="x,y"` | Log indexed draws whose bbox covers that pixel |
| `MC2_VERTDUMP=<path>` / `_AT_SECS` | One-frame terrain vertex dump, for GL-vs-vk diffing |
| `MC2_FOG_DEBUG=1` | Fog-bake dump |
| `MC2_VK_NO_DSET_CACHE=1` | Disable the descriptor-set cache |
| `MC2_VK_CAPTURE_AT_SECS=N` / `MC2_VK_CAPTURE_FILE` | Write a Metal `.gputrace` headlessly (also needs `METAL_CAPTURE_ENABLED=1`) |

**`tools/vkprobe/run.sh`** wraps launch → settle → capture → clean-quit for
headless repros (`--save`, `--capture screenshot|gputrace|log`, `--at/--quit`,
`--bin` for GL vs vk). It passes through any exported `MC2_*`, so check
`env | grep MC2_` before a capture that matters — a stale export will silently
change the result. Archived repro saves live in `docs/bugs/saves/`; load them
**by explicit path**, never by copying over the user's live quicksave.

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
- **Work on feature branches, not main, no exceptions** (`fix/...`,
  `feat/...`, `docs/...`, `m1/...`). `main` is branch-protected with admin
  enforcement on — GitHub rejects direct pushes even for docs-only changes,
  so there's no "docs-only can go straight to main" shortcut anymore. Merge
  via PR only after the user has seen the result working (or reviewed the
  diff for non-runnable changes). Never rebase/amend anything already merged
  to main.
- No `upstream` remote (alariq/mc2) — removed per user request; see
  Key decisions for why we don't push patches there anyway.
- Keep an engineering log at `docs/ENGINEERING_LOG.md` (practice borrowed from
  the Generals Mac port that inspired this project): one entry per significant
  bug hunt or porting battle — symptom, cause, fix. Append entries as part of
  the work, not after the fact.
- **Updating the docs is part of the work, not a separate favour to ask about.**
  Ship doc changes in the same PR as the change that made them true, without
  being prompted:
  - `CLAUDE.md` "Current status" whenever the milestone, build layout, dev-hook
    list, or what's-next changes. A stale status here misleads every future
    session, so treat it as the highest-value file in the repo.
  - `docs/ENGINEERING_LOG.md` for the symptom → cause → fix of anything
    non-obvious, **including refuted hypotheses** — knowing what was ruled out,
    and on what evidence, is what stops the next session re-running it.
  - `docs/CREDIT_PLAN.md` when a task starts, finishes, or a new one is found.
    Check for duplicate task numbers after any merge.
  - `docs/bugs/<date>-<slug>.md` for a bug worth investigating later, even one
    you aren't fixing now. Note whether it reproduces on **both** backends.
  - When a conclusion turns out to be wrong, **correct the doc that recorded
    it** and say why it was wrong. A confidently-worded bad conclusion costs
    more than no conclusion — see the 2026-07-27 pavement-holes entry.
- Prefer minimal diffs against the vendored base code; keep our changes
  separable from upstream's so provenance stays clear.

## Build

`cmake -B build && cmake --build build -j8` → `build/mc2`. Prereqs and
platform notes in BUILDING.md. When adding/porting code, watch for the Darwin
traps catalogued in docs/ENGINEERING_LOG.md — especially `unsigned long` vs
`uint64_t` overload identity and `<malloc.h>`.
