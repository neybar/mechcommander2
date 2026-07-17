# Roadmap

Milestones are sequential; each ends in something verifiable. **V1 = M0**
per the project owner: a clean build on macOS is the first success condition.

## M0 — Builds cleanly on macOS (Apple Silicon)  ✅ complete (2026-07-16)

- [x] Vendor alariq/mc2 into this repo (record upstream SHA; decide OQ-1)
- [x] Inventory build prerequisites; get deps via Homebrew or vendored (SDL2, etc.)
- [x] CMake configures on macOS ARM64
- [x] Full compile + link succeeds (stubbing subsystems is acceptable, but every
      stub gets a tracking note)
- [x] Document the build in CLAUDE.md and a BUILDING.md
- Exit criterion: `cmake -B build && cmake --build build` produces an `mc2`
  binary on the M-series Mac, from a clean checkout.

## M1 — Boots and renders on macOS (OpenGL bootstrap)  ✅ complete (2026-07-17)

- [ ] Asset-directory configuration + friendly missing-assets message (AD-4)
      — *carried forward: game must be launched from the game dir; no friendly
      error yet. Revisit alongside the GitHub/public-readiness work.*
- [x] Game reaches the main menu with user-provided assets
- [x] One mission loads and is playable (FMVs may be skipped — Bink, see hazards)
- [x] Triage list of what's broken (audio, input, rendering artifacts)
      — see ENGINEERING_LOG 2026-07-17: playthrough observations all triaged
- Exit criterion: play a mission start-to-finish on the Mac. **Met**: user
  played Training 1 & 2 to completion (movement, combat, win triggers,
  campaign progression all working); unattended `-mission` quickstart runs
  clean.

## M2 — Vulkan renderer (the real graphics work)  ← current target

- [x] Audit the renderer abstraction boundary in the codebase (gos/MLR layers)
      — docs/RENDERER_AUDIT.md: the gos_* API is the waist; backends live in
      GameOS/gameos/render{gl,vk}/ behind `cmake -DMC2_RENDERER=...`
- [ ] Vulkan backend behind that boundary; MoltenVK on macOS
      — *scaffold done: MoltenVK clears/presents inside the real game loop;
      null gos_* backend runs the game logic. Parity work next.*
- [ ] Shader translation strategy decided (OQ-4) and implemented
      — *leaning: port the ~9 small shaders to Vulkan-GLSL, offline glslc*
- [ ] OpenGL path kept until Vulkan reaches parity, then retired
- Exit criterion: same mission plays on the Vulkan backend on all three platforms.

## M3 — Cross-platform parity & polish

- [ ] CI-style build verification for Linux and Windows
- [ ] Audio completed (OQ-5)
- [ ] FMV playback via FFmpeg (adapt Restoration Project's GPL work, with credit)
- [ ] Full campaign playable
- Exit criterion: full campaign on Mac, Linux, and Windows.

## M4 — Community readiness (when going public)

- [ ] Licensing/credits documentation airtight (Shared Source + GPL v3 + provenance)
- [ ] Mod compatibility verified against popular moddb mods
- [ ] Packaging: macOS .app (signing/notarization), Linux tarball/Flatpak, Windows zip
- [ ] Public GitHub repo with honest README about AI-assisted development

## Deliberately unscheduled

Networking/multiplayer, gameplay enhancements, editor revival, mobile.
