# Vulkan cement/pavement "holes" — investigation findings (OPEN)

**Status:** open, heavily localized. No fix yet. Branch `fix/vk-black-quad-diag`.
**Last worked:** 2026-07-24.

## Symptom

On the **Vulkan build only**, pavement/city-block ground shows sharp
**fog-colored ("white") square holes** plus a **black band** that *sweeps across
the empty squares as the camera pans* (user's description: "like the camera is
holding up a cloud that blocks the light"). Near building 13 / the drop zone in
Mission 1. **GL is clean at the same spot** (user-confirmed first-hand;
comparison screenshots exist in `docs/bugs/2026-07-20-task4/`).

User's field observation (load-bearing): holes appear **only in pavement / city
blocks**, never in natural terrain, across the training maps + Mission 1.

## What it actually is (measured, high confidence)

Via Xcode Metal frame capture (`docs/bugs/2026-07-23-metal-capture/apron.gputrace`,
read in full Xcode — the capture tooling is committed, see below):

- The "white" squares are **holes**: at an apron pixel (render coord ~2009,234,
  drawable is native **6016×3384**) the color is `(0.80,0.78,0.80)` = the exact
  fog color, and **depth = 0.99999** = the cleared/far value. Nothing terrain
  wrote depth there. Debug Pixel is **greyed** (no fragment) at that pixel.
- The hole is filled by the **sky**: `data/tgl/128/sky07_back_left.tga` is drawn
  **opaque (am=1), z=1.0, zwrite=1**, in `fogState=0xcdc8cc` (= the apron color).
  With `LessEqual` depth it only survives where nothing nearer drew — i.e. in the
  holes. So sky-through-hole = fog-colored square. The **black band** is the same
  holes showing the cloud/haze layer, moving with the camera.
- Where cement/terrain *does* draw, it draws **correctly**: 3 separate Debug
  Pixels all showed dark pavement (`c ≈ 0.30–0.35`, depth ~0.75, `FogValue=1.0`,
  no discard). So it is **not** a shading/fog/texture/alpha bug on drawn tiles.

## Why only pavement (explained)

`mclib/quad.cpp:1506` — a **pure cement tile** (`isCement`, no detail, no overlay)
is submitted **only** via the `MC2_ISCRATERS` path, with **no `MC2_DRAWSOLID`
base layer under it**:

```c
if (detail==0xffffffff && overlay==0xffffffff && isCement)
    addVertices(..., MC2_ISTERRAIN | flags | MC2_ISCRATERS);   // cement: ONE layer
else if (terrainHandle!=0)
    addVertices(..., MC2_ISTERRAIN | MC2_DRAWSOLID);           // everything else
```

Regular terrain always draws a solid base, so it never holes. Cement has **no
fallback** — if its coverage fails, you get bare depth buffer → the z=1 sky fills
it. That is exactly why holes appear only in pavement/city blocks.

## The draw path

1. `ISCRATERS` nodes are drawn in `mclib/txmmgr.cpp:1164` (the `Renderer != 3`
   block — NB `Environment.Renderer == 0` on **both** backends, so all
   `Renderer == 3` blocks are dead code here) via
   `gos_RenderIndexedArray(vertices, n, indexArray, n)`. `indexArray` is identity
   `[0,1,2,…]` (`txmmgr.cpp:168`). Both cement AND solid terrain use this same
   call — the difference is only the render **state** each block sets.
2. Cement runs under leftover **`AlphaMode = AlphaInvAlpha` (am=3)** (set at
   `txmmgr.cpp:1105` for the DRAWALPHA overlays and never reset in the cement
   block), plus `ZWrite=1, ZCompare=1, AlphaTest=0`.
3. GL impl → `gosRenderer::drawIndexedTris` (`rendergl/gameos_graphics.cpp:1850`).
   vk impl → `rendervk/gameos_graphics.cpp:2030`: expands indices to a flat tri
   list, `emitDraw(SHADER_TEX_VERTEX, TOPO_TRIS, …)`.
4. vk vertex shader (`shaders/vk/gos_vertex.vert`) and the immediate projection
   matrix (`updateProjection`, identity-Z) are **byte-identical** to GL's. The
   `[0,1)` CPU depth cull in `quad.cpp:1496` and `Camera::projectZ` are
   backend-independent → the **same** cement triangles are submitted on both.

## Ruled OUT (do not re-chase)

- Shading/fog `mix`, texture content, **alpha-test discard** (cement is
  `AlphaTest=0`; and a discarded/blended fragment under `ZWrite=1` would still
  write depth — the hole depth is *unwritten*, so no fragment ran there).
- **Ring-buffer overflow** — frame completes clean under `MC2_VK_DEBUG=1`, no
  "draws dropped" line.
- **Degenerate vertices** — `MC2_VK_RIALOG` probe found **no** NaN/huge/oob
  coords over the apron; z∈[0.72..], rhw sane.
- **CPU culling / projection / vertex shader** — provably identical GL vs vk
  (`Renderer=0` both; same matrices/shader).
- Additive-terrain-over-sky, missing/dead textures (0 BADTEX), vertex argb,
  blend-mode translation, camera/haze params (all from the earlier round).

## The open question

A cement tile's **bounding box** covers the apron pixel (RIALOG:
`a_s_gatecontrol.tga` z≈0.72, am=3), yet the pixel's depth is 0.99999 — so the
tile's **actual triangles leave a gap** at that pixel. The CPU submits identical
cement triangles to both backends with no degeneracy, yet **vk leaves gaps and GL
does not.** That points at a **GPU-side coverage/rasterization difference** in the
vk cement path. Unresolved candidates:

- Native-resolution cracking: does GL render at 6016×3384 too, or lower? If GL is
  lower-res, cracks/T-junctions between cement quads could be a native-res-only
  artifact on vk. (Holes look too big for this, but unverified.)
- A subtle per-vertex data difference reaching the GPU only on the vk ring path
  (attribute layout / half-pixel UV / provoking vertex), despite identical CPU
  values.
- The leftover `am=3` (AlphaInvAlpha) on opaque pavement is anomalous but does
  NOT by itself explain unwritten depth.

## Recommended next steps

1. **Per-triangle trace** (existing tooling): `MC2_VK_TRACEPX="x,y"` lists every
   pass whose *triangles* actually cover a pixel (vs RIALOG's loose bbox). Run at
   an apron pixel AND a black-band pixel to confirm exactly what does/doesn't
   cover them.
2. **GL vs vk resolution check** — cheap, decides the cracking hypothesis.
3. If same triangles + same res: diff the actual submitted cement vertex stream
   GL vs vk (instrument `gos_RenderIndexedArray` / `drawIndexedTris`).

## Tooling & repro (all committed on this branch)

- **Metal frame capture:** `METAL_CAPTURE_ENABLED=1 MC2_VK_CAPTURE_AT_SECS=<n>
  MC2_VK_CAPTURE_FILE=<path.gputrace> ./mc2-vk …` (needs full Xcode.app to read).
- **Per-pixel triangle trace:** `MC2_VK_DEBUG=1 MC2_VK_TRACEPX="x,y"`.
- **Indexed-draw bbox probe:** `MC2_VK_DEBUG=1 MC2_VK_RIALOG="x,y"` (added
  2026-07-24, `rendervk/gameos_graphics.cpp:2035`) — logs any `gos_RenderIndexedArray`
  whose vertex bbox covers (x,y) or is degenerate.
- **Fog bake dump:** `MC2_FOG_DEBUG=1`.
- **Repro:** save `~/.mechcommander2/savegame/testgame.ims` is at building 13
  (Mission 1). Warp: launch `./mc2-vk -mission mc2_02`, wait ~20s, make frontmost,
  `tools/devinput/sendkeys load` ×2 (8s apart), settle ~20s. Scripts in the
  session scratchpad (`capture_run.sh`, `vkdebug_run.sh`, `rialog_run.sh`).
- Evidence PNGs / .gputrace live under `docs/bugs/…` (gitignored; only `.md`
  writeups here are tracked).
