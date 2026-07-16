# Roadmap

Milestones are sequential; each ends in something verifiable. **V1 = M0**
per the project owner: a clean build on macOS is the first success condition.

## M0 — Builds cleanly on macOS (Apple Silicon)  ← current target

- [ ] Vendor alariq/mc2 into this repo (record upstream SHA; decide OQ-1)
- [ ] Inventory build prerequisites; get deps via Homebrew or vendored (SDL2, etc.)
- [ ] CMake configures on macOS ARM64
- [ ] Full compile + link succeeds (stubbing subsystems is acceptable, but every
      stub gets a tracking note)
- [ ] Document the build in CLAUDE.md and a BUILDING.md
- Exit criterion: `cmake -B build && cmake --build build` produces an `mc2`
  binary on the M-series Mac, from a clean checkout.

## M1 — Boots and renders on macOS (OpenGL bootstrap)

- [ ] Asset-directory configuration + friendly missing-assets message (AD-4)
- [ ] Game reaches the main menu with user-provided assets
- [ ] One mission loads and is playable (FMVs may be skipped — Bink, see hazards)
- [ ] Triage list of what's broken (audio, input, rendering artifacts)
- Exit criterion: play a mission start-to-finish on the Mac.

## M2 — Vulkan renderer (the real graphics work)

- [ ] Audit the renderer abstraction boundary in the codebase (gos/MLR layers)
- [ ] Vulkan backend behind that boundary; MoltenVK on macOS
- [ ] Shader translation strategy decided (OQ-4) and implemented
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
