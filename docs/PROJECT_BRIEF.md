# Project Brief

## What

A modern, cross-platform port of **MechCommander 2** — the 2001 real-time
tactics game set in the BattleTech universe (command a mercenary Mech force
across three campaigns on Carver V). Microsoft released the source and most
assets in 2006 under a shared-source license; the community has kept it alive
since. This project's contribution is a **native macOS port** and a **modern
Vulkan/Metal rendering path**, building on that lineage.

Background on the game: https://www.sarna.net/wiki/MechCommander_2

## Inspiration

[Generals-Mac-iOS-iPad](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad) —
a native Apple Silicon port of C&C Generals: Zero Hour, built through human–AI
collaboration (Claude Code doing the C++ engineering, a human directing by
symptoms). It proves the model this project uses: build on the community
lineage, keep assets user-supplied, translate the renderer to Metal via the
Vulkan ecosystem, and document the bug hunts in an engineering log. We adopt
its working style but not its iOS/iPad scope — this project targets desktop
platforms only.

## Why

- The maintainer (jalance) wants to play MC2 natively on their Mac.
- No existing port supports macOS: alariq/mc2 covers Windows/Linux via OpenGL,
  and the Restoration Project focuses on Windows 11.
- OpenGL is deprecated on macOS, so "just compile the Linux port" isn't a
  durable answer — a Vulkan renderer (via MoltenVK on Mac) is.

## Goals, in priority order

1. **Runs natively on Apple Silicon macOS** — the founding motivation.
2. **Linux and Windows stay first-class** — one codebase, three platforms.
3. **Modern renderer** — Vulkan everywhere, MoltenVK translating to Metal on Mac.
4. **Mod-friendly** — preserve compatibility with the existing mod ecosystem
   (moddb.com/games/mech-commander-2); make modding easier where cheap to do.
5. **Clean-hands asset policy** — users supply their own game data (retail copy
   or Microsoft's shared-source asset release); the repo ships engine only.

## Non-goals (for now)

- Multiplayer/networking (was stripped from the source release; out of scope).
- Commercial anything — this is free software built on others' free work.
- Gameplay changes or remastering. Faithful port first; enhancements later.
- Mobile platforms.

## People

- **jalance** — project owner, reviewer. Claude drives implementation.

## Lineage and credit

- FASA Interactive / Microsoft — original game and 2006 source release.
- [alariq/mc2](https://github.com/alariq/mc2) — OpenGL/SDL2/CMake port for
  Windows and Linux; our base codebase. Independent fork only: that project
  does not accept AI-generated contributions, so nothing flows upstream from here.
- [MechCommander2-Restoration-Project](https://github.com/Alexbeav/MechCommander2-Restoration-Project)
  — Windows 11 fork of alariq's port; useful reference for its FFmpeg-based
  video pipeline (Bink codec replacement), GPL v3 so code can be borrowed with credit.
- [Omnitech](https://github.com/Echelon9/mechcommander2-open) — moddability-focused
  variant; reference for mod-ecosystem expectations.
