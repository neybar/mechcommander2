# Renderer Abstraction Audit (M2, step 1)

*2026-07-17. Question: where does a Vulkan backend plug in? Answer: behind the
existing `gos_*` API — no new abstraction layer is needed. The OpenGL
implementation is already isolated in five files (~5.5K lines) under
`GameOS/gameos/`, and a Vulkan backend is a drop-in second implementation of
those files, with two small leaks to plug first.*

## Layer map

```
code/ + gui/ + Viewer/          game logic, UI, camera — calls gos_* only
mclib/ (incl. mlr/, ~34K lines) MLR sorts/batches, then also emits gos_* calls
────────────────────────────── gos_* API (GameOS/include/gameos.hpp) ── THE WAIST
GameOS/gameos/
  gameos_graphics.cpp (2991)    gosRenderer + gosTexture/gosMesh/gosFont/
                                gosRenderMaterial/gosBuffer — all draw logic
  utils/gl_utils.{h,cpp} (564)  GL helper wrappers
  utils/shader_builder.cpp(959) GLSL compile/link + hot-reload
  gos_render.cpp (741)          SDL window + GL context (API in gos_render.h
                                is already backend-neutral except init_render_context)
  gameosmain.cpp (268)          frame loop — makes a few raw GL calls (leak, see below)
shaders/                        7 GLSL vert/frag pairs + object_tex + 2 includes
```

**Verified: no game code touches OpenGL directly.** `grep` for GL calls across
the repo hits only the five files above. ~40 files in mclib/code/gui call the
gos draw/state API, but they all converge on the same function set. MLR
(the original Mid-Level Renderer) sits *above* the waist, not below it — it is
platform-independent triangle sorting that ends in `gos_DrawTriangles`.

## The contract a Vulkan backend must fulfill

**Immediate-mode draws** — `gos_DrawQuads/Triangles/Lines/Points` (+
`gos_RenderIndexedArray`): caller passes a CPU-side `gos_VERTEX` array; the
renderer applies the full render-state block and issues one draw per call.
Quads are already expanded to triangle lists CPU-side (`drawQuads`,
gameos_graphics.cpp:1703). `gos_DrawFans`/`gos_DrawStrips`: **zero callers** —
skip them. This matters because Metal/MoltenVK has no quads or triangle fans;
we inherit the expansion for free.

**Render states** — ~30 states (`gos_SetRenderState`), applied lazily at draw
time by `applyRenderStates()` (gameos_graphics.cpp:1488). Only a handful reach
the pipeline: cull (3 values), z-write (2), z-compare (3), alpha blend (5
modes), alpha test (shader flag), filter/address (sampler), textures 0-2.
In Vulkan this becomes a pipeline cache keyed on (blend, depth, cull,
primitive topology, material variant) — low hundreds of combinations worst
case, built lazily.

**Materials** — a fixed, closed set selected from state, not data-driven:
`gos_vertex`, `gos_tex_vertex`, their `_lighted` variants (× alpha-test shader
flag), `gos_text`, `gos_YCbCr` (FMV), `object_tex` (sebi's HW T&L mech path).
Loose `uniform` declarations (e.g. `uniform mat4 mvp`) — for Vulkan these need
UBO/push-constant form, but the shaders are tiny (~20 lines each).

**Retained buffers** — sebi's newer path, only 3 call sites: `tgl.cpp` (static
mech mesh VB/IB + `object_tex`), `txmmgr.cpp` (uniform buffers for lights/
scene), `mc2movie.cpp` (FMV quad). The old DX-style
`gos_CreateVertexBuffer`/`gos_LockVertexBuffer` family has no game callers.

**Textures** — create from TGA file/memory or empty, formats
solid/keyed/alpha/detect, `gos_LockTexture`/`UnLock` for CPU writes,
`gos_UpdateTexture` for streaming (FMV frames). Maps to staging-buffer uploads.

**Fonts/text** — `gosFont` texture atlas + `drawText`; pure consumer of the
texture and quad paths, no extra GPU surface.

**Render-to-texture** — `gos_StartRenderToTexture` is declared in gameos.hpp
but **not implemented in the GL port and never called**. Defer.

**Frame loop** — `beginFrame` (bind VAO, reset draw counter), `endFrame`
(shader hot-reload poll), then gameosmain.cpp clears/viewports/swaps.
Resolution changes go through `gosRenderer::handleEvents` →
`graphics::resize_window` — this is where swapchain recreation slots in, and
the path is already exercised by the menu(800x600)→mission resize.

## Leaks to plug before the backend split (prep work on the GL path)

1. **gameosmain.cpp makes raw GL calls** (glClear/glViewport/glUseProgram/
   glCullFace, lines ~164-183) — move into gosRenderer begin/end-frame so the
   frame loop is backend-agnostic.
2. **gos_render.h context API** — `init_render_context`/`make_current_context`
   are GL-flavored; window creation hardcodes `SDL_GL_*` attributes and
   `SDL_WINDOW_OPENGL`. Needs a backend-selected window flag
   (`SDL_WINDOW_VULKAN`) and a neutral "init device for window" entry point.

Both are small, testable-on-GL refactors — do them first as their own branch.

## Proposed plug-in shape

Build-time backend selection (runtime toggle not worth it yet):

```
GameOS/gameos/rendergl/   gameos_graphics.cpp, gl_utils, shader_builder, GL context
GameOS/gameos/rendervk/   same gos_* symbols implemented on Vulkan
```

CMake option `MC2_RENDERER=GL|VULKAN` picks the directory. The gos_* API,
gameos.hpp, and everything above the waist stay untouched. GL remains the
default until Vulkan reaches mission parity (M2 exit criterion), then retires.

## Vulkan/MoltenVK-specific notes

- Immediate-mode vertex streams → per-frame ring buffer (persistently mapped,
  one per frame-in-flight). The GL path already re-uploads every draw
  (`gosMesh::draw`), so Vulkan won't be architecturally worse.
- One render pass per frame is enough (color + depth); no RTT, no post-FX.
- Point/line draws exist (map overlays, effects) — Metal supports both;
  wide lines don't exist on Metal but the GL path never sets line width.
- MoltenVK via Homebrew (`molten-vk`) or LunarG SDK; watch
  `VK_KHR_portability_subset` (no triangle fans — unused; no wide lines — fine).
- Shader strategy (OQ-4): port the ~9 small GLSL pairs to Vulkan-GLSL
  (explicit bindings, UBO/push constants), compile offline with `glslc` to
  SPIR-V, commit the .spv alongside sources. Hot-reload can come later or be
  dropped — it's a dev convenience polling wall-clock in endFrame.

## Suggested M2 sequencing

1. Prep refactor on GL path: plug the two leaks above (verify game unchanged).
2. Backend split scaffolding: move GL files to `rendergl/`, add CMake option,
   stub `rendervk/` that opens a window and clear-screens via MoltenVK.
3. Port shaders to SPIR-V; build the pipeline cache + per-frame ring buffer.
4. Parity by subsystem, verified with `MC2_AUTOQUIT_SECS=45 ./mc2 -mission
   mc2_01` + screencapture at each stage: textures → UI quads/text (menus) →
   terrain/overlays → mech meshes (`object_tex` retained path) → effects → FMV
   (YCbCr).
5. Full mission parity on Vulkan → retire GL (keep until then).
