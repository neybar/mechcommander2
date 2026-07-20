# Credit-wise execution plan (M2 finish → M4)

Written 2026-07-18, when Fable credits became the scarce resource.
Goal: finish the port spending Fable only where Fable-level debugging or
judgment is genuinely needed.

## Model legend

- **SONNET** — switch the session to Sonnet (`/model`). Zero Fable spend.
- **OPUS** — switch the session to Opus. For meaty but well-scoped
  implementation.
- **FABLE** — spend Fable. Reserved for renderer-level mystery debugging
  and architectural calls.
- **Escalate** — start at the cheap end; move up only when stuck.
  "Stuck" = two failed fix attempts, or instrumentation added and still
  no root-cause hypothesis. Write findings to ENGINEERING_LOG *before*
  escalating so the next model starts warm.

Every session, any model: read `docs/ENGINEERING_LOG.md` top entries and
the auto-memory before touching renderer code. Log battles as you go —
that discipline is what makes model switches cheap.

## Tasks

### Now (M2 close-out)

1. ~~**Commit the dev input tools**~~ DONE (2026-07-18): `tools/devinput/`
   (mousemove/clickat/sendkeys + README), merged to main.
2. ~~**Review + merge `m2/backend-split` to main**~~ DONE (2026-07-18).
3. ~~**vk in-game resolution switching test matrix**~~ DONE (2026-07-18):
   windowed/fullscreen × several resolutions tested. Found a real,
   unrelated bug — intermittent (~27%) SIGSEGV in `SoundEngine::destroy()`
   on clean shutdown, reproducing on every config tested, not
   resolution-specific. Logged in ENGINEERING_LOG, **still open, needs an
   OPUS fix pass** (own root-cause hypothesis already written up: audio
   callback-thread teardown race). Resolution-mismatch-causes-visible-
   breakage theory investigated and ruled out.
4. **Effects/transparency parity sweep**: user plays later campaign
   missions on vk (free!); for anything off, GL-vs-vk screenshot pairs +
   MC2_VK_DEBUG log land in a bug note. — user + **SONNET** intake;
   **OPUS** for blend/state fixes; **FABLE** for anything that looks
   like today's descriptor-collision class. **Not started.**
5. ~~**Solo Mission screen check**~~ DONE (2026-07-18): first test since
   M1, first-ever on vk. Full chain (list → select → briefing → mech bay
   → back → reopen, no duplication → exit) verified clean.
6. ~~**Strip/organize vk debug instrumentation**~~ DONE (2026-07-18):
   consolidated 7 `getenv("MC2_VK_DEBUG")` call sites into one cached
   flag.

### Soon (risk + hygiene)

7. ~~**GitHub remote + push**~~ DONE (2026-07-18): public repo at
   github.com/neybar/mechcommander2. Went **public**, not private (user
   call). Pre-push asset audit purged a real retail-data leak
   (`Viewer/mission.fst`, 20MB of actual packed mission data — see
   ENGINEERING_LOG) and unused Windows-only `3rdparty.zip` from all
   history via `git filter-repo`. README rewritten (was a verbatim copy
   of alariq's). `main` now **branch-protected** (PR required, admin
   enforcement on, force-push/deletion disabled) after a repeat incident
   of committing straight to main — see feedback-git-branches memory.
   git-lfs fully retired (nothing left to track). Local pre-push hook
   (`tools/hooks/`, build check + advisory clang-tidy) added instead of
   a GitHub Action — revisit CI at M3. `upstream` (alariq/mc2) remote
   removed entirely per user request.
8. ~~**AD-4: asset-dir config + friendly missing-assets message**~~ DONE
   (2026-07-18): `-assetdir <path>` / `MC2_ASSET_DIR` env var + upfront
   `data/` validation, single `chdir()` covers all existing relative-path
   code. Found (not fixed, logged) a related bug: missing single files
   post-startup hit a retry loop that hangs forever because `MessageBox`
   is stubbed to a no-op on this port.
9. ~~**Clamp window-size requests to usable display bounds**~~ DONE
   (2026-07-20): `gos_GetDesktopDisplayMode` wired up in both backends
   (existing `graphics::get_desktop_display_mode` helper was unused before
   this), `CPrefs::applyPrefs` clamps `resolutionX/Y` to it before
   `gos_SetScreenMode`. Single fix point covers boot + mission-load switch.
   Verified via `MC2_AUTOQUIT_SECS` with an oversized `options.cfg` value —
   see ENGINEERING_LOG.
10. **Audit pre-existing clang-tidy warnings**: the pre-push hook has been
    running clang-tidy advisory-only (doesn't block pushes) since the
    GitHub-remote task, and every recent PR's build log has been full of
    warnings on files the PR didn't touch — macro-parentheses in
    `gameos.hpp`/`platform_winbase.h`, narrowing-conversion warnings
    scattered through `warrior.h` and similar legacy files,
    uninitialized-field warnings, unnecessary-value-param perf warnings.
    We've been treating these as "pre-existing, unrelated, ignore" one PR
    at a time without ever actually triaging the backlog. Categorize each
    warning class as (a) safe and worth fixing, (b) inherent to legacy
    Microsoft-era code / not worth the diff churn, or (c) possibly hiding
    a real bug — the narrowing conversions in particular are exactly the
    class of x86-assumption bug this port has hit before (see
    ENGINEERING_LOG's LP64/Darwin `unsigned long` entries). Not started.
    — **SONNET** triage/categorize (read-only judgment call, no code
    changes); **OPUS** for fixes beyond trivial one-liners; escalate to
    **FABLE** only if triage surfaces something that looks like a real
    correctness bug rather than style.

### M2 perf pass

11. **vk perf pass**: Instruments/MTL_HUD profile first, then the known
    naive spots — per-draw descriptor alloc (reuse within frame),
    per-draw push constants, pipeline-cache warmup, ring sizing. Verify
    with MC2_AUTOQUIT_SECS runs + frame timing. — **OPUS** (well-
    understood engineering; escalate to FABLE only for inexplicable
    results, e.g. sync bugs surfacing under reordering)

### M3 (other platforms)

12. **Linux Vulkan build** (CMake, SDL2, real Vulkan drivers vs
    MoltenVK differences). — **OPUS**; FABLE for driver-specific
    rendering mysteries only.
13. **Windows Vulkan build**. — **OPUS**, same escalation.

### M4 (release)

14. **.app bundle + MoltenVK dylib packaging, codesigning**. — **OPUS**
    first time (script it), **SONNET** thereafter.
15. **README for going public: lineage credits, AI disclosure,
    non-commercial license notes**. — **SONNET** draft, user edits.

### Future enhancements (post-parity, user-approved concepts)

16. **F5/F8 quick save/load hotkeys + "Game saved" toast**. — **SONNET**
    (spec: mirror PauseWindow guards, table entry in missiongui.cpp,
    controlGui.setChatText feedback)

### Standing FABLE-only items

- New "impossible" renderer bugs (wrong-content/corruption class).
- Architecture decisions (e.g. if the perf pass motivates a real
  frame-graph change, or M3 forces backend interface changes).
- Post-mortem review of tricky merges if cheaper models get stuck.

## Credit habits

- One task per session; end sessions rather than pivoting long context.
- User playtesting is free QA — prefer "user plays, files symptoms,
  cheap model triages" over model-driven exploration.
- Cheap models run the build/run/screenshot loops; they have the same
  tools (dev input tools, MC2_AUTOQUIT_SECS, MC2_VK_DEBUG).
- Escalation always passes through a written ENGINEERING_LOG entry —
  the expensive model should start from evidence, not re-derive it.
