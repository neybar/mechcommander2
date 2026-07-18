# MechCommander 2 — a modern, cross-platform port

A rebuild of **MechCommander 2** (FASA Interactive / Microsoft, 2001), the
real-time tactics game set in the BattleTech universe, targeting **macOS
(Apple Silicon) first**, then Linux and Windows. Built on Microsoft's 2006
shared-source release, via [alariq's OpenGL/SDL2/CMake port](#lineage--credit).

Background on the game: https://www.sarna.net/wiki/MechCommander_2

## Status

- **M0/M1 — done.** Builds and runs natively on Apple Silicon macOS; the
  campaign is playable start to finish (movement, combat, win triggers,
  campaign progression).
- **M2 — in progress.** Porting the renderer to **Vulkan** (MoltenVK on
  macOS) so the project isn't stuck on deprecated OpenGL. Menus and
  in-mission rendering (mechs, lighting, FMVs) already run on the new
  Vulkan backend; the OpenGL path stays in place until Vulkan reaches full
  parity.
- **M3/M4** — Linux/Windows builds, audio completion, packaging. See
  [`docs/ROADMAP.md`](docs/ROADMAP.md) for the full milestone breakdown and
  [`docs/ENGINEERING_LOG.md`](docs/ENGINEERING_LOG.md) for the bug-hunt
  history.

## Assets are not included

**This repo ships engine code only — no game data, ever.** MechCommander 2
assets (art, sound, missions) are copyrighted and not redistributed here.
To run the game you need your own copy of the assets, either from a retail
install or Microsoft's shared-source asset release. See
[`BUILDING.md`](BUILDING.md) for how to point the engine at your asset
directory.

## Modding

Two decades of community mods exist for MC2 (see
[moddb.com/games/mech-commander-2](https://www.moddb.com/games/mech-commander-2)).
This project aims to preserve compatibility with existing mod formats
rather than break them without cause.

## Building

See [`BUILDING.md`](BUILDING.md) (macOS, primary target) and
[`BUILD-WIN.md`](BUILD-WIN.md) (Windows, inherited from upstream — not
yet verified against this fork's CMake changes).

## A note on AI-assisted development

This project is developed with heavy use of Claude Code: an AI assistant
drives most of the implementation, with a human (the maintainer) directing,
reviewing every change, and doing the actual playtesting. If that's not
something you want in a codebase, this project may not be for you — and
that's exactly why our patches never flow upstream to alariq's port, whose
policy explicitly excludes AI-generated contributions (see below). We think
it's important to be upfront about this rather than quietly ship AI-written
code under a "just a contributor" byline.

## Lineage & credit

This project stands entirely on the work of others. In rough order of
how directly we depend on it:

- **[alariq/mc2](https://github.com/alariq/mc2)** — the OpenGL/SDL2/CMake
  port for Windows and Linux that this project forks as its base codebase.
  This is **one person's solo project**, not a community effort — there's
  no guarantee of continued upstream development, which is exactly why we
  forked an independent base rather than depending on it staying alive.
  alariq did the hard work of getting a 20-year-old DirectX codebase
  running on modern OpenGL; without that, this project doesn't exist.
  **We do not submit patches upstream** — alariq/mc2's policy prohibits
  AI-generated contributions, and we respect that by staying strictly
  downstream. Credit flows one direction: from us to him.
- **FASA Interactive / Microsoft** — the original 2001 game, and the 2006
  shared-source release that made any of this possible at all.
- **[MechCommander2-Restoration-Project](https://github.com/Alexbeav/MechCommander2-Restoration-Project)**
  — a Windows 11-focused fork of alariq's port; useful reference for its
  FFmpeg-based video pipeline (replacing the original Bink codec).
- **[Omnitech](https://github.com/Echelon9/mechcommander2-open)** — a
  moddability-focused variant of the codebase; reference for what the mod
  ecosystem expects.
- **[Generals-Mac-iOS-iPad](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad)**
  — a native Apple Silicon port of C&C Generals: Zero Hour built through
  human–AI collaboration. This project borrows its working model (build on
  the community lineage, keep assets user-supplied, translate the renderer
  via Vulkan/Metal, keep an engineering log) directly.

## Licensing

- **Original engine code**: Microsoft's [Shared Source Limited Permissive
  License](EULA.txt) — non-commercial use only. This project is, and will
  stay, free.
- **New code** (ours and alariq's port): **GPL v3** — see [`license.txt`](license.txt).
- Third-party libraries (SDL2, GLEW, etc.) retain their own licenses.

This project is not affiliated with, and is not endorsed by, Microsoft,
FASA Interactive, or Smith & Tinker.
