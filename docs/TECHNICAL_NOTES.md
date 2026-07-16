# Technical Notes

## Codebase evaluation (2026-07-16)

Three candidate starting points were evaluated:

### alariq/mc2 — **chosen base**
https://github.com/alariq/mc2

- Windows + Linux, 64-bit, OpenGL, SDL2, CMake. 426 commits, v0.1.3 (May 2026).
- The hard, boring work is already done: DirectX removal, 64-bit cleanup,
  cross-platform build. This is the shortest path to a macOS build.
- Gaps: sound system partially incomplete; networking unimplemented; maintainer
  has marked the project "finished for now."
- **Constraint:** upstream prohibits AI-generated code contributions. We fork
  and diverge; we do not send patches back.
- Licensing: original code under Microsoft Shared Source Limited Permissive
  License (non-commercial); port's new code under GPL v3. Our new code: GPL v3.
- Assets live in a separate data repo maintained by alariq — evaluate whether
  to point users at it or only at retail/shared-source data (M0 question).

### Original Microsoft 2006 release — rejected as base
Would mean redoing years of DirectX-removal and 64-bit work that alariq
already finished. Kept as a reference for anything the port changed
(https://github.com/SimonDarksideJ/MechCommander2-Source,
https://archive.org/details/mech-commander-2-source-code).

### Alexbeav Restoration Project — rejected as base, valuable as reference
Fork of alariq/mc2 focused on Windows 11; no macOS interest. But it solved a
problem we will hit: replacing the end-of-life **Bink video codec** with an
FFmpeg/MP4 pipeline. GPL v3, so we can adapt that work with credit.

## Architecture decisions

### AD-1: Fork alariq/mc2, vendor it into this repo
Keep upstream's history importable (subtree or plain vendor + recorded SHA).
Keep our diffs cleanly separable from vendored code.

### AD-2: Renderer — Vulkan, with MoltenVK on macOS
Chosen over the alternatives:
- *Keep OpenGL:* works today on macOS 4.1 (deprecated) but is a dead end, and
  Apple's GL drivers are frozen. Acceptable only as a bootstrap.
- *bgfx/sokol wrapper:* another abstraction layer over a codebase that already
  has its own renderer abstraction (`gosFX`/MLR layers from the original engine);
  more churn than value here.
- *Vulkan + MoltenVK:* one modern API for Linux/Windows, translated to Metal on
  Mac — the same model dxvk uses for D3D→Vulkan. MoltenVK is mature (all Vulkan
  games on Mac use it via VKD3D/DXVK stacks).

Bootstrap sequence: get the existing OpenGL renderer compiling on macOS first
(GL 4.1 core caveats: the port may use GL features/extensions macOS lacks —
audit early), then port the renderer backend to Vulkan as its own milestone.

### AD-3: C++ + CMake, modernize incrementally
Match the vendored code's era where touching it (it's early-2000s C++), and
modernize only what we rewrite. No wholesale reformatting — keeps diffs against
upstream reviewable.

### AD-4: Assets are user-provided
The engine looks for game data in a user-configured directory. First-run
experience should detect missing assets and tell the user exactly what to
provide (retail CD install files, or Microsoft's 2006 shared-source asset set).
No game data, including "small test fixtures" derived from it, enters git.

### AD-5: Mod compatibility is a constraint, not a feature
File-format readers (`.pak`, `.fit`, `.abl` scripts, terrain, etc.) must keep
accepting the files the modding community produces. Format changes require a
documented migration path.

## Known hazards for Apple Silicon

The codebase is 2001-era Windows x86 C++ that alariq ported to x86_64
Linux/Windows. ARM64 macOS adds:
- Alignment: ARM64 is stricter about unaligned access patterns that x86 tolerates
  (the asset loaders do struct-overlay reads of packed file data).
- Any residual x86 intrinsics/assembly must be replaced or stubbed.
- macOS-specific: case-sensitive vs insensitive filesystem assumptions in asset
  paths; SDL2 via Homebrew or vendored; code-signing/notarization eventually.
- Bink video files won't play (codec is x86 Windows-era) — mirror the
  Restoration Project's FFmpeg approach or skip FMVs initially.

## Open questions (resolve during M0/M1)

- OQ-1: Subtree vs plain vendor for upstream import?
- OQ-2: Does alariq's separate asset-data repo have licensing we're comfortable
  pointing users at?
- OQ-3: What GL version/extensions does the port actually require vs what
  macOS 4.1 core provides?
- OQ-4: Shader story for the Vulkan port — hand-written GLSL→SPIR-V, or adopt
  the existing shaders with minimal translation?
- OQ-5: Audio backend state — how incomplete is upstream's sound system, and is
  SDL_mixer/OpenAL the fix?
