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

1. **Commit the dev input tools** (mousemove/clickat/sendkeys Swift
   sources from the session scratchpad → `tools/devinput/`, tiny README;
   scratchpads are session-scoped and these enable all headless GUI
   testing). — **SONNET**
2. **Review + merge `m2/backend-split` to main** (user reviews; merge
   mechanics, branch tidy). — **SONNET**
3. **vk in-game resolution switching**: scripted test matrix (windowed/
   fullscreen × a few resolutions, screenshots, clean-exit checks) using
   the dev input tools. — **SONNET** to test; **OPUS** to fix what
   breaks; **FABLE** only if it smells like a swapchain/present contract
   bug (escalate with logs).
4. **Effects/transparency parity sweep**: user plays later campaign
   missions on vk (free!); for anything off, GL-vs-vk screenshot pairs +
   MC2_VK_DEBUG log land in a bug note. — user + **SONNET** intake;
   **OPUS** for blend/state fixes; **FABLE** for anything that looks
   like today's descriptor-collision class.
5. **Solo Mission screen check** (untested since M1). — **SONNET**
   test-and-report; escalate fixes as above.
6. **Strip/organize vk debug instrumentation** (keep MC2_VK_DEBUG and
   MC2_VK_NO_DSET_CACHE, tidy anything ad-hoc). — **SONNET**

### Soon (risk + hygiene)

7. **GitHub private remote + git-LFS push** (license audit already done,
   OQ-7; this is the only backup of months of work — overdue). —
   **SONNET**
8. **AD-4: asset-dir config + friendly missing-assets message**. —
   **SONNET** (clear spec in ROADMAP)
9. **Clamp window-size requests to usable display bounds** (minor). —
   **SONNET**

### M2 perf pass

10. **vk perf pass**: Instruments/MTL_HUD profile first, then the known
    naive spots — per-draw descriptor alloc (reuse within frame),
    per-draw push constants, pipeline-cache warmup, ring sizing. Verify
    with MC2_AUTOQUIT_SECS runs + frame timing. — **OPUS** (well-
    understood engineering; escalate to FABLE only for inexplicable
    results, e.g. sync bugs surfacing under reordering)

### M3 (other platforms)

11. **Linux Vulkan build** (CMake, SDL2, real Vulkan drivers vs
    MoltenVK differences). — **OPUS**; FABLE for driver-specific
    rendering mysteries only.
12. **Windows Vulkan build**. — **OPUS**, same escalation.

### M4 (release)

13. **.app bundle + MoltenVK dylib packaging, codesigning**. — **OPUS**
    first time (script it), **SONNET** thereafter.
14. **README for going public: lineage credits, AI disclosure,
    non-commercial license notes**. — **SONNET** draft, user edits.

### Future enhancements (post-parity, user-approved concepts)

15. **F5/F8 quick save/load hotkeys + "Game saved" toast**. — **SONNET**
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
