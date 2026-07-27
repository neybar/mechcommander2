# Vulkan cement/pavement "holes" — investigation findings (OPEN)

**Status:** open. Root-caused to **MoltenVK/Metal rasterization** (geometry proven
identical GL vs vk). No fix yet. Branch `fix/vk-black-quad-diag`.
**Last worked:** 2026-07-27.

## NEXT SESSION — Metal frame-debugger plan (start here)

Everything answerable from the CPU/API side is done: five hypotheses refuted, and
the GL-vs-vk vertex diff proved the submitted geometry + projection + shader are
identical, so the holes are **purely MoltenVK/Metal rasterization** of that
geometry. The only tool left that can see *why* Metal drops the coverage is the
**Xcode Metal frame debugger** (jalance is wiring an Xcode MCP; a `.gputrace` is
Apple-proprietary and only introspectable inside Xcode, so expect to drive the
Xcode UI, not parse the trace).

Leading hypothesis to confirm/refute: **guard-band / large-coordinate
rasterization.** The D3D `XYZRHW` terrain submits screen-space triangles reaching
x ∈ −2781..+5203 (2048-wide space) → clip coords far outside ±w
(`gl_Position.x≈8160` vs `w≈2000`). Rasterizers use fixed-point edge math with a
finite guard band (~±32768 px, 8-bit subpixel); beyond it → overflow / wrong
coverage (fgiesen pt.5). Both backends submit these; desktop GL rasterizes them,
Metal is suspected to drop them.

Steps:
1. Capture a `.gputrace` at a hole — use `tools/vkprobe/run.sh --save 1
   --capture gputrace --at <n>` (writes to `docs/bugs/<date>-vkprobe/`; a fresh
   one may already be waiting there from the handoff).
2. In Xcode, **Debug Pixel** at a known hole. Building-13 apron hole ≈ gos(684,75)
   = render(2009,234) on the 6016×3384 drawable; the big black band is center-right.
   Confirm: does *any* terrain fragment run there, or is depth cleared/far?
3. **Geometry inspector** on the terrain draw(s) covering that pixel: are the
   covering triangles present in Metal's post-clip geometry? Are the huge
   off-screen triangles clipped correctly, or do they vanish / mis-cover? (Watch
   the known geometry-inspector NDC/frustum display quirk — Apple forum 773786 —
   verify against actual coverage, not just the 3D view.)
4. If Metal is dropping the huge triangles → fix is CPU-side: clip/limit terrain
   triangles to a sane screen bound before submit (shared `quad.cpp`/`txmmgr.cpp`),
   OR file an upstream MoltenVK issue with a minimal repro.
5. If the covering triangle rasterizes but is depth-killed → re-open the depth
   angle (but MC2_VK_DEPTHCLAMP was already refuted).

Repro is fully headless now: `tools/vkprobe/run.sh --save 1 ...` reaches
building 13 with no input (see the tooling section at the bottom). All gated
diagnostic env vars: `MC2_VK_TRACEPX`, `MC2_VK_RIALOG`, `MC2_VERTDUMP`,
`MC2_CEMENT_SOLID`, `MC2_VK_POSVIEWPORT`, `MC2_VK_DEPTHCLAMP`, `MC2_FOG_DEBUG`.

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
- **Backface culling / pipeline-cache cull leak** (checked 2026-07-25). Cement
  draws under **`gos_Cull_None`** — `txmmgr.cpp:1045` sets Culling→None right
  before all the terrain/ISCRATERS blocks (the only prior CW set is `:948`, for
  the 3D hardware-shape pass, which finishes at `:1042`). vk bakes cull into the
  pipeline and the **pipeline-cache key includes cull mode** (`stateBits`,
  `gameos_graphics.cpp:463-466`, bit 15) — so cement gets its own `CULL_NONE`
  pipeline; no stale CW pipeline is reused. With cull disabled, nothing is
  dropped by winding, so culling cannot produce the holes.
  - Side note (latent, NOT this bug): vk's **negative-height viewport**
    (`vp.height = -height`, `gameos_graphics.cpp:1125`) inverts effective winding
    vs GL for the *screen-space identity-Z path*, so the `frontFace=CCW` comment
    at `:564` is misleading. It bites nothing today: that path always runs
    `Cull_None`, and the 3D models (the real `Cull_CW` consumers) use a separate
    perspective-MVP path (`ShapeRenderer`, `txmmgr.cpp:1032`) whose winding is
    correct. Leave it; revisit only if a future culled screen-space draw appears.
- **Vertex/index stream identity** (checked 2026-07-25). `indexArray` is identity
  `[0,1,2,…]` (`txmmgr.cpp:168`); vk's CPU index-expansion (`gameos_graphics.cpp:2079`)
  therefore reproduces the exact GL triangle list. `MAX_SENDDOWN = 10002`
  (`txmmgr.cpp:67`) is divisible by 3, so batch splits land on triangle
  boundaries on both backends. Triangles are built and depth-culled per-triangle
  in shared code (`quad.cpp:1476-1501`, `z<1.0` cull) → **identical triangle set
  submitted to both backends**, bit for bit.

## Experiment: `DRAWSOLID` base under cement — REFUTED (2026-07-25)

Hypothesis was: cement holes because pure cement is the *only* terrain drawn as a
single `ISCRATERS` layer with no `MC2_DRAWSOLID` base to fill depth. Added a
gated toggle `MC2_CEMENT_SOLID` (`quad.cpp:1506`) that routes pure cement through
the `DRAWSOLID` base path like every other terrain tile.

**Result: not fixed — dramatically worse.** With the flag on, the entire
cement/pavement area shatters into white (sky) + black (haze) triangular shards;
natural terrain (grass, road) stays perfect. Captured headless via
`tools/vkprobe`, before/after at building 13.

What this proves:

- The missing-base-layer theory is **dead**. The base layer wasn't hiding
  anything for regular terrain — regular terrain simply lacks this geometry
  problem. Cement has it in **both** passes.
- The baseline only showed a couple of holes because cement's leftover **`am=3`
  (AlphaInvAlpha) blend was masking widespread partial-coverage gaps** (blending
  cement over the background). Opaque `DRAWSOLID` removes that masking → the gaps
  are revealed to be **pervasive across cement geometry**, systematic, not a few
  stray tiles.
- Same pass / states / `Cull_None` / CPU-identical geometry as the grass that
  renders fine → the differentiator is **the cement tiles' own vertex data
  tripping MoltenVK rasterization**. The `MC2_CEMENT_SOLID` mode is now the
  *preferred* repro (many holes, not two) for hunting which vertex property does it.

## PROVEN (2026-07-25): geometry covers the hole, MoltenVK emits no fragment

Ran `MC2_VK_TRACEPX` + `MC2_VK_RIALOG` at the findings' *measured* apron pixel
(gos **684,75** = render 2009,234 ÷ the 2048×1080→6016×3384 scale), headless via
`tools/vkprobe`:

- **TRACEPX**: `cement_1.tga` **covers** the pixel — normal grey argb `ff929292`,
  z=0.803, am=3, zw=1, zc=1, atest=0. Many other terrain tris (mc2_02.detail,
  quonset, oak&maple, vehiclehang) also cover it at z≈0.73–0.81.
- **RIALOG**: that cement batch is `nv=66` (22 tris), **NOT degenerate**,
  z=[0.58,0.80], rhw∈[0.00066,0.00138] (all positive/sane).
- Metal capture (prior) at the same pixel: fog color, depth **0.99999** (cleared),
  **no fragment**.

TRACEPX is a CPU point-in-triangle on the *submitted* verts, so this is
airtight: **well-formed triangles are submitted and cover the pixel in screen
space, yet MoltenVK rasterizes zero fragments there.** Not missing geometry, not
degenerate verts, not depth-occlusion (nothing drew at all), not the base layer
(refuted above). The failure is **downstream of vertex submission, in MoltenVK's
rasterization** of these triangles.

Also disambiguated a *second, distinct* white artifact nearby (not the holes):
some terrain tiles carry vertex argb `0x..ffffff` and render **washed white on
vk / grey on GL** (the EXTREME-ARGB case). The top-left white square by
building 13 is this, at *near* depth (z≈0.735), not a hole — don't conflate it
with the true far-depth apron holes.

### Negative-height viewport — REFUTED (2026-07-25)
Suspected MoltenVK's negative-viewport emulation dropped coverage. Added gated
`MC2_VK_POSVIEWPORT` (`gameos_graphics.cpp`): Y-flip moved into the projection
matrix + a positive-height viewport. Result: scene renders correctly (models
upright, as the math predicts) and **the holes are unchanged** — same white
square, same black band. The negative viewport is not the cause.

### Ring-buffer aliasing — REFUTED by code (2026-07-25)
Suspected the single per-frame vertex ring wrapped mid-frame and a later
`memcpy` clobbered an earlier draw's verts before GPU submit. But `ringAlloc`
(`gameos_graphics.cpp:651`) does **not** wrap: on overflow it returns NULL, drops
the draw, and prints "draws dropped this frame" (never observed); `ring_off`
only advances within a frame; the ring is 16 MB (a terrain frame is far under
that). No intra-frame aliasing possible.

### Caveat on the "no fragment" contradiction
The TRACEPX-covers-but-capture-shows-no-fragment result pairs a *this-session*
TRACEPX at gos(684,75) with a *prior-session* Metal capture at the "same" pixel.
Pixel targeting has been unreliable (two earlier picks landed on crates / a
washed-white tile, not holes). **To solidify:** detect a hole pixel from the
current screenshot (largest pure-black/-white region centroid), then TRACEPX
that exact pixel in the same run. What's solid regardless: at every sampled
hole-region pixel, cement geometry is present, covering, and non-degenerate.

### External research: the D3D→GL→VK clipping lineage (2026-07-27)
`gos_VERTEX` (x,y,z,**rhw**) is D3D's `D3DFVF_XYZRHW` **pre-transformed** vertex
format. Documented behavior of XYZRHW: Direct3D does **not** frustum-clip these
verts — **only guard-band clips** them (GameDev.net; MS docs). Two Vulkan porting
traps follow, both from this lineage:

1. **Depth clip vs clamp.** Vulkan defaults to `depthClampEnable=FALSE` → depth
   *clip* ON; D3D content assumes depth *clamp* always on. `VK_EXT_depth_clip_enable`
   exists explicitly "for translating DX content which assumes depth clamping is
   always enabled" (Vulkan docs). → **TESTED (MC2_VK_DEPTHCLAMP, enabling the
   depthClamp device feature + depthClampEnable=VK_TRUE = clamp on / clip off):
   holes UNCHANGED. REFUTED.** Consistent with `quad.cpp:1496` already CPU-culling
   terrain depth to [0,1) — so no primitive has out-of-range NDC z to clip anyway.
2. **XY guard-band / frustum clipping (UNTESTED).** The engine relies on D3D not
   frustum-clipping XYZRHW verts. Terrain emits **huge** triangles far off-screen
   (RIALOG base/detail bbox −2781..+5203 in a 2048-wide space → clip coords well
   outside ±w). Vulkan *always* frustum-clips XY (guard band is a HW optimization,
   not spec-controllable); GL renders them fine, MoltenVK/Metal may not. Caveat:
   the cement batch actually covering the apron hole had a *modest* bbox
   (586,−166)-(2236,1214), not huge — so if huge-triangle clipping is the cause,
   the mechanism must be indirect. DXVK (D3D→Vk) hit related MoltenVK issues.

Research sources logged in ENGINEERING_LOG (2026-07-27 entry).

### GL-vs-vk submitted-vertex diff — DONE: geometry is identical (2026-07-27)
Added `MC2_VERTDUMP` (shared code, `txmmgr.cpp` `renderLists()`): one-frame dump of
every terrain vertex node's exact `gos_VERTEX` data, run on both the GL and vk
builds at the same headless warp (`tools/vkprobe --bin …/mc2` and `…/mc2-vk`),
then diffed.

Result (128 nodes each, 108 with geometry, 12,387 verts in equal-count nodes):
- **96.0% of verts match to <0.1 px**; 96.2% to <1 px.
- **The huge off-screen triangles (screen x −2781..+5203) are present, identical,
  in BOTH backends** — MoltenVK is not being handed different/degenerate geometry.
- The 3.2% >10px diffs are **localized/clustered** (e.g. all of node 80 shifted
  ~24px in one region near the deployed mechs) — dynamic elements captured at
  slightly different animation frames across the two separate launches, not a
  systematic transform difference. One node (i=115) differed by 9 verts (a single
  frustum-edge tile), same texture.

**Conclusion:** GL and MoltenVK receive the *same* clip-space geometry (this diff)
through the *same* projection matrix + vertex shader (verified earlier). The
cement holes are therefore **100% a MoltenVK/Metal rasterization behavior** on
identical input — not geometry, not transforms, not state. Note the huge
guard-band-reliant XYZRHW triangles reach Metal on both backends; GL rasterizes
them cleanly, MoltenVK does not → consistent with the XYZRHW/guard-band research
thread, though the specific failing cement triangle at the apron was modest-sized,
so the exact Metal-level mechanism still needs the Metal debugger to see directly.

### Remaining candidate directions
1. Same-run TRACEPX at a screenshot-verified hole pixel (tighten the above).
2. **GL-vs-vk submitted-vertex diff** (findings' original step 3): dump the terrain
   vertex stream on both backends at this warp and diff — definitively settles
   whether the geometry reaching the GPU is truly identical. GL binary can now
   run the same headless warp (`--bin …/mc2`) once the load-hook build is deployed.
3. Single missing-triangle inspection in the Metal debugger (needs Xcode).

## The old open question (superseded by PROVEN above)

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
