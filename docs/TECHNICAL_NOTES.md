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
https://archive.org/details/mech-commander-2-source-code — pristine mirror;
targets the Feb 2006 DirectX SDK, D3D8, VS-era build projects, XNA solution).
Caution: that GitHub mirror is labeled "MIT", which the uploader had no
authority to grant — the real terms remain Microsoft's shared-source license.

### Omnitech (Echelon9/mechcommander2-open) — not a base; modding reference
Moddability-focused fork of the original source: VS2010, Windows-only, still
DirectX, 33 commits on GitHub (real activity happened on forums/ModDB).
Value to us: its loadable-campaign system (MC1 campaign remake, Carver V
variants) is the de facto community standard for custom campaigns — study it
when designing our mod support, ideally stay format-compatible. Cautionary
tale: Omnitech baked difficulty increases into the fork itself, making it
rough on casual players — reinforces our rule that balance/difficulty belongs
in data/mods, never hardcoded in the engine.

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
- *DXVK chain (D3D8 → DXVK → Vulkan → MoltenVK → Metal):* the approach the
  inspiring [Generals Mac port](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad)
  used — keep the original DirectX renderer and translate at runtime. Not viable
  on our chosen base: alariq/mc2 already **removed** DirectX in favor of OpenGL,
  so there is no D3D layer left to translate. It would only apply if we started
  from the original Microsoft source, which we rejected for the much larger
  64-bit/build-system burden (see evaluation above). Revisit only if the alariq
  base proves unworkable. Framing: any port must solve both (a) the renderer API
  and (b) 64-bit/Win32/build modernization. The DXVK chain solves only (a);
  Generals could use it because its community had already spent years on (b)
  (TheSuperHackers → Fighter19 → GeneralsX). For MC2, alariq's port *is* that
  community stack — and it solved (a) by deleting D3D, closing the DXVK door.
- *Zink (GL-over-Vulkan) on MoltenVK:* run alariq's OpenGL renderer unmodified
  through Mesa's Zink → MoltenVK → Metal. Experimental on macOS and adds a
  fragile layer, but it's a potential zero-rewrite bootstrap for M1 (playable
  on Mac before the Vulkan backend exists) and cheap insurance if macOS's
  deprecated GL 4.1 can't run the port's shaders. Worth a one-day spike in M1.

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
- OQ-6: Is Omnitech's custom-campaign format documented anywhere, and can our
  mod support load Omnitech-authored campaigns (MC1 remake, etc.)?
- OQ-7: Exact license text lineage — Shared Source Limited Permissive vs Ms-PL:
  which text actually governs the 2006 release? Pin the authoritative copy in
  the repo before going public.
