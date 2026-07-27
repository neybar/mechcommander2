# Brief for the Xcode Metal frame debugger session

Paste this into the Claude chat inside Xcode. It is self-contained — it assumes
no knowledge of the mc2 repo.

---

## What you're looking at

`apron_095408.gputrace` is a Metal frame capture from **MechCommander 2**
(2001 RTS, Microsoft shared-source), running as a modern macOS ARM64 port.
The renderer is **Vulkan via MoltenVK** on an **Apple M4 Pro**. The capture was
taken headlessly with `MVK_CONFIG_AUTO_GPU_CAPTURE_*` (no Xcode involved), so
everything you see is MoltenVK-generated Metal, not hand-written Metal.

Trace: `docs/bugs/2026-07-27-metal-capture/apron_095408.gputrace`
Drawable: **6016 × 3384** (Retina 2×). The game's own coordinate space
("gos space") is **2048 × 1080**; multiply gos → render by ~2.937 in x.

## The bug

The ground plane (pavement / city blocks — never natural terrain) shows two
defects **on the Vulkan build only**. The OpenGL build of the same engine, same
scene, same camera is clean.

1. **A large black band** across the pavement. In the captured frame it occupies
   roughly render-space **x ∈ 3000..5600, y ∈ 1850..2250**; a good solid sample
   point is **render (4300, 2050)**.
2. **Fog-white square holes** in the pavement. One is at gos (684, 75) =
   render **(2009, 234)**, near the building marked "13".

Both are stable frame to frame and sweep across the ground as the camera pans.

## What is already ruled out (please don't re-derive these)

Seven hypotheses have been refuted from the CPU/API side:

- The submitted geometry, projection matrix and shaders are **byte-identical
  between the OpenGL and Vulkan backends** (verified by dumping and diffing the
  terrain vertex stream on both). So this is not a geometry or transform bug.
- Depth clip vs depth clamp (`VK_EXT_depth_clip_enable`) — no change.
- Negative-height viewport (Y-flip via viewport) vs Y-flip in the projection
  matrix with a positive-height viewport — no change.
- Vertex ring-buffer aliasing — impossible by construction.
- Drawing an opaque base layer under the pavement — made it worse.
- **Vulkan validation layer**: clean. Only a swapchain semaphore-reuse warning
  and a teardown leak. Zero draw / pipeline / state errors.
- **Metal API validation**: clean. Zero errors, zero warnings.
- **Guard-band / large-coordinate rasterization**: refuted. The engine feeds
  Direct3D-style `XYZRHW` pre-transformed vertices that reach far off-screen
  (x ∈ −2781..+5203 in the 2048-wide space). We added a CPU Sutherland-Hodgman
  clip of every triangle to the viewport before submit — it drops ~21% of all
  triangles as fully off-screen and clips ~5% more, so nothing reaching Metal
  extends past screen+64px. The resulting frame is **pixel-identical**. Dead end.

## The one fresh observation

Cropping the black band shows it is bounded by a **clean straight diagonal
edge**, and buildings, crates and HUD elements draw correctly *on top* of it.
A straight edge implies **rasterized primitive coverage** — something is
actively drawing black/fog-colored geometry there — rather than the pavement
failing to draw. Earlier work had inferred "no fragment runs here" from the
depth buffer reading 0.99999 at a hole, which points the opposite way. Those two
readings need reconciling and that is the most valuable thing you can settle.

## Questions to answer, in priority order

1. **Debug Pixel at render (4300, 2050)** (black band) and at **(2009, 234)**
   (white hole). For each: what is the *full draw history* for that pixel? Does
   any fragment shader execute? If yes, what does it return and what is the
   final blend? If no, what is the depth value and which draw last wrote it?
2. If a fragment *does* run and returns black or fog-white: **identify that
   draw** — its pipeline, bound textures, blend state, and its vertex color
   input. The engine bakes fog into a per-vertex color; a bug there would show
   as exactly this. Report the texture name if one is bound.
3. If **no** fragment runs: use the **Geometry inspector** on the terrain draws
   whose bounding boxes cover that pixel. Are the covering triangles present in
   post-clip geometry? Are they degenerate, back-facing, or culled? (Watch the
   known geometry-inspector NDC/frustum display quirk, Apple forum thread
   773786 — trust actual coverage over the 3D view.)
4. Compare a **working** pavement pixel against a **broken** one in the same
   frame — same draw call if possible. The delta between two pixels of one
   primitive is the highest-signal evidence available.
5. Anything anomalous MoltenVK is doing in the encoder: unexpected render-pass
   splits, load/store actions that discard, `MTLStoreActionDontCare` on a
   still-needed attachment, or a memoryless/tile attachment being reused.

## Useful context on the draw path

The pavement is drawn through Direct3D-legacy `XYZRHW` pre-transformed vertices.
MoltenVK sees a vertex shader that does:

```glsl
vec4 p = projection * vec4(pos.xyz, 1);
gl_Position = p / pos.w;      // pos.w carries D3D's rhw (= 1/w)
```

so `gl_Position.w` ends up as `1/rhw`. Blend state on the offending pavement
draws is `AlphaInvAlpha` (alpha blending) even though the pavement is opaque —
that is inherited engine behavior, present on the working OpenGL path too, so it
is suspicious but not by itself the cause.

Please report findings as: which draw call index, what state, and what you
observed at the pixel — concrete over interpretive.
