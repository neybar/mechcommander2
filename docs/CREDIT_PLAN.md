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
   resolution-specific. Logged in ENGINEERING_LOG.

   **OPUS fix pass run 2026-08-05: no fix possible — it will not reproduce.
   Still open, but BLOCKED on a live sighting, not on model time.** 85 launches
   across five configurations (current vk windowed; vk under CPU load; GL; the
   **July-18 binary rebuilt from `c199e46`**; and vk fullscreen cycling three
   resolutions) produced **zero** crashes — a ~10^-11 outcome at the measured
   27%. Two things were gained and both are in the 2026-08-05 ENGINEERING_LOG
   entry: the crash site is now **pinned to `SDL_QuitSubSystem(SDL_INIT_AUDIO)`**
   (`gameos_sound.cpp:270`, proved by disassembling the July binary and mapping
   return addresses — July's ":272" is the *next* line), and six hypotheses are
   refuted with evidence, including the original **audio-callback-race** suspect,
   display-mode switching, the audio output device, SDL library drift, and the
   plausible-looking `soundHeap`-destroyed-before-the-buffers theory (the gos
   heaps are inert — `operator new` is plain `malloc`).

   **Do not re-run scripted launch loops on this.** That avenue is exhausted; a
   sixth clean batch buys nothing. The next useful input is a crash **in the
   wild** — when it happens, save `~/Library/Logs/DiagnosticReports/*.ips`
   before macOS ages it out, and note what was connected. Four real teardown
   defects found by reading (dead `is_initialized_` flag, undrained
   `streamedAudioList_`, uninitialized `audio_device`, leaky
   `gosAudio_DestroyResource`) are catalogued in the same entry as **hardening
   candidates, explicitly not the fix** — see task 3a.
3a. **Harden the audio teardown (independent of task 3's crash)**: four defects
    found while reading `gameos_sound.cpp` on 2026-08-05, each real on its own
    terms and none of them a verified fix for the SIGSEGV — full detail in that
    day's ENGINEERING_LOG entry, section 5.
    (a) `is_initialized_` is never set true, and `destroy()` never checks it, so
    a failed `gos_CreateAudio()` — which `prefs.cpp:496` deliberately tolerates —
    still runs the whole SDL teardown including `SDL_QuitSubSystem`. Highest
    value of the four, and the only one whose shape matches the known crash site.
    (b) `SoundEngine::destroy()` never drains `streamedAudioList_`, so movie
    `SDL_AudioStream`s outlive the subsystem quit that destroys them.
    (c) `audio_device` is missing from the constructor's init list.
    (d) `gosAudio_DestroyResource` frees a resource only when it is bound to a
    channel, leaking unbound ones until shutdown.
    **Ship it labelled as hardening; it must not be recorded as closing task 3**,
    which stays open until a live crash is seen again. Note the risk that argues
    for care rather than speed: with no reproduction, a change to this exact code
    path cannot be shown not to have made things worse. Not started. — **SONNET**
    (small, well-specified, no judgment call left open).

4. **Effects/transparency parity sweep**: user plays later campaign
   missions on vk (free!); for anything off, GL-vs-vk screenshot pairs +
   MC2_VK_DEBUG log land in a bug note. — user + **SONNET** intake;
   **OPUS** for blend/state fixes; **FABLE** for anything that looks
   like today's descriptor-collision class. **All four Mission 1 findings are
   now closed (2026-07-31).** Later campaign missions have not been swept yet,
   so the task itself stays open pending playtesting:

   - ~~**Finding 1 — recurring black quad (Mission 1, parking pad / tower
     roof / wall)**~~ **CLOSED 2026-07-27: same bug as the vk cement/
     pavement holes**, fixed by porting GL's `makeKindaSolid` into the vk
     `fillPixels` (commit `41f81b3`, PR #7). Zero-alpha retail TGAs made the
     draw rasterize perfectly and paint nothing — which is exactly why the
     SRM turret's retracted mechanism was visible *through* the black area,
     and why no graphics toggle touched it. Both intermediate theories
     recorded here (shadow blend-state, then terrain-lighting-goes-black)
     were wrong; see ENGINEERING_LOG 2026-07-27 and
     `docs/bugs/2026-07-24-vk-cement-holes-FINDINGS.md`. User play-test
     confirmed all tracked spots clean.
   - ~~**Finding 2 — destroyed LRM truck wreck, black/white checkerboard**~~
     **CLOSED 2026-07-29, not a bug**: user's call on review — it is just
     low-resolution wreck art, not a wrong/missing texture. No vk/GL
     divergence claimed; nothing to fix.
   - ~~**Finding 3 — downed mech's team-color skin flips blue→red**~~
     **CLOSED 2026-07-31 — fixed. NOT a descriptor-cache collision, and not a
     vk bug at all.** The descriptor-collision triage recorded here was
     **refuted** by one free test: jalance ran the same scene on **GL** and it
     flipped there too, which a Vulkan-only cache mechanism cannot do. Real
     cause: **`-DBGR` itself.** It R↔B-inverted *every* mech and vehicle team
     colour, and because `resetPaintScheme`'s early-return stored `ps*` raw
     while `getPaintScheme` always converted, the inversion also **oscillated**
     on every LOD transition — which is the part that got reported.
     Camera-dependence = LOD distance thresholds. **Fixed by removing `-DBGR`**
     (`mclib/CMakeLists.txt`): with it undefined both paths are the identity, so
     the flip disappears by construction *and* colours come out as authored.
     A shared `storePaintScheme()` helper on `Mech3DAppearance` and
     `GVAppearance` is kept as defence-in-depth, not as the fix.
     **Port regression, not an original-engine wart:** the asymmetric code is
     Microsoft's but was inert (MS never defined `BGR`); upstream's `383e3c2`
     added `-DBGR` on top of `d0ce5f4`, which had already fixed the byte order
     ten months earlier — a double-correction.
     **Review caught a bad first fix** that equalised the two storage paths but
     left the inversion in place, stabilising every unit on the *wrong* colour;
     see ENGINEERING_LOG for that post-mortem. Verified twice: instrumented
     before/after for the flip, and a fresh-mission screenshot for the colours
     (lance renders yellow/navy, matching `options.cfg` exactly). Savegame
     side-effect in `docs/bugs/2026-07-31-paintscheme-baked-into-saves.md`.
   - ~~**Finding 4 — building glass flips transparent↔opaque-dark with
     camera pan**~~ **CLOSED 2026-07-21: original-engine wart, not a vk
     parity issue.** Windows are drawn *untextured* (`addVertices(0xffffffff,
     …, MC2_DRAWALPHA)`, `tgl.cpp:2671`) and coloured by per-vertex lighting;
     daytime windows are meant to be dark grey `0x2f2f2f` (`tgl.cpp:1763`),
     so the *dark* state is correct and the *see-through* state is the bug.
     The window pass draws untextured alpha with depth-write on (`ZWrite=1`,
     `ZCompare=LEQUAL`, in `mclib/txmmgr.cpp` — find it by the port FIXME
     starting "becase some objects are drawn with alphs blend + depth write";
     it was at :1412 when this was written and had already drifted to :1468 by
     2026-07-30, so search the text, not the line) so glass panes depth-reject
     each other. **Provenance
     confirmed against the pristine Microsoft 2006 shared-source**
     (`ms_txmmgr.cpp:796-810`, same depth-writing alpha pass) — original
     Microsoft code, like task 18, not a port regression. Reproduces on GL;
     user confirmed. **Fix attempted 2026-07-21 (ZWrite=0; vertex z-bias;
     hardware polygon offset) and REVERTED** — all dead ends, the real
     mechanism is transparent-vs-transparent, which needs back-to-front
     sorting the engine doesn't do. A proper fix = a transparency-sorting
     pass (its own future task, see standing items). Post-mortem in
     ENGINEERING_LOG. Being untextured, glass never constrained the mech
     flip in finding 3 — they are separate bugs.
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
   see ENGINEERING_LOG. **Lead review (2026-07-20) surfaced two follow-ups —
   9a and doc fix below; both are refinements, not regressions.**
9a. **Snap the resolution clamp to the nearest enumerated display mode**:
    task 9 clamps to the raw desktop bounds (`SDL_GetDesktopDisplayMode`,
    e.g. 3008x1692), which produces a resolution that isn't one of the modes
    the in-game Options dropdown enumerates *and* isn't one of the widths
    `controlgui.cpp` (~2648) recognizes — so an oversized config silently
    falls into the generic `else → buttonlayout1920.fit` HUD-layout path.
    Safe (that fallback exists) and strictly better than the old unclamped
    window, but inconsistent with the dropdown and with the earlier
    "fall back exact → size-only → closest-area" logic from the 2026-07-17
    resolution work. Reuse that enumeration + closest-fit path so a clamped
    value is always a real supported mode that hits a matched HUD layout.
    Secondary: the clamp only logs via `SPEW`, which is compiled out of the
    default RelWithDebInfo build (`_ARMOR` is Debug-only), so a user who hits
    it in a shipping build gets no feedback — consider `SPEWALWAYS`/`printf`
    if visibility matters. Not started. — **SONNET** (well-scoped; reuses
    existing mode-enumeration code).
10. **Audit pre-existing clang-tidy warnings**: the pre-push hook has been
    running clang-tidy advisory-only (doesn't block pushes) since the
    GitHub-remote task, and every recent PR's build log has been full of
    warnings on files the PR didn't touch. We've been treating these as
    "pre-existing, unrelated, ignore" one PR at a time without ever
    triaging the backlog. Not started.

    **Lead review (2026-07-20) ran the config and measured the pile — it
    is not one uniform backlog. Split the work by warning class, because
    the two highest-volume checks are exactly this port's two documented
    bug classes, so the *judgment* (is this site safe?) is the hard part
    there, not the fix. Do NOT assign a single model to the whole task.**

    Representative per-file counts and model assignment:

    - **Noise tier — SONNET** (categorize + dispose via `.clang-tidy`
      suppressions with a documented rationale, mirroring the curation the
      config already models; low correctness stakes):
      `bugprone-macro-parentheses` (~78–91/file, the Win32 `__stdcall`/
      `DWORD` shim macros), `performance-unnecessary-value-param`
      (~17–34), `bugprone-switch-missing-default-case` (~2–16),
      `bugprone-branch-clone` (~3–13).
    - **Correctness tier — OPUS** (per-site reading with the ARM64/LP64
      and non-virtual-dispatch hazard model in mind; a wrong "safe"
      verdict here silently *reintroduces* a bug the check caught, so this
      is the judgment-heavy core, not cheap bucketing):
      - `bugprone-narrowing-conversions` (~130–300/file) — `long→int`,
        `unsigned long→long`, `DWORD→int` truncation, the exact LP64/ARM64
        class behind the ENGINEERING_LOG `unsigned long`-overload battles,
        mixed in with harmless `double→float` graphics math that must be
        told apart from it.
      - `bugprone-derived-method-shadowing-base-method` (~150–170/file) —
        **same category as the GOSImagePool non-virtual-destructor bug**
        (ENGINEERING_LOG 2026-07-16): mostly intentional Singleton/CRTP
        patterns, but a base-pointer call to a shadowed non-virtual method
        silently runs the wrong version. Not safe to bulk-suppress unseen.
      - the low-count long tail, each worth a real look:
        `bugprone-infinite-loop`, `bugprone-integer-division`,
        `bugprone-incorrect-roundings`, `bugprone-signed-char-misuse`,
        `bugprone-unhandled-self-assignment`,
        `bugprone-implicit-widening-of-multiplication-result`.
    - **FABLE** — only if OPUS triage confirms a *live* truncation/dispatch
      corruption bug rather than a benign-but-flagged conversion.

    If forced to pick one model for the whole task instead of splitting:
    **OPUS** — the narrowing-conversion analysis is the crux and getting it
    wrong reintroduces the port's signature bug, which outweighs the credit
    savings of running it on SONNET.

    Aside for whoever picks this up: tidy also reports ~1
    `clang-diagnostic-error` per file under the hook's invocation — likely
    a header-filter/standalone-compile artifact, but glance at it so the
    advisory hook isn't silently degraded.

19. ~~**Fix swapchain binary-semaphore reuse (vk)**~~ **DONE (2026-07-30).**
    Fixed with one render-completion semaphore per swapchain image, indexed by
    the acquired image index (Khronos option (a); `VK_KHR_swapchain_maintenance1`
    rejected to avoid a MoltenVK extension dependency). The
    `VUID-vkDestroyDevice-device-05137` leak was fixed in the same pass: the vk
    draw engine had **no teardown at all**, so ~200 objects (pipelines, shader
    modules, samplers, layouts, descriptor pool, ring, dummy image/UBO, 40 live
    textures) were alive at `vkDestroyDevice`. New `engineDestroy()` /
    `graphics::vk_destroy_draw_engine()` mirrors `engineInit` in reverse.
    **Verified: zero validation errors or warnings**, baseline-vs-fixed under
    `VK_LAYER_KHRONOS_validation`; five clean-quit cycles, unchanged rendering,
    GL build unaffected. Full write-up in ENGINEERING_LOG (including a trap
    about the anonymous namespace in `gameos_graphics.cpp`). Task 3's
    intermittent teardown SIGSEGV did **not** reproduce in those five runs, but
    it is intermittent — this does not close task 3.

    Original description follows.

    The Khronos validation layer
    flags `VUID-vkQueueSubmit-pSignalSemaphores-00067` every frame — a render-
    completion semaphore is signalled again while the swapchain may still be
    using it from a prior present, because the code reuses one semaphore
    instead of indexing per swapchain image. Real presentation-sync bug: it
    risks tearing, flicker or a hang, and it is undefined behaviour regardless.
    Found 2026-07-27 while running validation during the pavement-holes hunt
    (unrelated to that bug; see ENGINEERING_LOG). Fix is either **a semaphore
    per swapchain image**, indexed by the acquired image index, or
    `VK_KHR_swapchain_maintenance1` to fence the present. Khronos guidance:
    <https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html>.
    Repro: run any mission under
    `VK_LAYER_PATH=$VULKAN_SDK/share/vulkan/explicit_layer.d
    VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` and read stderr — should be
    **zero** validation errors when fixed. The same run also reports
    `VUID-vkDestroyDevice-device-05137` (objects still alive at teardown);
    worth cleaning up in the same pass, and it may be related to the open
    audio-teardown crash in task 3. — **OPUS** (sync/lifetime reasoning).

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

### Doc hygiene (batch when convenient)

**Note on task numbering:** numbers are allocation order, not document order —
task 19 sits physically between 10 and 11 because it was appended to the "Soon"
section after the fact. Don't renumber to tidy it up: the numbers are cited from
ENGINEERING_LOG entries, commit messages and PR descriptions, so renumbering
would silently invalidate those references. Check for *duplicates* after a
merge; ignore ordering.

17. ~~**Fix a drifted line-reference in ENGINEERING_LOG**~~ **DONE
    (2026-07-30).** The 2026-07-18 AD-4 entry cited
    `code/mechcmd2.cpp:2689` for `Environment.checkCDForFiles`; by the time
    this was picked up it had drifted twice more (2701 → **2708**), which is
    the whole argument against citing line numbers for a moving target. Fixed
    by citing the **symbol** instead, with the drift history kept as a warning.
    Same treatment applied to task 4's `txmmgr.cpp` FIXME citation (:1412 →
    :1468 after PR #7), now anchored on the comment text.

    **Standing practice from this:** cite a function or symbol name and let the
    reader grep. A line number in a doc is only accurate on the day it is
    written.

### Bugs found during playtesting

18. ~~**Mech shadows swing wildly with small heading changes**~~ **CLOSED
    2026-07-30 — original-engine shadow quality, wontfix as a port task.**
    jalance play-compared GL vs vk and found shadows equally bad on both
    (no DX build available to compare against); his call is that the engine's
    shadowing is simply poor rather than defective in our port.

    **The 2026-07-20 double-yaw hypothesis was refuted** on a timeboxed code
    read the same day — `s_lightDir` is in *shape* space, so
    `RotateLight(s_lightDir, rotation)` in `TG_Shape::MultiTransformShadows`
    is the correct inverse of the yaw in `worldToShape`, and its sign matches
    Stuff's `BuildRotation` under the row-vector convention. **Removing that
    call, as the old entry proposed, would introduce a yaw-coupled shadow bug
    rather than fix one.** Full argument in ENGINEERING_LOG 2026-07-30; the
    2026-07-20 entry is annotated as superseded.

    Real weaknesses found and *not* pursued: `RotateLight` undoes yaw but never
    pitch/roll, leaving `lightDir.y` contaminated in the `zFactor = up.y /
    s_lightDir.y` length term (a much better fit for "swings and rescales");
    `shadowOrigin` in `TG_MultiShape::TransformMultiShape` is built pitch-only
    and never used (dead code, likely an abandoned MS attempt at that same
    pitch handling); and the projection assumes flat ground at the unit's own
    elevation, so slopes are wrong by construction. A genuine fix is a
    shadow-projection rewrite, not a one-liner — if it ever happens it belongs
    in the mod/option bucket per the original-engine-wart rule, at jalance's
    discretion, not in parity work.

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
