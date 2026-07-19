# Building

## macOS (Apple Silicon) — primary target

Prerequisites (Homebrew):

```sh
brew install cmake sdl2 sdl2_mixer sdl2_ttf glew
```

Notes:
- `sdl2` installs `sdl2-compat` (the SDL2 API over SDL3) on current Homebrew — that's fine.
- zlib and OpenGL come from the macOS SDK. OpenGL is deprecated-but-present;
  the Vulkan/MoltenVK renderer is milestone M2 (see docs/ROADMAP.md).

Build:

```sh
cmake -B build
cmake --build build -j8
```

Produces `build/mc2` (arm64 Mach-O), plus the `viewer` and asset tools
(`makefst`, `makersp`, `pak`).

Running the game needs user-provided assets (retail install or the
shared-source data set) — see docs/PROJECT_BRIEF.md. Asset setup is part of
milestone M1 and not wired up yet.

## Linux

Should build the same way with distro packages for SDL2, SDL2_mixer, SDL2_ttf,
GLEW, zlib (this is upstream alariq/mc2's home platform). Not yet re-verified
in this fork.

## Windows

See `BUILD-WIN.md` (upstream's instructions). Not yet re-verified in this
fork — note it references `3rdparty.zip`, which this fork removed as dead
weight (unused Windows prebuilt binaries, see ENGINEERING_LOG.md); get it
from alariq's upstream repo if reviving the old `.vcproj` path.

## Git hooks

Optional local pre-push hook (build check + advisory clang-tidy) — see
`tools/hooks/README.md`.
