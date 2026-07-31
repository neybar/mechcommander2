# Building

## macOS (Apple Silicon) — primary target

Prerequisites (Homebrew):

```sh
brew install cmake sdl2 sdl2_mixer sdl2_ttf glew
```

Notes:
- `sdl2` installs `sdl2-compat` (the SDL2 API over SDL3) on current Homebrew — that's fine.
- zlib and OpenGL come from the macOS SDK. OpenGL is deprecated-but-present,
  which is why the Vulkan backend exists — see below.

Build (OpenGL, the default backend):

```sh
cmake -B build
cmake --build build -j8
```

Produces `build/mc2` (arm64 Mach-O), plus the `viewer` and asset tools
(`makefst`, `makersp`, `pak`).

### Vulkan / MoltenVK

The Vulkan backend is playable (M2 — see `CLAUDE.md` for exactly how far along
it is). Both backends build from the same tree; `MC2_RENDERER` picks which
`GameOS/gameos/render*/` implementation is compiled in. Everything above the
`gos_*` API is backend-agnostic.

```sh
brew install molten-vk vulkan-headers vulkan-loader
cmake -B build-vk -DMC2_RENDERER=VULKAN
cmake --build build-vk -j8
```

Both configurations produce an executable named `mc2`, so keep them in separate
build dirs. The convention in this project is to deploy the Vulkan one as
`mc2-vk` alongside the GL `mc2` in the game directory, which is what the docs,
`tools/vkprobe/run.sh` and the engineering log all assume:

```sh
cp build-vk/mc2 ~/Games/mc2-port/mc2-vk
```

Shaders are **pre-compiled SPIR-V checked into the repo** (`shaders/vk/*.spv`,
built from the GLSL beside them with `glslc`); there is no shader compile step
in CMake. The game loads them as `shaders/vk/<name>.spv` **relative to its
working directory**, so `shaders/` has to be present in the game dir next to
`data/`. A stale or missing `.spv` there makes the renderer fail to initialise
and draws become no-ops.

To check a Vulkan change against the Khronos validation layer — the backend is
expected to run with **zero** errors or warnings, so any output is a regression:

```sh
cd ~/Games/mc2-port
VK_LAYER_PATH=$VULKAN_SDK/share/vulkan/explicit_layer.d \
  VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
  MC2_AUTOQUIT_SECS=25 ./mc2-vk -mission mc2_01
```

(`MC2_AUTOQUIT_SECS` matters — without it that's a fullscreen run with no way
out. See `CLAUDE.md` for the full dev-hook table.)

Running the game needs user-provided assets (retail install or the
shared-source data set) — see docs/PROJECT_BRIEF.md. By default mc2 looks
for a `data/` directory in its current working directory (so launching
from inside your asset install "just works", as before). To run it from
anywhere else, point it at your asset directory one of two ways:

```sh
build/mc2 -assetdir ~/Games/mc2-port
# or
MC2_ASSET_DIR=~/Games/mc2-port build/mc2
```

(`-assetdir` wins if both are set.) If the directory doesn't exist or
doesn't contain a `data/` subdirectory, mc2 prints a message explaining
what's wrong and exits, instead of launching into a broken state.

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
