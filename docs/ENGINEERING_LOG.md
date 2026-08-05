# Engineering Log

One entry per significant bug hunt or porting battle: **symptom → cause → fix**.
Newest entries at the top. Practice borrowed from the
[Generals Mac port](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad).

---

## 2026-07-31 — Task 4 finding 3 SOLVED: `-DBGR` was inverting every team colour; the flip was a symptom. Not a vk descriptor collision

**Refutes the 2026-07-20 entry "Downed mech flips team-color skin (blue ↔ red)
between frames on vk", and the cross-reference to it inside the 2026-07-20 glass
entry ("the mech flip is texture-based (binding/descriptor class)"). Both were
wrong. This is not a Vulkan bug, not a descriptor-cache bug, and not a texture
*binding* bug at all.**

**Symptom.** A mech's team-colour skin flips blue↔red (with the chest highlight
flipping cyan↔yellow), moments apart, same unit, no respawn — driven by camera
position. jalance re-confirmed it 2026-07-31 and added two observations that
broke the case open: he had **only ever noticed it on downed enemy mechs**, and
it looked **camera-position dependent**. (Stated as observations, not
boundaries — which was the right framing, because "downed" and "enemy" turned
out to be incidental.)

**The refutation that mattered, and it was free.** jalance ran the same scene on
the **GL** build and the flip happened there too. A descriptor-cache-key
collision is a Vulkan-only mechanism, so one comparison falsified the standing
hypothesis outright. The code involved is in `mclib`, with no renderer code
anywhere in the path.

**Mechanism of the flip — a get/set asymmetry under `#if defined(BGR)`.**

- `Mech3DAppearance::getPaintScheme` **always** applies `bgrTorgb` (an R↔B byte
  swap).
- `Mech3DAppearance::setPaintScheme(DWORD,DWORD,DWORD)` **always** applies it
  when storing `psRed/psGreen/psBlue`. So get/set round-trip cleanly.
- **But** `resetPaintScheme`'s early-return branch — taken when the texture
  instance already exists, under the comment `//Still need to store
  psRed/psGreen/psBlue!!!!` — stored `psRed = red` **raw**, skipping the
  conversion.
- Every LOD transition runs `getPaintScheme(r,g,b); resetPaintScheme(r,g,b);`
  (`Mech3DAppearance::render`, the `selectLOD != currentLOD` and `currentLOD &&
  baseLOD` blocks). So each camera-driven LOD crossing swapped R and B, which
  changed the computed `paintInstance` key, which selected **a genuinely
  different texture instance**. Self-sustaining: it flips back on the next
  crossing, forever.

That is why it looked like wrong-texture-content: the engine really did load and
bind a differently-coloured skin. Nothing was mis-bound; the *request* was wrong.

**Root cause — but the asymmetry is not the real defect. `-DBGR` is.**

> **This section supersedes an earlier draft of this same entry**, which
> concluded that `-DBGR` merely *activated* a dormant Microsoft bug and that
> equalising the two storage paths was the fix. That was wrong, and it would
> have shipped a build where **every** mech and vehicle rendered with R and B
> inverted — stably, instead of intermittently. Caught in review before merge.

The engine's colour DWORDs are **`0xAARRGGBB`**, and `bgrTorgb` converts *out*
of the format the engine already uses. Evidence, all from data rather than
elimination:

- `mclib/tgl.cpp:1811` `0xffff0000 //Hot Red`, `:1815` `0xff0000ff //Hot Blue`.
- Locked texture memory is `0xAARRGGBB` on **both** backends — GL's swizzle loop
  in `Lock()` (`rendergl/gameos_graphics.cpp`) and vk's `swizzleRB`
  (`rendervk/gameos_graphics.cpp`) perform the same transform. There is no
  GL-vs-vk divergence here for `-DBGR` to be compensating for.
- `setPaintScheme(void)` reads `ps*` as `0x00RRGGBB` and writes `0xff<<24 |
  r<<16 | g<<8 | b`. So `ps*` must be RGB-ordered, which the *incoming* mission
  and prefs colours already are.

**The history is a double-correction.** `d0ce5f4` (2016-11-19, *"rgba -> bgra ->
rgba conversion on lock/unlock, because CPU code expects BGRA"*) already made
lock/unlock hand CPU code the right byte order. Ten months later `383e3c2`
(2017-09-29, *"fixed paint scheme BGR define was missing"*, a one-line CMake
change with no rationale) added `-DBGR` on top of it. Both are upstream
(alariq); the second undoes the first for paint schemes only.

**So `-DBGR` had two effects**, and only the second was ever noticed: it inverted
R↔B on every mech and vehicle team colour, *and* it broke the get/set round-trip
so the inversion oscillated on LOD changes. The oscillation is what got reported;
the inversion had been there since 2017, hiding in plain sight as "that's just
what the colours look like."

**Fix: remove `-DBGR`** (`mclib/CMakeLists.txt`, replaced with a comment saying
why it must not come back). With `BGR` undefined, `getPaintScheme` and the store
are both the identity, so the early-return's raw store becomes genuinely
equivalent — the oscillation disappears **by construction**, not by patching, and
colours come out as authored.

**Also kept: a `storePaintScheme()` helper** on both `Mech3DAppearance` and
`GVAppearance`, with `setPaintScheme(DWORD,DWORD,DWORD)` and all three
early-return sites (1 in `mech3d.cpp`, 2 in `gvactor.cpp`) routed through it.
This is **defence in depth, not the fix** — it makes the two storage paths
incapable of diverging if anyone ever defines `BGR` again.

**Why the incidental observations fit.** Downed mechs are stationary corpses you
pan around, and the destruction path forces `currentLOD = 0`, guaranteeing a
transition on the next frame the camera isn't close — so they're where you'd
*notice* it. But "downed" and "enemy" were never preconditions, and jalance was
careful to file them as observations rather than boundaries. That framing
mattered: **the player's own lance was flipping the whole time.** The very log
lines used as evidence — `in(r=0007083e g=0007083e b=00fae525)` — are
`resetPaintScheme(highlightColor, highlightColor, baseColor)` from
`mission.cpp:1093`, i.e. the *player* branch, carrying jalance's own configured
colours. A theory built on "enemies only" would have been chasing a boundary
that didn't exist.

**Provenance — port regression, not an original-engine wart.** Both halves are
upstream's: the `-DBGR` define (`383e3c2`) and the `d0ce5f4` lock/unlock swizzle
it double-corrects. The asymmetric early-return is Microsoft's, but it was
**inert** in their build — MS never defined `BGR` (confirmed: `mclib/MCLib.vcproj`
preprocessor definitions contain no `BGR`), so both storage paths were the
identity and `psRed = red` was exactly correct. Per jalance (2026-07-31), the
preserve-warts-and-all rule applies only to defects that were **also in the
DirectX original**; anything upstream introduced in the GL conversion is fair
game, so this was fixed outright rather than gated behind an opt-in.

**Vehicles had it too.** `GVAppearance` carries the identical early-return
asymmetry at two sites, and the inversion applied to vehicles equally. Enemy
colours are per-unit mission data (`mission.cpp:1095`), not a global palette;
only the player's side uses `prefs`.

**Verification, in two parts.**

*1 — the flip is gone.* Temporary `MC2_PAINT_DEBUG` instrumentation logged every
`resetPaintScheme` (incoming colour, stored `ps*`, `paintInstance`,
`currentLOD`), before and after, on the same archived save. Counting only
**re-paints of already-painted mechs** — excluding first-paint events where `ps`
is still the `0xffffffff` constructor sentinel; raw `grep -c "PAINT reset"` gives
45 and 70 and is *not* the number below:

| | before | after |
|---|---|---|
| re-paints of already-painted mechs | 11 | **36** |
| instances that changed texture instance | **1** | **0** |

The path was exercised **3x more** and never once swapped, so this is not a false
pass from the code path going quiet. Before, the oscillator (`this=0x786e14000`)
alternated `inst=2b8004d` ↔ `6a80149` on each `lod=` change.

*2 — the colours are right, which part 1 could not show.* This is the check that
caught the bad first fix. Under the storePaintScheme-only version, a fresh
mission rendered jalance's lance **teal**; with `-DBGR` removed it renders
**yellow with dark navy trim**, matching his `options.cfg` exactly:

| setting | value | RGB |
|---|---|---|
| `BaseColor` | `0x00fae525` | (250, 229, 37) yellow |
| `Highlightcolor` | `0x0007083e` | (7, 8, 62) dark navy |

And `bgrTorgb(0x00fae525)` = `0x0025e5fa` = (37, 229, 250) — exactly the teal
seen before. Screenshot-verified on a **fresh mission**, not a save.

**The archived repro save cannot answer part 2**, which is why part 2 needed a
fresh mission: `ps*` is serialized (`mover.cpp:7375` saves `getPaintScheme`
output, `:7541` restores it), so a save written pre-fix has each mover's colour
baked in at whatever parity it happened to be at. In the post-fix log, two movers
of the same team sit permanently at `inst=2b8004d` and `6a80149` — one the exact
swap of the other. See `docs/bugs/2026-07-31-paintscheme-baked-into-saves.md`.

**Two false conclusions were drawn from that polluted save and are worth
recording**, because both looked like corroboration at the time: that enemy cargo
trucks "landing on their authored red" confirmed the fix (it was baked save data,
not authored data), and that the player's lance had never been affected (it had —
see above). Evidence from a save written *by the buggy build* is evidence about
the bug, not about the fix.

**Lesson — this is the "elimination is not a root cause" rule, skipped.** Finding
3 was triaged to OPUS *because* it pattern-matched the 2026-07-17 descriptor-cache
entry's symptom description ("stable-looking but wrong texture content ...
flipping with camera/render state"). That match was never gated on a GL-vs-vk
comparison, and the comparison takes one playthrough and costs nothing. A
familiar-looking symptom is a reason to run the cheap discriminating test first,
not a reason to skip it. The 2026-07-17 entry is accurate about what it fixed —
the error was in over-applying it.

**Second lesson — "the symptom stopped" is not "the bug is fixed."** The first
fix made the flip go away and had a clean instrumented before/after to prove it,
and it was still wrong: it stabilised the *inverted* colour, and would have
shipped every mech and vehicle permanently mis-coloured. What caught it was
asking why the offending code existed at all, rather than only whether the
symptom was gone. The discriminating test — "what colour *should* this be, and
does it match?" — costs one fresh mission and was never run until review forced
it. When a fix equalises two paths, check which of them was right.

---

## 2026-07-30 — Task 18 shadows: the double-yaw hypothesis is REFUTED; it's original-engine shadow quality

**Supersedes the root-cause hypothesis in the 2026-07-20 entry "Mech shadows
swing wildly with small heading changes" — that entry's mechanism is wrong.
Do not implement the fix it proposes.**

**What prompted this.** jalance play-compared GL vs vk for task 18 and called it:
shadows look equally bad on both, no DX build available to compare against, and
the symptom reads as generally poor shadowing rather than a specific defect.
Timeboxed code read to check whether anything obvious was behind it.

**The refutation.** The 2026-07-20 entry claimed `RotateLight(s_lightDir,
rotation)` (`tgl.cpp`, in `TG_Shape::MultiTransformShadows`) double-applies the
mech's yaw, because the vertices are already in world space. That premise is
false: **`s_lightDir` is in shape space, not world space.**

- `TG_MultiShape::TransformMultiShape` (`msl.cpp`, the `TG_LIGHT_INFINITE` case)
  builds `s_lightToShape = lightToWorld × worldToShape`, then takes
  `GetLocalForwardInWorld` into `s_lightDir`; `s_rootLightDir` copies it for the
  root node (`parentNode == NULL`). Composing with `worldToShape` is what puts
  the direction in shape space.
- `worldToShape` is `Invert(shapeToWorld)`, so its yaw component is R(−yaw).
- `RotateLight` (`mathfunc.cpp`) applies `x' = x·cos + z·sin; z' = z·cos − x·sin`,
  i.e. R(+yaw) about Y.
- `Matrix4D::BuildRotation(const EulerAngles&)` (`stuff/matrix.cpp`) with
  pitch = roll = 0 produces that same rotation under Stuff's row-vector
  convention — the handedness and sign match, they are not merely similar.

So the `RotateLight` call is the **correct inverse** of the yaw baked into
`worldToShape`: it returns the shape-space light to world space so it matches the
world-space vertices from `s2w`. Removing it — the 2026-07-20 suggestion — would
*introduce* a yaw-coupled shadow bug, not fix one. The commented-out
`RotateLight(..., -angles.yaw)` lines in `TransformMultiShape` are consistent
with this: re-enabling them would over-rotate, which is presumably why they are
commented out.

**What is actually weak in this shadow path** (observed, not chased):

1. **Only yaw is ever undone; pitch and roll are not.** `worldToShape` carries
   all three, but `RotateLight` rotates x/z and leaves `y` untouched. The
   projection divides by that `y` (`zFactor = up.y / s_lightDir.y`), so a
   shape-space-contaminated `lightDir.y` scales shadow *length* — and with a
   low sun, `zFactor` is hypersensitive to it. This fits "swings and rescales"
   far better than a yaw error would; a yaw error rotates a shadow, it doesn't
   rescale it.
2. **`shadowOrigin` in `TransformMultiShape` is dead code.** It is built
   pitch-only with yaw explicitly zeroed
   (`BuildRotation(EulerAngles(-angles.pitch,0,0))`), given a translation, and
   then never read. Together with the commented-out yaw lines it looks like an
   abandoned Microsoft attempt at exactly the pitch handling missing in (1).
3. **The projection assumes flat ground at the unit's own elevation**
   (`up.y -= pos->y`, with the in-code comment "this assumes terrain is FLAT and
   at zero elevation"). On any slope the shadow is wrong by construction.

**Verdict: original-engine shadow quality, not a port regression, not fixed.**
Reproduces identically on GL and vk (jalance, play comparison) and the code
predates the alariq port (`git blame`, MS shared-source commit `63e58e9`). Per
the standing rule on original-engine warts, a behavioural change here would be a
mod or an opt-in option and is jalance's call. A real fix is a shadow-projection
rewrite (terrain-aware ground plane + full light-space transform), not a
one-liner — it is not parity work and should not be smuggled into the port.

---

## 2026-07-30 — vk swapchain semaphore reuse + missing draw-engine teardown (task 19): validation now clean

**Symptom.** Running any mission under `VK_LAYER_KHRONOS_validation` reported two
errors, every run: `VUID-vkQueueSubmit-pSignalSemaphores-00067` (a binary
semaphore signalled while it may still be in use) and
`VUID-vkDestroyDevice-device-05137` (~200 live child objects at device
teardown). No visible misbehaviour, but both are undefined behaviour.

**Cause 1 — one render-done semaphore shared by every frame.**
`RenderContext` held a single `sem_render_done_`. `swap_window` signals it from
`vkQueueSubmit`, `vkQueuePresentKHR` waits on it, and then the code waits on
`frame_fence_`. That fence proves the *submit* completed — it says nothing about
whether the *presentation engine* has consumed the semaphore. So the next
frame's submit could signal a binary semaphore a pending present was still
waiting on. The single-frame-in-flight design made this look safe and it is not:
the fence and the present are separate synchronisation domains.

**Fix 1.** One render-completion semaphore per swapchain image, indexed by the
acquired image index (`sem_render_done_[cur_image_]`) — Khronos' recommended
option (a), <https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html>.
Sufficient rather than arbitrary: an image cannot be re-acquired until its
present retires, so its semaphore is always free by the time we signal it again.
Chose this over `VK_KHR_swapchain_maintenance1` (option b) to avoid depending on
extension support under MoltenVK.

Lifetime detail worth remembering: these semaphores belong to the **swapchain**,
not the frame loop — destroying one while a present still waits on it is the
same bug in reverse. They are created in `create_swapchain` immediately after
the old swapchain is destroyed, and torn down *after* `vkDestroySwapchainKHR`
in `destroy_render_context`. Deliberately **not** part of
`destroy_swapchain_views`, which runs before the old swapchain is gone.
`sem_image_available_` stays single: it is per frame-in-flight, there is exactly
one frame in flight, and the fence does prove its wait was consumed.

**Correction, same day (review catch):** the first version of this entry, and
the code comments with it, said destroying the old swapchain "retires any
present still holding the previous semaphores." **That is not a Vulkan
guarantee and should not have been written as one.** Core Vulkan gives no way
to know when a presentation engine has finished waiting on a semaphore —
Vulkan-Docs #2007 says so explicitly, and neither `vkDeviceWaitIdle` nor
`vkQueueWaitIdle` covers a pending present; only
`VK_KHR/EXT_swapchain_maintenance1`'s present fence specifies it. What the code
does is the best approximation available without that extension (the recreate
path device-waits first, and MoltenVK drains presents on the same queue), which
holds on macOS and is a **portability risk for M3's Linux/Windows builds**.
Logged as `docs/bugs/2026-07-30-present-semaphore-destroy-unspecified.md`.
Note the zero-VUID result does **not** vindicate the original wording: the
validation layer only checks present-semaphore destruction when
`swapchain_maintenance1` is enabled.

**Cause 2 — the vk draw engine had no teardown at all.** `engineInit` creates
pipelines, shader modules, samplers, descriptor set/pipeline layouts, a
descriptor pool, the 16MB host ring and the dummy image/UBO; textures allocate
image+view+memory per slot. Nothing ever destroyed any of it. Inventory at exit
(`VK_LAYER_DUPLICATE_MESSAGE_LIMIT=1000`): 42 `VkDeviceMemory`, 41
`VkDescriptorSet`, 40 `VkImage`/`VkImageView`, 14 `VkPipeline`, 12
`VkShaderModule`, 4 `VkSampler`, 2 `VkBuffer`, plus the layouts and pool.

**Fix 2.** New `engineDestroy()` (anonymous namespace, `rendervk/
gameos_graphics.cpp`) mirroring `engineInit` in reverse, exported as
`graphics::vk_destroy_draw_engine()` and called from `destroy_render_context`
after `vkDeviceWaitIdle` and before everything else. It also flushes the
deferred image/buffer lists, which otherwise never get their `vk_begin_frame`
flush on the last frame. Destroying the descriptor pool frees its 41 sets, so
they need no separate pass.

The first version had an `if(!g_eng.initialized) return` shortcut, which the
review caught as reintroducing the very VUID it closes: `engineInit` sets
`init_failed` and bails *after* creating some of the 12 shader modules (a stale
or missing `.spv` in the deploy dir is enough), and the shortcut skipped the
shader-destroy loop. Removed — every step is handle-guarded or iterates a
container that is empty when init never ran, so one uniform path is correct
whether init succeeded, failed halfway, or never started. General lesson:
an early-out keyed on a "fully initialised" flag is the wrong shape for a
teardown, because partial initialisation is exactly when teardown matters.

Not covered, deliberately: gosBuffers still alive at shutdown aren't reachable
from `engineDestroy`, so the zero-leak result depends on the game destroying
its own buffers (txmmgr's light/scene UBOs, tgl's vertex/index buffers) before
`gos_DestroyRenderer` runs last in `TerminateGameEngine`. True today, enforced
by nothing; noted in a comment at the call site.

**Note for whoever adds a namespace-scoped helper to `gameos_graphics.cpp`:**
almost the whole file sits inside an **anonymous namespace** (opens at ~219,
closes at ~1264). Writing `namespace graphics { … }` inside it silently creates
`(anonymous)::graphics`, which shadows the real one and makes every existing
`graphics::vk_frame()` call in the file fail to resolve. Define the helper as a
plain function inside the anonymous namespace and add a thin
`namespace graphics { … }` forwarder *after* the anonymous namespace closes.

**Verified.** Baseline binary vs fixed, same harness
(`tools/vkprobe/run.sh --save 1`), both under `VK_LAYER_KHRONOS_validation`:
baseline reports both VUIDs, fixed reports **zero validation errors or
warnings**. Five consecutive clean-quit cycles all exited 0 with zero VUIDs —
worth checking explicitly since teardown is where task 3's intermittent
`SoundEngine::destroy()` SIGSEGV lives and this change adds work to that path;
it did not reproduce in those five runs (it is intermittent, so this is not a
claim that task 3 is fixed). Screenshots before and after the teardown change
are identical, and the GL build still compiles.

---

## 2026-07-29 — Bookkeeping: the task-4 "black quad" *was* the cement/pavement holes; checkerboard wreck dismissed

Two task-4 findings closed on review, no code change.

**The 2026-07-20 "recurring black quad" and the cement/pavement holes solved
2026-07-27 are the same bug.** The hunt renamed itself mid-flight (the fix
branch was `fix/vk-black-quad-diag`) and the finding was never marked closed in
`CREDIT_PLAN.md`, so a later session reading the plan would have re-opened a
solved bug. Same location (pavement/roofs near building 13, Mission 1), same
profile: sharp-edged, world-anchored, vk-only, unaffected by every graphics
toggle. The zero-alpha-texture cause explains both puzzles that broke the
earlier theories: the draw **rasterized perfectly and painted nothing**, so
whatever lay behind showed through — black where nothing was behind it,
fog-white where the fog backdrop was, and the retracted SRM turret mechanism
visible "through" it because nothing was ever painted over the pit. Both
intermediate root causes recorded on 2026-07-20 (shadow blend-state, then
terrain-lighting-goes-black) are superseded by the 2026-07-27 entry below.

**The destroyed LRM truck wreck's black/white checkerboard is not a bug** —
jalance's call on review: it's chunky low-resolution wreck art, not wrong or
missing texture content. The 2026-07-20 entry inferred "wrong texture" from
screenshots alone and never checked it against GL. Worth noting as a pattern:
the checkerboard read as a placeholder texture *because* we were mid-hunt on a
real texture-binding bug at the time.

Net: task 4 has exactly **one** open finding left — the mech team-colour
blue→red flip (finding 3).

> **Update 2026-07-31:** finding 3 is now closed too, so **all four Mission 1
> findings are resolved** and task 4 has zero open findings from Mission 1
> (later missions still unswept). It was not the texture-binding bug this entry
> assumed was live at the time — see the 2026-07-31 entry at the top.

---

## 2026-07-27 — vk cement/pavement holes SOLVED: zero-alpha textures, not the rasterizer

**Symptom.** Vulkan build only: sharp fog-white square holes and a black band in
pavement/city-block ground near building 13 (Mission 1), sweeping as the camera
pans. GL clean at the same spot. Open for ~a week across several sessions.

**Cause.** Some retail TGAs are logically opaque but carry an **all-zero alpha
channel**. `fillPixels` in `rendervk/gameos_graphics.cpp` forced alpha to 255 for
`FORMAT_RGB8` sources but did a straight `memcpy` for `FORMAT_RGBA8`, so that
zero alpha reached the GPU intact. The terrain shader computes
`c = Color.bgra; c *= tex_color;` → `c.a = 1.0 × 0 = 0`, and the cement pads are
drawn with `AlphaInvAlpha` (and sometimes alpha test), so `src·0 + dst·(1−0)`
leaves the destination untouched — or the fragment is `discard`ed outright.
**The draw rasterized perfectly and painted nothing**, and the backdrop showed
through as "holes".

**Fix.** Port the GL path's `convertIfNecessary`/`makeKindaSolid`/
`doesLookLikeAlpha` trio (`rendergl/gameos_graphics.cpp:841-880`) to the vk
backend as `looksLikeAlpha`/`makeKindaSolid`/`normalizeAlpha`, called at the end
of `fillPixels`. Force-opaque any `gos_Texture_Solid` whose source had an alpha
channel, and resolve `gos_Texture_Detect` → Alpha/Solid the same way GL does.
Upstream's comment on `makeKindaSolid` names this exact case — "happens when
drawing terrain, see TerrainQuad::draw() case when no detail and no overlay but
isCement is true". alariq hit it on the GL port; our vk backend was written
without it.

**Verified and closed.** Headless screenshot at building 13: black band and
fog-white square both gone, pavement continuous. Then a user play-test (jalance)
— Mission 1 completed, Mission 2 started and panned around, no visible flaws at
any of the spots he'd been tracking. The repro quicksave is kept at
`docs/bugs/saves/` in case of regression.

**How it was found, and why it took so long.** The Xcode Metal frame debugger,
driven by hand (its MCP is no help — see the previous entry). Chain: draw 495
(`vertexCount:66` = 11 quads) green-outlines *exactly* the hole regions → its
vertex data is perfect (grey `0.573`, proper UVs `0.008…0.992`, unfogged, sane
rhw) → its descriptor at offset `0xAF0` samples `Texture 0x9ae4e6a80` → that
texture is a correct cement image reading **`R≈0.580 G 0.573 B≈0.537 A 0`**.

The earlier "root-caused to MoltenVK/Metal rasterization" conclusion was
**wrong**, and it steered several sessions into dead ends — validation layers,
guard-band clipping, depth clamp, viewport orientation, ring aliasing. All came
back clean because nothing was wrong at the API or rasterizer level. That
conclusion had been reached by *elimination* ("identical geometry in, different
pixels out") without ever confirming a mechanism. The lesson worth keeping:
elimination is not a root cause, and what actually cracked it was inspecting a
**sampled texel's value** — data, not state. Also worth noting the bug was
sitting in plain sight in the GL source the whole time, with a comment
describing it; a diff of the two backends' texture-load paths would have found
it in an hour.

## 2026-07-27 — vk cement-holes: validation layers clean, guard-band refuted

Two more experiments against the cement holes, both **negative**, plus a
correction to the plan that was carried into this session. Detail in
`docs/bugs/2026-07-24-vk-cement-holes-FINDINGS.md`.

**Validation layers, both APIs — clean.** Ran the headless repro under
`VK_LAYER_KHRONOS_validation` and then under Metal API validation. Vulkan flagged
exactly two things, neither raster-related: swapchain binary-semaphore reuse
(`VUID-vkQueueSubmit-pSignalSemaphores-00067` — a genuine presentation-sync bug
to fix separately, either per-image semaphores or `VK_KHR_swapchain_maintenance1`)
and live objects at `vkDestroyDevice`. Metal API validation: zero errors, zero
warnings, holes still reproduced. Metal *shader/GPU* validation is unusable here
— it stalls the game before the first frame. Net: the holes are not API misuse
on either side, they are correct calls producing wrong rasterization.

**Guard-band / large-coordinate rasterization — refuted.** This had been the
leading hypothesis: D3D `XYZRHW` pre-transformed terrain reaches x ∈ −2781..+5203
in a 2048-wide space, D3D never frustum-clips those verts, and fixed-point
rasterizer guard bands are finite. New gated hook `MC2_VK_GUARDCLIP`
(`rendervk/gameos_graphics.cpp`) does a screen-space Sutherland-Hodgman clip of
every triangle to the viewport in `emitDraw` — the one choke point all of
`gos_DrawQuads`/`gos_DrawTriangles`/`gos_RenderIndexedArray` pass through.
Perspective-correct for XYZRHW: `x,y,z,rhw` lerp directly (screen-linear),
`u,v` go through `u*rhw`/`v*rhw` and divide back, so clipped edges don't skew.
It drops **~21% of all triangles as fully off-screen** and clips ~5% more, so
after it runs nothing Metal sees exceeds screen+64px — and the frame is
**pixel-identical** to the unclipped run. Refuted in both directions.

Worth carrying forward: cropping the black band shows it bounded by a **clean
straight diagonal edge**, with buildings and crates drawing correctly on top.
A straight edge means rasterized primitive coverage, not absent geometry — so
"something draws black there" now looks more likely than "pavement fails to
draw", which the earlier depth-based "no fragment" inference had implied.

Also corrected a planning assumption: **Apple's Xcode MCP (`xcrun mcpbridge`)
cannot help with this class of bug.** Its ~21-24 tools are file ops, build/test,
diagnostics, docs search and SwiftUI previews — no Metal, GPU-capture or
Instruments tools — and it bridges into a running Xcode with an open project,
which a CMake tree doesn't have. Nor is there any headless `.gputrace` reader
(`xctrace` is Instruments/timing; `GPUTools*.framework` is private and GUI-only).
Reading a capture stays a human-in-Xcode step. Local gotcha: Xcode lives on
`/Volumes/Media` here and `xcode-select` points at the CLT instance, so `xcrun`
finds no Xcode tools until it's repointed.

## 2026-07-27 — vk cement-holes: experiments + the D3D→GL→VK clipping research

Used the new `tools/vkprobe` harness to run four gated experiments against the
cement holes (full detail in `docs/bugs/2026-07-24-vk-cement-holes-FINDINGS.md`).
All **refuted**: `MC2_CEMENT_SOLID` (DRAWSOLID base — made holes *worse*),
`MC2_VK_POSVIEWPORT` (Y-flip via projection + positive viewport — unchanged),
ring-buffer aliasing (refuted by code — `ringAlloc` can't wrap), and
`MC2_VK_DEPTHCLAMP` (depth clamp on / clip off — unchanged).

Key reframing came from **external research** (per
[[feedback-external-research]]), which I'd neglected for several sessions:
`gos_VERTEX` is D3D `D3DFVF_XYZRHW` **pre-transformed** verts, which Direct3D
**never frustum-clips — guard-band only**. Two VK porting traps follow: (1) depth
clip vs clamp — Vulkan defaults to clip, DX assumes clamp (`VK_EXT_depth_clip_enable`
exists for exactly this) — tested and refuted (we already CPU-cull depth to
[0,1)); (2) XY guard-band clipping of the engine's huge off-screen terrain
triangles — untested, the leading remaining suspect. Sources: GameDev.net XYZRHW
clipping threads, MS D3D9 transformed-vertex docs, Vulkan `VK_EXT_depth_clip_enable`
/ `VK_EXT_depth_clamp_control` docs, MoltenVK + DXVK issue trackers. The
`MC2_VK_DEPTHCLAMP` path also enables the `depthClamp` device feature in
`gos_render.cpp` (harmless when the pipeline flag is off).

## 2026-07-25 — Headless renderer-probe harness (`MC2_LOAD_SAVE` + `tools/vkprobe`)

Not a bug fix — tooling, to stop re-deriving the same launch→warp→capture→quit
dance by hand every bug-hunt session. The black-quad/cement-holes repro lives at
building 13 (Mission 1), and reaching it meant `-mission mc2_02` then two synthetic
`sendkeys load` keystrokes (ctrl+alt+shift+Z) 8s apart with the window forced
frontmost — fragile, and it takes over the desktop *input*, so it can't run
unattended.

**Engine dev hook `MC2_LOAD_SAVE=<path|1>`** (`code/mission.cpp`,
`code/mechcmd2.cpp`). `Mission::update()` arms a wall-clock timer on the first
mission frame and, after `MC2_LOAD_SAVE_SECS` (default 6s), sets the existing
`loadInMissionSave` flag — the *same* flag the quickload hotkey sets — so the
proven `mission->load()` path runs with no keyboard input. The handler
(`mechcmd2.cpp:2277`) resolves the env value: a value containing a path separator
loads that explicit `.ims`; a bare token like `1` (or the hotkey with the env
unset) keeps the historical `savePath/testgame.ims`. Wall-clock, not turn-based,
so a script can predict when the load lands. Gated `#ifndef FINAL`, next to the
other dev hooks.

**Harness `tools/vkprobe/run.sh`.** Parameterized launch→settle→capture→clean-quit:
`--save/--load-secs`, `--capture screenshot|gputrace|log`, `--at/--quit`, `--bin`
(GL vs vk), `--out`. Screenshots via `screencapture -x` (game is
full-screen-desktop, so a display grab = the game); gputrace wires
`METAL_CAPTURE_ENABLED` + `MC2_VK_CAPTURE_AT_SECS`; passes through any `MC2_*` the
caller exported (`MC2_VK_DEBUG`, `TRACEPX`, `RIALOG`, …). Evidence lands under
`docs/bugs/<date>-vkprobe/` (gitignored — retail-derived pixels).

Validated first run: `run.sh --save 1 --at 45` reached building 13 headlessly and
the screenshot captured **both** cement-holes symptoms (fog-white square + the
black band). The game still runs full-screen (covers the display ~60s), so still
announce before running — but no synthetic input, so it no longer hijacks the
keyboard/mouse. Note native capture resolution is 6016×3384 (Retina 2×).

## 2026-07-23 — Metal frame capture on MoltenVK, for the black-quad hunt

Not a bug fix — tooling. The vk-only "black/fog-colored ground quad" near the
Mission 1 drop zone has resisted every printf-style approach: the previous
round's `MC2_VK_TRACEPX` per-pixel draw trace depends on naming the right
render-space pixel, and screenshot coordinates don't map to it cleanly (the
game renders 2048x1080 while `screencapture` returns 6016x3384 at a different
aspect, so traced pixels kept landing on the wrong terrain). The question is
"which draw wrote *this* pixel", and that is what a GPU capture answers
directly.

mc2-vk runs Vulkan on Metal via MoltenVK, so Xcode's Metal frame debugger sees
the real draws — per-pixel draw history, per-draw blend/depth state, bound
textures, fragment shader debugging — with no coordinate math at all: click
the pixel.

**How it's wired:** `rendervk/gos_metal_capture.{h,mm}` (Objective-C++, built
only on `APPLE`, no-op inline stubs elsewhere). The MTLDevice behind MoltenVK
comes out through `VK_EXT_metal_objects` (advertised rev 2 on the M4 Pro):
`VkExportMetalObjectCreateInfoEXT` chained into `VkDeviceCreateInfo::pNext` at
creation, then `vkExportMetalObjectsEXT` + `VkExportMetalDeviceInfoEXT` to
read the handle. Capturing the device covers every queue on it, so no
command-queue export is needed. Capture brackets one frame:
`metal_capture_frame_begin()` in `vk_begin_frame` after the acquire (past the
last point the frame can be skipped, before any command buffer opens) and
`metal_capture_frame_end()` at the tail of `swap_window`. The pairing is exact
because both sites sit behind the same `frame_active` guard. `frame_end` does
a `vkDeviceWaitIdle` first — MoltenVK's present command buffer can still be in
flight, and stopping the capture under it truncates the trace.

**Triggering:** `MC2_VK_CAPTURE_AT_SECS=<n>` captures the first frame past
that much uptime, one per run, to `MC2_VK_CAPTURE_FILE` (default
`/tmp/mc2-vk.gputrace`). Time-based rather than hotkey-based because the warp
repro is already wall-clock scripted (load, sendkeys, settle), so the existing
scripts need no changes. Requires `METAL_CAPTURE_ENABLED=1` in the
environment; the entitlement route (`com.apple.security.get-task-allow`)
turned out not to be necessary for a locally built binary. Nothing is enabled
unless the env var is set — an ordinary run doesn't even request the device
extension, so device creation is byte-identical to before.

**Gotcha:** Metal will not overwrite an existing trace, and it fails at
*stopCapture* — after the frame you wanted is already gone. Init therefore
`stat`s the output path and refuses up front.

Verified end to end: a 5s smoke capture wrote a 256 MB trace containing the
CAMetalLayer, heaps and textures. Reading the black-quad trace in Xcode is the
next step and is at-keyboard work.
## 2026-07-20 — Building glass flips see-through ↔ dark with camera pan (original-MS transparency-sort wart, both backends — provenance CONFIRMED via shared-source diff; fix attempted 2026-07-21 & REVERTED, needs depth sorting)

**Symptom:** on `mc2-vk` (Mission 1), a building's glazing flips appearance
with nothing but a small camera move — no unit or state change. Captured
live as a screenshot pair around a slight pan/zoom: the control-tower cab
on a low building rendered its angled roof glass **fully see-through** in
one frame (pavement clearly visible *through* the panels, as if there were
no glass) and **opaque dark filled glass** in the next. Same static
building, same panels both frames. Evidence: `docs/bugs/2026-07-20-task4/mc2_glass_transparent.png`
/ `mc2_glass_filled.png` (cab crops `zoom_glass_transparent.png` /
`zoom_glass_filled.png`).

**Which state is the bug: the see-through one.** Traced the glass path.
Building windows are submitted **untextured** — `TG_Shape::Render` sends
window faces via `addVertices(0xffffffff, gVertex, MC2_DRAWALPHA)`
(`tgl.cpp:2671`), a sentinel handle, not a real texture; their colour comes
entirely from per-vertex lighting. Window verts carry the magic value
`0xffff00ff` ("hot pink"), which the lighting pass *replaces*: at night →
lit-window colour, and **in daytime → dark grey `0x2f2f2f`**
(`tgl.cpp:1763-1768`). So the **dark filled glass is the correct daytime
look**; the see-through frame is the anomaly (the dark-grey alpha surface
failing to cover).

**Correction to the earlier draft of this entry — glass does NOT constrain
the mech blue↔red flip; they are different paths.** The glass is untextured
(above), so there is no texture to mis-bind — which *rules out* the
descriptor/texture-binding family for the glass, but also means it says
nothing about the mech. Mech team colour is a **per-instance recoloured
texture**: `Mech3DAppearance::setPaintScheme` (`mech3d.cpp:1619`) reads base
texture pixels (`baseColor = *textureMemory`) and rewrites their RGB per the
paint scheme. So the mech flip is texture-based, the glass flip is
untextured-alpha compositing — treat them as **separate bugs**. (Retracts the
prior "rules out team-color selection logic" claim, which wrongly assumed a
shared cause.)

> **Correction, 2026-07-31:** the "**(binding/descriptor class)**" label
> originally attached to the mech flip in the sentence above was **wrong**, and
> has been struck from it. The mech flip was solved 2026-07-31: it is a BGR
> get/set round-trip asymmetry in `Mech3DAppearance::resetPaintScheme`, in
> `mclib`, reproducing on **both** backends — no binding, no descriptor, no
> renderer code. The "separate bugs" conclusion this paragraph draws is still
> correct; only the proposed class for the mech half was wrong. This is the
> second place that bad label was recorded, which is why it needed correcting
> here as well as in the finding-3 entry itself.

**Root cause found — it's a documented pre-existing engine bug, not vk.**
The window pass carries an upstream FIXME describing this exact symptom
(`txmmgr.cpp:1412-1414`):

> *because some objects are drawn with alpha blend + depth write rather than
> alpha test, if order is wrong then objects which are behind may not be
> visible: e.g. hangar in first mission - under some angles "window" mesh is
> drawn after hangar shell and it is failing depth test*

The window (untextured, `MC2_DRAWALPHA`) is drawn in the alpha pass with
**`ZWrite = 1`** and **`ZCompare = 1` (LEQUAL)** (`txmmgr.cpp:1404-1408`),
in reverse node order (`i = nextAvailableVertexNode-1 … --`,
`txmmgr.cpp:1418`). The building **shell** is drawn solid first and writes
depth; then at some camera angles the window mesh — coplanar/just inside the
shell — **fails the depth test and is skipped**, so nothing covers those
pixels and the ground shows through ("see-through" frame). At other angles
it passes and composites the dark-grey glass ("filled" frame). The FIXME
even names *"hangar in first mission"* — the building in our shots.

**This is shared, backend-agnostic code** (draw order + depth state live in
`txmmgr.cpp`, identical on GL and vk), so it is **not a vk-parity bug** — the
user confirmed it reproduces on GL too. "Both backends" is **not** on its own
proof of "original 2001 game" (shared code includes the port layer), so we
settled provenance by **diffing the pristine Microsoft 2006 shared-source**
`txmmgr.cpp` (`SimonDarksideJ/MechCommander2-Source`, `Source/MCLib/`).

**PROVENANCE CONFIRMED — the root cause is original Microsoft code, not the
port.** The original's `MC_TextureManager::renderLists` window/alpha pass
sets **`ZCompare = 1` (LEQUAL) and `ZWrite = 1` (depth-write ON)**
(`ms_txmmgr.cpp:796,800`) and then draws the non-terrain / non-shadow /
non-compass / non-crater `MC2_DRAWALPHA` nodes — i.e. windows — in that
depth-writing alpha pass (`ms_txmmgr.cpp:803-810`). That is **identical** to
ours (`txmmgr.cpp:1404-1408`): the "alpha-blend + depth-write ⇒
order-dependent depth-test failure" design is Microsoft's. So the original
2001 game had the *same* latent window-vs-shell depth fragility. The user's
doubt was reasonable but the diff resolves it: **original-engine wart.**

**What the port changed here is *mitigation*, not cause.** Original is
**single-pass, forward** iteration (`for i=0 … ++`). sebi added a two-pass
alpha-test split (2018) and flipped the loop to **reverse** order (2026,
`txmmgr.cpp:1418`) — a crude back-to-front attempt — with the FIXME
conceding it still isn't fixed. Net: the port has been trying (imperfectly)
to paper over an original MS bug. One consequence worth noting: because the
port reordered the alpha draws, the *exact* camera angles that show the
see-through may not match the 2001 original frame-for-frame, even though the
underlying wart is the same.

**Classification: original-engine bug → "warts and all".** Same bucket as
task 18. Distinct from the black-quad finding below (opaque, camera-invariant,
core-terrain) and the mech flip (textured, `setPaintScheme`).

**FIX ATTEMPTED 2026-07-21, then REVERTED — leave it as the wart. Three
approaches tried, all dead ends; recorded so nobody re-runs them:**

1. **`ZWrite = 0` on the whole alpha-blend sub-pass** (blended geometry tests
   depth but doesn't write it). *Mostly* fixed the flip — but because nothing
   in the pass then writes depth, hidden panes of a glass-box building stopped
   being occluded and **poked out past the silhouette** ("glass outside the
   building"). Traded the flip for transparency overdraw.
2. **Fixed per-vertex z-bias on windows** (nudge `gVertex.z` toward the camera
   in the `isWindow` submit). Can't work: NDC depth is **non-linear**, so one
   constant is too strong on near buildings (glass floats in front) and too
   weak on far ones (still flips) — confirmed live, three buildings in one
   view showing all three states at once.
3. **Hardware polygon offset** on the blend sub-pass (new `gos_State_ZBias`,
   `glPolygonOffset` on GL + `depthBias` in the vk pipeline). **Zero effect
   even at a huge `-32` constant / `-8` slope.** That is the tell: a depth
   problem could not survive that. Culling is already off for this pass
   (`gos_Cull_None`, `txmmgr.cpp:1045`), so it's not facing either.

**Real diagnosis:** the flip is **transparent-vs-transparent**, not
glass-vs-wall. Blended glass panes depth-**write** and reject *each other*;
which pane wins depends on draw order, which shifts with camera angle.
Uniform depth bias moves them all together so it changes nothing (why #3 did
nothing); `ZWrite=0` stops the mutual rejection but then nothing is hidden
(why #1 overdrew). The only correct fix is **back-to-front depth sorting of
the transparent triangles**, which this engine never had — it batches by
*texture node*, not depth. That's a real renderer feature (its own project),
out of proportion to a cosmetic original wart, so we're leaving it. If
someone builds transparency sorting later, this is the payoff case. All fix
code reverted; engine tree matches the original.

---

## 2026-07-20 — UPDATE: black quad is NOT an object/mech shadow and is not gated by any graphics option (task 4)

> **CLOSED — this is the cement/pavement-holes bug, solved 2026-07-27
> (zero-alpha textures). The negative results below are sound; the
> "texture fails to bind / terrain lighting goes black" reframing at the
> end is not the mechanism. See the 2026-07-27 and 2026-07-29 entries.**

**Live toggle test (user-driven, vk, Mission 1) narrows the black quad
substantially — read the two entries below for the prior investigation,
then this.** Toggling the in-game **Shadows** and **Local Shadows** options
removed the mech/unit shadows as expected but left the recurring black quad
**unchanged**. That is a clean negative result: the black quad is **not**
drawn by the object-shadow path (`useShadows` → `TG_Shape::MultiTransformShadows`
/ `bldgShadowShape`, the `argb = 0x3f000000` quads at `tgl.cpp:3274`) — the
subsystem both entries below assume it belongs to. The remaining graphics
toggles (**Detail Textures**, **High Object Detail**, **Non-Weapon
Effects**) also had **no effect** on it.

**Independently, static analysis had already shown the object-shadow blend
path can't produce opaque black:** under the shadow draw's `AlphaInvAlpha`
blend, source alpha is `vertexA(0x3f→0.247) × texA ≤ 0.247`, so a single
shadow draw can at most darken the surface ~25% — it is mathematically
incapable of solid black. Verified end-to-end that the vk blend state,
pipeline blend attachment, vertex `argb` unpack (offset 16, `R8G8B8A8_UNORM`,
`.bgra` swizzle), fragment math (`c = Color.bgra; c *= tex_color`), and
depth compare/write all match GL exactly. So the "shadow rendering fully
opaque / blend-state bug" conclusion in the entries below is **wrong** — not
the mechanism, and now not even the right subsystem.

**Where that leaves it:** the quad is opaque, sharp-edged, world-anchored,
flat on pavement/roofs, survives every graphics toggle, and does **not**
flip with the camera. That profile says **core terrain/building
rendering** — a surface whose texture fails to bind (rendering black) on vk,
or a terrain/decal lighting value (`crater.cpp` colors decals with
`argb = lightRGB`, `crater.cpp:452`) resolving to black. Also unresolved:
the vk-parity framing still rests on a single "GL-clean" screenshot at this
spot — a same-spot GL capture is needed to confirm it's a vk divergence at
all and not original-engine. Next step is `MC2_VK_DEBUG=1` last-draw logging
at the repro to name the actual draw call. **OPUS** once the draw is
identified.

---

## 2026-07-20 — UPDATE: black quad may be a missing/failed draw, not a blend-alpha bug — see turret-pit sighting

> **CLOSED — cement/pavement holes, solved 2026-07-27. The instinct here
> was right: nothing was being painted. The cause was a zero-alpha texture,
> not a failing cover/floor draw.**

**This revises the "building shadows render fully opaque" entry below —
read that one first, then this.** Same recurring black quad (same
hangar-13/wall location tracked through this whole session), new vantage:
screenshot `docs/bugs/2026-07-20-task4/mc2_turret_xray.png` (crop `docs/bugs/2026-07-20-task4/zoom_turret.png`) shows the black
area sitting directly over a closed SRM turret emplacement — and *inside*
the black region, the turret's retracted rocket-tube cluster and mast are
clearly visible, fully rendered, not just implied. The user's read is
exactly right: that mechanism should be hidden/occluded when the turret is
retracted (per game design, it only rises when an enemy approaches), and it
shouldn't be visible right now.

That's a different signature than "shadow rendering too opaque." Seeing
occluded geometry through the black area is more consistent with **some
covering surface failing to draw at all** on vk (a ground patch or turret
"lid" that should render over the pit) than with an alpha-blend bug on a
shadow quad. Where earlier sightings showed nothing underneath (the hangar
pad, the tower roof), that read as "solid black shadow" — but there may
have been a pit/void there too, just with nothing behind it worth seeing,
so the two explanations were indistinguishable until this vantage.

**Net effect: the earlier entry's root-cause conclusion (blend-state bug on
`MC2_ISSHADOWS` shadow draws) should be treated as unconfirmed again, not
settled.** Both explanations point at the vk backend failing to draw
something it should — either the shadow quad's blend state, or a separate
cover/floor draw — but which one (or whether it's the same draw call at
all) isn't established. Whoever picks this up next should look at what's
*supposed* to render over a closed turret pit before assuming the shadow
codepath is the culprit.

---

## 2026-07-20 — Downed mech flips team-color skin (blue ↔ red) between frames on vk (task 4 — SOLVED 2026-07-31, root cause below is WRONG)

> **CLOSED 2026-07-31. The descriptor-cache-collision hypothesis in this entry is
> REFUTED.** It is not a Vulkan bug and not a cache/binding bug: it reproduces on
> GL too, and the cause is a BGR get/set round-trip asymmetry in
> `Mech3DAppearance::resetPaintScheme`'s early-return path, in `mclib`, with no
> renderer code involved. The "on vk" in this heading is also wrong.
> See the 2026-07-31 entry at the top of this log. Kept for the record, and as a
> worked example of a symptom-profile pattern-match that went unchecked against
> the cheap GL-vs-vk test.

**Symptom:** a downed/kneeling mech, same pose and geometry, rendered deep
blue with a cyan chest panel in one screenshot (`docs/bugs/2026-07-20-task4/mc2_downed_mech_1.png`,
crop `docs/bugs/2026-07-20-task4/zoom_downed_1.png`) and red with a yellow chest panel in a second
screenshot (`docs/bugs/2026-07-20-task4/mc2_downed_mech_2.png`, crop `docs/bugs/2026-07-20-task4/zoom_downed_2.png`) taken
moments later, on `mc2-vk`, with only a small camera pan between the two —
no unit changed, nothing respawned, same mech in the same lance formation
both times.

**This looks like the same bug class as the 2026-07-17 fix**
(descriptor-cache-key collision — see that entry above), not a new
mechanism: stable-looking but *wrong* texture content bound to a draw,
flipping with camera/render state rather than crashing or showing garbage.
That fix keyed the vk descriptor cache on the exact binding tuple (sampler,
3 views, 2 UBOs) instead of a folded hash to close exactly this kind of
collision — this finding means either a case that fix didn't cover, or a
different cache/lookup with the same lossy-key mistake, applied to mech
skin/team-color textures instead of GUI textures. Not yet confirmed which.

**Not investigated yet, but higher-confidence than the other two task-4
findings above** given the direct match to a previously root-caused bug.
Next step: reproduce with `MC2_VK_DEBUG=1` (set at launch) and check
whether the same last-draw diagnostic logging from the 2026-07-17 fix
covers mech/skin texture binds, or needs extending to them. Worth an
**OPUS** look first (targeted, matches an existing fixed pattern) before
considering **FABLE**, per the credit plan's task 4 escalation guidance for
"anything that looks like today's descriptor-collision class."

---

## 2026-07-20 — Destroyed LRM truck wreck shows a black/white checkerboard texture on vk (task 4, open)

> **CLOSED 2026-07-29 — NOT A BUG.** jalance's call on review: it's chunky
> low-resolution wreck art, not wrong/missing texture content. The
> "wrong/missing texture" read below was inferred from screenshots alone
> and never checked against GL.

**Symptom:** during the same task-4 playtesting session (Mission 1), the
wreck of a destroyed LRM truck (confirmed by the user) rendered with a hard
black-and-white checkerboard pattern covering the *entire* model, on
`mc2-vk`. First spotted while investigating the wall-shadow finding above
(screenshot `docs/bugs/2026-07-20-task4/mc2_shadow_wall.png`); a closer follow-up shot isolates just
the wreck — `docs/bugs/2026-07-20-task4/mc2_lrm_wreck.png` (crop `docs/bugs/2026-07-20-task4/zoom_lrm3.png`). At close range it's
clearly a full-model checkerboard, not scorch/damage art — ruling out the
"maybe it's an intentional damage decal" alternative raised in the first
sighting. Nearby soft-edged dot-pattern scorch decal (visible in the same
shots) still looks like normal game art, unrelated.

**Not investigated yet.** Visually this looks more like a wrong/missing
texture than a blend-state problem (sharp checkerboard, not a solid-color
overlay) — plausibly the same general class as the descriptor-cache-key
collision fixed 2026-07-17 (texture content swapped/wrong for a draw), but
that's a guess from screenshots, not confirmed. Not yet checked against GL
at the same spot. Logged as a separate task-4 finding — do not conflate
with the wall/building shadow-blend bug above, they look like different
bug classes.

---

## 2026-07-20 — Building shadows render fully opaque instead of alpha-blended on vk (task 4, open)

> **CLOSED — cement/pavement holes, solved 2026-07-27 (zero-alpha
> textures). The root cause below is wrong; kept for the record.**

**Superseded/revised by the entry above ("UPDATE: black quad may be a
missing/failed draw") — a later sighting of the same quad over a turret pit
showed occluded geometry through it, which doesn't fit "shadow rendering
too opaque." Read this entry for the original investigation, but don't
treat its root-cause conclusion as settled.**

**Symptom:** during task-4 parity playtesting (Mission 1), a sharp-edged
rectangular quad appeared flat on the ground/rooftops near buildings on the
`mc2-vk` build — split between solid black and a normally-lit light-colored
surface (a parking pad, a building roof), reproduced on 2 separate vk runs
(one showing two instances in a single frame, near hangar 13 and near a
second building/tower cluster), absent on one GL run at the same spot.
Screenshots: `docs/bugs/2026-07-20-task4/mc2_shadow_artifact.png`, `docs/bugs/2026-07-20-task4/mc2_shadow_vk2.png`.

**Root cause, confirmed by pixel measurement:** initial live impression
(mine and the user's, from watching the view pan) was that the black region
stayed fixed relative to the *camera* rather than the world — suggesting a
camera-space-vs-world-space bug in shadow direction math. Tested directly:
captured a before/after screenshot pair around a pure camera pan (no unit
movement, mechs off-screen) — `docs/bugs/2026-07-20-task4/mc2_pan_before.png` / `docs/bugs/2026-07-20-task4/mc2_pan_after.png`.
Template-matched a static landmark *on the same building* the shadow was
attached to and measured its on-screen shift precisely: **(+600, +0) px**
at full resolution (6016x3384). Sampled four points inside the black shape
in the "before" frame and checked those exact points offset by that same
(+600, +0) in the "after" frame — all landed on matching near-black values
(e.g. `(1,5,9)`→`(1,5,9)`, `(4,7,11)`→`(1,5,9)`, `(37,42,45)`→`(41,46,49)`).

**Conclusion: the shadow is world-anchored, not camera-anchored** — it
moves in lockstep with the building it belongs to. The camera-relative
impression was parallax: a separately-measured flat-ground landmark (a
guardrail, at a different depth) shifted only (+284, +184) over the *same*
pan, so different objects visibly move different amounts on screen under a
panning perspective camera — normal 3D behavior, easily misread as "this
one thing is following the camera" if compared against the wrong reference.
The mech-shadow-swing entry below (heading-relative rotation) is a
**separate, unrelated bug** — this one is not a rotation/direction problem.

The actual bug: the game's shadow quads are drawn at a fixed
`argb = 0x3f000000` (~25% opaque black — see `tgl.cpp:3274` etc., found
during the mech-shadow investigation below), i.e. they're *meant* to
lightly darken whatever they're drawn over, not render solid black. What's
rendering here is close to fully opaque (`(0,0,0)`–`(4,7,11)`, not a 25%
blend toward the surface color). That's a blend-state bug on the shadow
draw path — plausibly vk-specific (matches task 4's "OPUS for blend/state
fixes" bucket directly), though only confirmed absent on a single GL data
point so far.

**Not fixed.** Next step is an vk-side look at how `MC2_ISSHADOWS`-flagged
draws (`tgl.cpp:3328`, `mcTextureManager->addVertices(...)`) set up
blending in the vk backend vs. GL — likely alpha blending isn't enabled, or
the alpha channel of `argb` isn't reaching the blend stage. `MC2_VK_DEBUG=1`
(set at launch, can't toggle mid-session) would help confirm which draw
call is responsible. Logged as credit-plan task 4 finding — root cause
narrowed via intake-level investigation, but the actual blend-state fix is
still **OPUS** work per the credit plan.

---

## 2026-07-20 — Mech shadows swing wildly with small heading changes (investigated, not fixed)

> **HYPOTHESIS REFUTED 2026-07-30 — see the entry of that date at the top of
> this log. `s_lightDir` is in *shape* space, so `RotateLight(s_lightDir,
> rotation)` is the correct inverse of the yaw in `worldToShape`, not a second
> application of it. Do NOT implement the fix proposed below; removing that call
> would introduce the very bug it was thought to cause. The symptom is real but
> is original-engine shadow quality (flat-ground projection, pitch never undone),
> confirmed equally bad on GL and vk. The `git blame` provenance note below
> still stands.**

**Symptom:** playtesting Training 1, unit shadows appear to swing/rescale
dramatically with the smallest mech movement — as if the "sun" were a point
light only a few feet away rather than a fixed distant directional light.
Reproduces on **both** GL and VK backends, so it's not a task-4 VK-parity
issue; it's either an original-engine bug or a latent one exposed by this
port. Not yet confirmed against the retail game (no side-by-side available).

**Root-cause hypothesis (not yet verified by a fix/test cycle):**
`TG_Shape::MultiTransformShadows` (`mclib/tgl.cpp:2850`) computes each mech's
shadow by projecting its local-space vertices onto the ground plane along the
directional ("infinite") sun light. It first transforms the vertex to true
world space via the shape's full `shapeToWorld` matrix (which already bakes
in the mech's current yaw — built from `shapeOrigin.BuildRotation(*rot)` in
`TG_MultiShape::TransformMultiShape`, `mclib/msl.cpp:1299`), then *also*
rotates the world-space sun direction itself by that same yaw
(`RotateLight(s_lightDir, rotation)` at `tgl.cpp:2910`, where `rotation` is
the global `yawRotation` set from the mech's Euler yaw). That looks like a
double application of the mech's orientation: the light direction ends up
coupled to the mech's own heading, so as the mech turns even slightly, the
effective sun angle used for its shadow swings with it — producing exactly
the "light is right next to me" look reported, rather than a fixed world
sun direction shared by every object.

Confirmed via `git blame` that this exact call (`RotateLight(lightDir,
rotation)`, since renamed to `s_lightDir`) predates the alariq OpenGL port —
it's in the original Microsoft shared-source commit (`63e58e9`), not
something introduced by the port. So if this is a real bug, it's an
original-engine one we'd be choosing to fix, not a porting regression.

**Not fixed.** This needs an OPUS pass: confirm the hypothesis (e.g. log the
effective light direction per mech per frame and correlate with heading),
decide whether `RotateLight(s_lightDir, rotation)` should be removed/changed
without breaking the (single-shape, non-multi) `TG_LIGHT_TERRAIN` case at
`tgl.cpp:2027`, which does a related-looking but different rotation (rotates
the *vertex* into world orientation rather than rotating the *light*, and
isn't part of the shadow path). Logged as credit-plan task 18.

---

## 2026-07-20 — Clamp window-size requests to usable display bounds

Task 9 from the credit plan. `options.cfg`'s `ResolutionX`/`ResolutionY` (read
in `CPrefs::load()`, `prefs.cpp`) were never validated against the actual
display — a stale config (e.g. saved on a bigger monitor) or a hand-edited
value would flow straight through `CPrefs::applyPrefs()` into
`gos_CreateWindow`/`gos_SetScreenMode` unclamped. The in-game Options menu was
already safe (it only offers SDL-enumerated valid modes); the gap was
specifically config values that bypass that UI.

**Fix:** exposed `SDL_GetDesktopDisplayMode` through the existing `gos_*` API
surface — `gos_GetDesktopDisplayMode(DisplayIndex, *XRes, *YRes, *BitDepth)`,
implemented in both backends (`rendergl`/`rendervk` `gameos_graphics.cpp`),
wrapping the `graphics::get_desktop_display_mode()` helper that already
existed in both (declared in `GameOS/gameos/gos_render.h`) but wasn't wired up
to a caller. `CPrefs::applyPrefs()` (`prefs.cpp`) now queries it for
`this->displayNumber` and clamps `resolutionX`/`resolutionY` down to fit
before `gos_SetScreenMode`. If the configured `DisplayNumber` doesn't resolve
(e.g. referencing a disconnected second monitor), falls back to display 0
rather than silently skipping the clamp. One fix point covers both the boot
path (`mechcmd2.cpp`) and mission-load resolution switch
(`loadscreen.cpp:489`) since both funnel through `applyPrefs()`.

**Ordering trap:** the desktop-mode query only works *after*
`gos_CreateWindow()` has run at least once — SDL's video subsystem isn't
initialized until `graphics::create_window()` calls `SDL_VideoInit()` lazily
on first use (`rendergl/gos_render.cpp:73`). Querying before window creation
(the natural place to put a "clamp before you create the window" check) hit
`SDL_GetDesktopDisplayMode failed: Video subsystem has not been initialized`
on cold boot and silently no-opped, since `gos_GetDesktopDisplayMode` returns
`false` on failure and the clamp code treats "can't determine bounds" as "skip
clamping." Moved the clamp to after `gos_CreateWindow`/`gos_CreateRenderer`
but before `gos_SetScreenMode` — the latter is what actually applies the
steady-state size (`Environment.screenWidth/Height`), so clamping there still
fully fixes the requested behavior; the initial `gos_CreateWindow` call itself
still passes the unclamped size for the very first window creation, but SDL
on macOS appears to fit oversized `SDL_CreateWindow` requests to the display
on its own (observed: requesting 9999x9999 produced an actual window of
3008x1571 immediately, no visible oversized flash), and `gos_SetScreenMode`
corrects it a moment later regardless.

Verified: `MC2_AUTOQUIT_SECS` runs with `ResolutionX/Y = 9999` in
`options.cfg` (both a Debug build, to see the `SPEW` clamp message —
`SPEW`/`_ARMOR` is compiled out in the default `RelWithDebInfo` build — and
the normal build, to confirm no crash/hang) show the clamp firing to the
dev display's 3008x1692 bounds; a normal in-bounds resolution produces no
clamp message (no false positives).

---

## 2026-07-18 — AD-4: asset-directory config; found a silent-forever-hang on missing assets

Task 8 from the credit plan. Every asset path in the codebase (`mclib/paths.cpp`
globals, `FullPathFileName::init`, `File::open`, `FastFile::open`) is a
hardcoded string relative to the process CWD — there was no base-directory
concept anywhere, hence "must launch from the game dir."

**Fix:** resolve an asset root at startup (`-assetdir <path>` CLI flag >
`MC2_ASSET_DIR` env var > CWD, unchanged default) and `chdir()` into it in
`GameOS/gameos/gameosmain.cpp::resolveAssetDirectory()`, called right after
`GetGameOSEnvironment()` and before `Environment.InitializeGameEngine()`. Since
every path in the codebase is CWD-relative, one `chdir()` covers all of them —
no changes needed to `mclib/paths.cpp` or any path-construction code. The
resolved root is validated (a `data/` subdir must exist) before proceeding;
`assetDirOverride` (the parsed `-assetdir` value) lives in `mclib/file.cpp`
next to `CDInstallPath` rather than in `code/mechcmd2.cpp`, because the
`Viewer` tool links `gameos_main` (and thus needs the symbol) without linking
`code/` — same reason `CDInstallPath` lives there.

**Related bug found, not fixed (out of scope for this task):** if a file goes
missing *after* startup (not the whole asset dir — one file), `File::open`
(`mclib/file.cpp:264-303`) and `FastFile::open` (`mclib/ffile.cpp`) fall into a
retry loop that calls `MessageBox()` and checks for `IDCANCEL` to break out.
On this port `MessageBoxA` is stubbed to a `printf` that always `return 0`
(`GameOS/src/platform_winuser.cpp:34-38`) — never equal to `IDCANCEL` — so the
loop never exits: it spams `MSGBOX: ...` to stdout forever with no way to quit
except killing the process. `Environment.checkCDForFiles = false`
(search `code/mechcmd2.cpp` for `checkCDForFiles`; it is set to `true` there —
cited as :2689 when this was written, since drifted to :2701 and then :2708,
hence the symbol) would skip the whole retry path and return a normal
"not found" error instead; that's the likely fix whenever someone picks this
up, plus checking whether legitimate legacy CD-swap use cases still need the
retry loop at all on this port.

---

## 2026-07-18 — Solo Mission screen verified on Vulkan (task 5, PASS); devinput focus gotcha found

First test of the "Solo Mission" flow (main menu's `SOLO MISSION` button →
`MM_MSG_SINGLE_MISSION` → `singleLoadDlg.beginSingleMission()`, an
`MPLoadMap` dialog) since M1, and its first test ever on the Vulkan
backend. Driven live with the `tools/devinput` clickat tool rather than
`-mission` quickstart, since the point was to exercise the menu/dialog
chain itself. Full chain verified clean: main menu → Solo Mission → map
list (all missions populated, thumbnails render correctly, no
corruption) → select a mission (Scouting Patrol) → description/preview
updates correctly → Accept → Mission Briefing (objectives text, VIDCOM
map, hangar art all render) → Next → 'Mech Bay logistics (empty
deployment team, as expected for a fresh solo entry — no canned lance
the way `-mission` quickstart provides) → Main Menu button backs out
cleanly → reopening Solo Mission shows the same list with no
duplication (the regression class the M1-era campaign-list bug was) →
in-game Exit → Yes quit confirmation → clean process exit, no crash.

**Methodology gotcha, now documented in `tools/devinput/README.md`:**
clicks silently failed to register even when the cursor was screenshot-
and log-confirmed to be exactly on the right button. Cause: the game
runs in `SDL_WINDOW_FULLSCREEN_DESKTOP`, which covers the screen without
forcing macOS app-activation — the terminal that launched it stayed the
frontmost app. Cursor *position* tracking is global regardless of focus
(misleading — `MC2_DEBUG_INPUT`'s `osMouse`/`norm` updated correctly),
but click/key delivery requires the target actually be frontmost. Fix:
`osascript -e 'tell application "System Events" to set frontmost of
process "mc2-vk" to true'` before each click. Also learned live from the
user: the cursor's hit-tested point is its tip, not its visual center —
confirm via the button's hover-highlight (side arrows + darker fill)
before trusting a click coordinate, don't just eyeball the sprite.

---

## 2026-07-18 — vk resolution/fullscreen test matrix: intermittent shutdown SIGSEGV found (OPEN), resolution-mismatch theory ruled out

Task 3 from the credit plan: scripted windowed/fullscreen × resolution
matrix on `mc2-vk` using `-mission mc2_01` + `MC2_AUTOQUIT_SECS` +
`screencapture`, options.cfg edited per-run (backed up first, restored
after). Dev machine is a 6K Retina display (native 6016x3384 px /
3008x1692 pt).

**Real finding: intermittent SIGSEGV on clean autoquit shutdown, ~27%
of runs (4/15), reproducing across every windowed and fullscreen config
tested** — not resolution-specific. Same crash signature every time
(confirmed via `.ips` in `~/Library/Logs/DiagnosticReports/`, symbolized
with `atos`):

```
libSDL3.0.dylib (x6 frames) -> libSDL2-2.0.0.dylib (sdl2-compat shim)
-> SoundEngine::destroy() (gameos_sound.cpp:272) -> gos_DestroyAudio()
-> main (gameosmain.cpp)
```

`SoundEngine::destroy()` calls `Mix_HaltChannel` for every channel, then
`gosAudio::destroyAudio` for each active sound, then
`SDL_CloseAudioDevice` / `Mix_CloseAudio` / `Mix_Quit` /
`SDL_QuitSubSystem(SDL_INIT_AUDIO)` — with no pause/lock around the
audio callback first. Prime suspect: a teardown race between the main
thread freeing sound state and SDL's audio callback thread still
running against it, surfacing through the sdl2-compat→SDL3 shim.
Affects the shared `GameOS/gameos/gameos_sound.cpp`, used by both GL
and VK backends — not a Vulkan bug, just more visible under a scripted
rapid launch/autoquit/relaunch loop. **Escalating to OPUS per the
credit plan** (own root-cause hypothesis in hand, two backends share
the code, worth a real fix rather than a triage guess).

**Investigated and ruled out: visible rendering breakage from
resolution/fullscreen mismatch.** `SDL_WINDOW_FULLSCREEN_DESKTOP`
always snaps to the real desktop size regardless of the requested
`ResolutionX`/`ResolutionY` (confirmed: `rendervk/gos_render.cpp`'s
`set_window_fullscreen`), and `Environment.screenWidth/Height` (i.e.
`g_width`/`g_height` in `rendervk/gameos_graphics.cpp`) is set from the
*requested* resolution in `gos_RendererHandleEvents` and never
reconciled against the real post-fullscreen drawable size — confirmed
live via `MC2_DEBUG_INPUT=1`: `envScreen=800x600` stayed pinned while
`sdlWin=3008x1692 sdlDrawable=6016x3384` (a deliberately extreme
fullscreen-at-800x600 test). Despite that, screenshots across
native/2048x1080(default)/800x600 fullscreen requests all show the 3D
scene rendering correctly proportioned, full-frame, no letterboxing —
it must project against the real swapchain extent, not the stale
logical size. Mouse tracking also stays correct under the same
mismatch: `MC2_DEBUG_INPUT`'s `norm` values scale off the real drawable
size, not the stale `envScreen`, so the old cursor-cage bug does not
reproduce here either. The stale `Environment.screenWidth/Height` is
still live in `gos_GetViewport()`'s multiplier math (2D immediate-mode
positioning) — no visible breakage observed in these screenshots, but
untested for precise HUD/click-target alignment under a mismatched
fullscreen config. Low priority given no observed symptom, but worth a
one-line note if anyone touches that code path.

Lesson: verify code-read hypotheses empirically before writing them up
as bugs — the resolution-mismatch theory was plausible from the source
alone, but screenshots and live mouse-debug output showed the actual
render/input paths don't use the stale value the way I expected.

---

## 2026-07-18 — Pre-GitHub asset audit: purged a real retail-data leak from history (FIXED)

Before standing up the private GitHub remote (overdue backup), audited
every non-source/binary file ever tracked for accidental copyrighted
game-data or needless bloat. Two problems, one real risk and one dead
weight:

`Viewer/mission.fst` (20MB, tracked in one commit) turned out to be
actual retail game data, not a fixture: only ~1.3MB of it is readable
strings, and those strings are `data\missions\mc2_NN.fit/.abl/.pak` for
every campaign mission. `system.cfg`, committed alongside it, names
`mission.fst` directly as one of the game's `[FastFiles]` bundles
(`tgl.fst`, `art.fst`, `textures.fst`, etc. are the same format) —
these are packed retail data archives, not anything Microsoft's
shared-source drop ever included. Someone's local Viewer-tool test
fixture, built from a real install, got committed by mistake.

`3rdparty.zip` (22MB) is legitimate but irrelevant to us: prebuilt
**Windows** binaries (SDL2/SDL2_mixer/SDL2_ttf/GLEW/zlib `.dll`/`.lib`/
`.pdb`) for the legacy `.vcproj` Windows build. Zero references anywhere
in our CMake files — it only ever fed the old Visual Studio project,
which we don't build. If a future Windows CMake build ever needs these,
get them from alariq's upstream or vendor via vcpkg; no reason to carry
Windows binaries in a Mac-first fork.

Also dropped `mclib/MCLib.aps` (26KB) — an auto-generated MSVC
resource-compiler cache, pure clutter, no content of note.

Fix: `git filter-repo --invert-paths` for all three paths, across all
history (repo had no remote yet, so this was the one cheap opportunity
before publishing — .git went 53M → 32M). Backed up the pre-filter
history as a bundle outside the repo first. Added `3rdparty.zip` and
`*.aps` to `.gitignore` so they can't silently return; `mission.fst`
was already covered by the existing `*.fst` rule (it predated that
rule, which is exactly how it got in).

---

## 2026-07-17 — M2: textures swap content under churn on Vulkan — descriptor-cache key collision (FIXED)

User report from the first vk playthrough: the mouse cursor flashed
between its icon, a semi-translucent blob, and invisible. Reproduced
without a user: a CGEvent tool sweeps the mouse during
`MC2_VK_DEBUG=1 mc2-vk -mission mc2_01` (iTerm needs Accessibility
approval — and per this session's lesson, after approving a blocked
permission, re-run the blocked test). Once texture churn starts, GUI
textures show *each other's* content: dock buttons render as concrete
slabs, pilot portraits render as mech icons, the minimap gets a circuit
board pasted over it, the cursor sometimes draws as a grey square. The
identical sweep on the GL build stays pixel-perfect.

Established with last-draw-of-frame logging (MC2_VK_DEBUG): the cursor
draw itself is healthy — alive handle, right texture *name* (walk.tga,
the animated walk cursor sheet), right animation UVs, white argb — and
walk.tga even renders correctly in frames where other GUI elements are
wrong. No bad-handle / ring-overflow / descriptor-exhaustion / TGA-decode
diagnostics fire. The TGAs are fine. So handles map to the right stub
textures; what's crossed is which *GPU texture* ends up bound.

Prime suspect: descriptorFor()'s per-frame cache key — sampler, three
view pointers, and two UBO pointers folded together with `*31`. That's
linear over the components: injective in any single component, but
cross-component collisions exist (a view-pointer delta exactly 31x a
sampler delta, etc.), and MoltenVK allocates objects at regular address
strides, so such ratios can recur systematically. A collision is
deterministic within a frame — the first draw to populate the entry
wins, every later collider silently renders with its texture — which
matches the *stable* wrongness (slabs stay slabs) better than any race.

Confirmed by experiment: `MC2_VK_NO_DSET_CACHE=1` (fresh descriptor set
per draw) made every symptom vanish under the same sweep. Fix: the
cache map is now keyed on the exact binding tuple (sampler, 3 views,
2 UBOs — a memcmp-ordered struct) instead of the folded hash. Sweep
with the cache enabled verified clean: correct portraits, dock,
minimap, cursor. The env toggle stays as a diagnostic.

Lesson: never key a cache on a lossy hash of handles when the full
tuple is only 48 bytes — a collision doesn't crash, it silently binds
the wrong resource, and it's *deterministic*, so it masquerades as
asset corruption rather than looking like the cache bug it is.

## 2026-07-17 — M2: fullscreen boots stale at 800x600 on Vulkan (exclusive fullscreen + macOS spaces)

First user playthrough on the vk build hit three symptoms at once: the FMV
intro and main menu drew as an 800x600 patch inside an otherwise-black
fullscreen (self-healed after an alt-tab), the OS cursor stayed visible
alongside the game's drawn cursor, and going fullscreen blanked the user's
second monitor and rearranged every window on the desktop (macOS did not
restore the layout afterward).

One root cause. The vk backend's `set_window_fullscreen` used **exclusive
`SDL_WINDOW_FULLSCREEN`** where the GL path deliberately uses
`SDL_WINDOW_FULLSCREEN_DESKTOP` (its source even carries the commented-out
rejected alternative). On macOS, exclusive fullscreen on an 800x600 window
enters a fullscreen space but SDL never delivers the resize that grows the
surface: the CAMetalLayer — and thus the swapchain — stays 800x600, and
because no `SIZE_CHANGED` event fires, `set_mouse_capture` never re-runs, so
`SDL_ShowCursor(SDL_DISABLE)` is never re-asserted either. Alt-tab forces
the space transition to settle, which finally emits the resize → drawable
refresh + swapchain recreate → everything snaps correct. The monitor
blanking is the same flag one layer up: exclusive fullscreen performs a
real display mode switch (800x600), which macOS treats as a display
reconfiguration — secondary displays blank during capture and every
desktop window is re-laid-out against the new geometry, and macOS is bad
at putting them back. Desktop fullscreen resizes the window immediately
(the whole event chain fires on its own), never touches the display mode,
and behaves like the green zoom button — other monitors unaffected.

Fixed by matching the GL path's proven window contract in
`rendervk/gos_render.cpp`: `SDL_WINDOW_FULLSCREEN_DESKTOP`, re-center on
return to windowed, `is_window_fullscreen` tests both fullscreen flags, and
`SDL_WINDOW_ALLOW_HIGHDPI` at window creation — the vk surface had been
running at Retina *points* (half resolution, silently upscaled); with the
flag the swapchain runs at native pixels like GL. Mouse math is unaffected:
`handleMouseMotion` already scales points→drawable by ratio, which was 1.0
before and 2.0 now, same as GL.

Lesson: the GL backend's window/SDL glue encodes years of platform fixes —
when writing a second backend, diff the *flags and call order* against it,
not just the rendering. (Same lesson as the texture-contract bugs below,
one layer down.)

## 2026-07-17 — M2: missions render on Vulkan (retained path complete)

The mech-mesh path (lighted materials + lights/scene UBOs), FMV YCbCr, and
real GPU buffers landed; `-mission mc2_01` on MoltenVK now visually matches
the GL reference. Two bugs found by GL-vs-VK screenshot diffing, both
2001-era contracts the backend must honor, not Vulkan problems:

**Terrain and mechs rendered with red/blue swapped** (brown ground came out
steel blue) → the game writes D3D-convention BGRA DWORDs into locked
textures; the GL path converts to BGRA on Lock and back to RGBA on Unlock,
and the Vulkan backend skipped that dance. Mirrored the in-place round trip.

**Control-panel chrome vanished / wrong textures on GUI quads** → the vk
backend reused destroyed texture-handle slots, but txmmgr's cache keeps
stale handles across cache-out and expects them to stay distinct — GL's
handle table is append-only. Never reuse handles. (Symptom was surreal:
concrete building slabs where the button dock should be.)

Lesson: when a legacy game misbehaves on a new backend, diff against the
old backend's *implementation*, not just its output — both fixes were
faithfully reproducing GL-side quirks, not writing better Vulkan.

## 2026-07-17 — M2: menus render on Vulkan (immediate-mode path complete)

Same-day follow-up to the backend split: the gos immediate-mode path
(quads/tris/lines/points + drawText) now renders for real on MoltenVK — the
full main menu is pixel-faithful to GL on the first successful boot. The
things that made it work on the first try, recorded for the mission-parity
work: (1) copy the GL quirks verbatim, don't "fix" them — the tex shader's
`Color.bgra` swizzle and gos_vertex's divide-by-rhw are load-bearing;
(2) GL uploads matrices with transpose=GL_TRUE from row-major storage, so
push constants need the same transpose or everything vanishes off-screen;
(3) a negative-height viewport (core in Vulkan 1.1) keeps GL's clip-space
orientation, so the GL projection matrix and winding rules port unchanged;
(4) glslc `#include` works with plain relative paths, letting the five
shaders share one push-constant block. Retained-buffer draws (mech meshes,
FMV YCbCr) are still no-ops — that's the next slice, then terrain.

## 2026-07-17 — M2 begins: backend split, first MoltenVK frame

Not a bug hunt — a milestone marker with the traps we dodged recorded.
The renderer audit (docs/RENDERER_AUDIT.md) found the gos_* API is a clean
waist: all GL was already confined to 5 files, except two leaks (raw GL in
gameosmain.cpp's frame loop; a `gl_utils.h` include in txmmgr.cpp that only
wanted the packed-color helpers — moved to utils/vec). After plugging those,
the split was file moves: GL implementation → `rendergl/`, new `rendervk/`
selected by `cmake -DMC2_RENDERER=VULKAN` (GL stays default).

First Vulkan frame (teal clear, presented inside the real game loop, clean
autoquit) worked on the first run. MoltenVK specifics that mattered:
`VK_KHR_portability_enumeration` + the portability instance flag (the brew
vulkan-loader hides non-conformant devices otherwise), enabling
`VK_KHR_portability_subset` on the device because MoltenVK advertises it,
and `SDL_Vulkan_LoadLibrary` needing a fallback path to
`/opt/homebrew/lib/libvulkan.dylib` on dev machines (the shipped .app will
bundle MoltenVK instead). Deps: `brew install molten-vk vulkan-headers
vulkan-loader`. The null gos_* backend backs texture Lock/Unlock with
correctly sized buffers (decoded via the shared Image code) and loads real
glyph metrics — the game's logistics/GUI code runs happily against it,
which is the parity-porting workbench for everything that comes next.

## 2026-07-17 — Windowed mode & resolution switching work; the cursor-cage hunt

Session goal: graphics testing (windowed mode, resolution changes). Windowed
mode = `b FullScreen = FALSE` in options.cfg (port uses SDL desktop-fullscreen
otherwise; window is drag-resizable). Resolution dropdown (Options→Graphics)
is the port's SDL-mode enumeration; changes apply at next mission load, menus
stay 800x600 by design. Fixes, in escalating order of difficulty:

**Dropdown claimed current res was 5120x2880** → prefs didn't match any SDL
mode (legacy retail options.cfg stores BitDepth as a 0/1 index, not 16/32) and
the no-match fallback picked item 0 = the largest mode. Fix: normalize legacy
BitDepth on load; fall back exact → size-only → closest-area, never item 0.

**Every resolution listed 2-3x** → SDL enumerates one mode per refresh rate;
dropdown shows only WxHxD. Deduped at the dropdown.

**Close button dead, ctrl-c dead** → `SDL_WINDOWEVENT_CLOSE` was never
handled, and the focus-lost gate in process_events discarded *all* events
while unfocused — including the SDL_QUIT synthesized from SIGINT. Quit
events now bypass the gate.

**THE CAGE: in-game cursor confined to a menu-sized box after launching a
mission through the menus** (direct `-mission` launches unaffected). Wrong
theories first: stale `Environment.drawable*` (fixed anyway — resize events
were discarded; real bug, wasn't this one), normalized-vs-points confusion in
`gos_SetMousePosition` (also real, also fixed, also not this). The
breakthrough was `MC2_DEBUG_INPUT=1` (new, kept): per-second dump of the
whole coordinate chain showed every engine value consistent, but the *OS
cursor's global position* pinned inside [239-1079]x[94-784] — an 800x600
box at the new window's origin. Cause: `SDL_SetWindowGrab` was enabled while
the 800x600 menu window existed; SDL/macOS confinement rect never re-applied
across the window's resize to mission resolution. Fix: grab only in
fullscreen (windowed grab also makes window chrome unreachable — upstream
had this condition commented out), re-apply capture on resize events.
Lesson: when input coordinates look right but movement is bounded, suspect
OS-level confinement, not arithmetic — and instrument before theorizing.

## 2026-07-17 — M1 playthrough observations triaged (user played Training 1+2 to completion)

User-verified in play: mission load, unit orders, pathing, combat (targeted
and destroyed a truck), win triggers, campaign progression (next mission
unlocked), edge-scroll, wheel zoom, minimap jump, keyboard shortcuts.
Findings from the run:

**Campaigns listed twice in New Game** → side effect of unpacking
`data/campaign/` to disk: `LogisticsSaveDialog::initDialog` scans the dir
for `*.fit` (finds campaign + tutorial), then a hardcoded fallback ("may be
in fast files") adds the same two again — retail never saw this because
those two lived only in the FST, so the scan came up empty. Fix: fallback
now skips names the scan already added (`isInGameList`); both packed and
unpacked layouts work.

**Vidcom static on Training mission-select** → not a bug: `tutorial.fit`
declares `Video = ""`, so the static overlay is the authentic no-briefing
placeholder. All 185 campaign briefing videos are present as converted
`.mpg` (`STANDIN`, `NODE_*`, etc.), so Carver V should play real briefings
— worth confirming when someone plays the main campaign.

**`i=0..24` spew during mission load** → upstream's forgotten debug printf
in `CSpecificEnemyUnitObjectiveCondition::Read` (objective.cpp) — removed.

## 2026-07-17 — First mission running on macOS (mc2_01 in-game, zero code changes)

**Solo Mission list would have been empty** → the mission browser
(`MPLoadMap::seedDialog`) enumerates `data/missions/*.fit` with
`FindFirstFile`, and the port's emulation (platform_winbase.cpp) is a plain
`scandir` of the real filesystem — fast files are invisible to it. Retail
worked because the installer put `data\missions\` on disk (Win32's real
FindFirstFile can't see into FSTs either; that's also how mission mods drop
in). Our game dir only had the FSTs. Fix is deployment, not engine:
`makefst -d -f mission.fst -p <out>` unpacks (output nests under
`<out>/mission.fst/`), then copy `data/missions/` (36 MB, 180 files) and
`data/campaign/` into the game dir. File precedence is disk-first
(`File::open` tries `_open` before `FastFileFind`), so on-disk missions
override FST copies — retail modding behavior preserved.

**Unattended in-mission testing works out of the box**: the original devs
left a `-mission <name>` command line flag (ParseCommandLine, mechcmd2.cpp)
that sets `justStartMission` and boots straight into the mission with
`MISSION_LOAD_SP_QUICKSTART` (canned 3-mech lance, no logistics screens).
`MC2_AUTOQUIT_SECS=45 ./mc2 -mission mc2_01` + `screencapture` gave visual
proof: terrain, mechs, pilot bar, minimap, compass and hint bar all render;
45 s stable, clean exit. First time the tactical game has run on macOS ARM64
— and it needed **no code changes**, only the data deployment above.

The `i=0..24` stdout spew during load is upstream's leftover debug printf in
`CSpecificEnemyUnitObjectiveCondition::Read` (objective.cpp:447) — harmless,
candidate for removal. Remaining M1 exit criterion: a human start-to-finish
playthrough (win/loss flow, mission results screens).

## 2026-07-17 — ESC-skipping the intro no longer strands the menu on black

**Menu drawn over solid black for ~8 s after ESC-skipping the intro movie**
(the 2026-07-16 UPDATE item) → last session's hypothesis (skip vs. finish
taking different state paths) was wrong: both movie endings converge on the
same `delete introMovie` in `MainMenu::update`. The real mechanism is a
**held-key leak plus a reveal that never actually skips**:

1. `userInput->getKeyDown` is level-triggered (PRESSED *or* HELD). A human ESC
   press spans several frames; frame N stops/deletes the movie, and on frame
   N+1 the *same key press* falls into the splash-reveal branch and sets
   `introOver = true` — the flag that unhides the menu buttons.
2. But `introOver` never touched the "rectfade" animation, and
   `MainMenu::render` draws `intro.render()` unconditionally — so the reveal's
   opaque-black hold (t=0–8 in `mcl_splashscreenintro.fit`) kept covering the
   background while the menu text/buttons drew on top of it. Hence: menu on
   black until t=8, background fades in at t=8–11.
   Letting the movie finish never sets `introOver`, so the menu stays hidden
   until the reveal completes — that's why only the skip path looked broken.

Fix: added `aAnimation::skipToEnd()` (jump past the last keyframe so
`isDone()` is true and color/position report final values — the rectfade's
final color is transparent) + `aAnimObject` passthrough. `MainMenu::update`
now fast-forwards the splash animObjects both when the movie is skipped and
when ESC is pressed during the reveal, so any skip cuts straight to the menu
with the background visible. Natural completion still plays the designed
fade. Anim clocks here advance by `frameLength` per `update()` call, not
wall-clock — worth remembering for future sequencing bugs.

## 2026-07-16 — M1: menu fully interactive; exit crash and "black background" resolved

**Mouse could not move past the middle of the screen** → SDL mouse events are
in window points; the engine normalizes cursor position against the drawable
size in pixels (2x on Retina), so the cursor topped out at exactly 50% per
axis → convert coords and deltas to pixel space at capture time
(gos_input.cpp) using the window-to-drawable ratio. Upstream never sees this
because points == pixels on their platforms.

**SIGTRAP on exit (Apple crash dialog)** → the lldb disassembly showed
`GOSImagePool::~GOSImagePool` complete-object destructor (D1) compiled to a
single `brk #1`. GOSImagePool is abstract (pure LoadImage) with a
**non-virtual destructor**, and MLRTexturePool does `delete imagePool` through
the base pointer — which statically calls the abstract class's D1. Apple's
toolchain emits a trap for that impossible symbol; on Linux it "works" and
silently skips the derived destructor. Fix: make the destructor virtual.
Original-2001 bug, present upstream too. Repro was fully automated with a new
dev hook: env var `MC2_AUTOQUIT_SECS=N` drives the normal quit path after N
seconds (kept for future smoke tests).

**UPDATE (user observation, end of session):** the black period depends on how
the intro movie ends. If the movie is **skipped with ESC**, the menu comes up
and sits black until the background reveal starts; if the movie **plays to
completion**, the background reveal begins before the menu is drawn, so no
black screen is perceived. So the skip path has a sequencing difference —
likely the splash-intro/beginAnim state machine starts its clock at different
moments relative to menu draw depending on how introMovie terminates
(MainMenu::update movie-skip path vs movie-finished path, mainmenu.cpp
~445-470). **To investigate next session** — the fix is probably to make the
ESC-skip path enter the same state the natural-completion path does.

**Menu background stays black for 10–15 s** → see UPDATE above; the data-driven
reveal below is real but the *perceived* issue is the ESC path. Profiling showed the
game idle at vsync (nothing loading); per-second state instrumentation showed
`SplashIntro.animObjects[0]` running for ~12 s after the intro movie. The
animation data (`art/mcl_splashscreenintro.fit`, "rectfade") deliberately
holds opaque black until t=8 then fades out by t=11 — the original designers'
reveal, comment in the file confirms. Feels broken only because the port
skips the retail FASA/intro cinematics that used to fill that gap. Escape
skips it (existing code path). If it still annoys later: candidate for an
opt-in "fast boot" tweak, not an engine change.

**Also fixed:** `gos_GetHiResTime` divided nanoseconds by 10.0e+9 instead of
1.0e+9 (sub-second time ran at 1/10 speed); only test code consumes it today,
but it was a landmine.

## 2026-07-16 — M1: first boot on macOS (engine runs, shaders compile clean)

Game data: built entirely from alariq's mc2srcdata repo using our arm64-built
tools (aseconv/makefst/makersp/pak/text_tool + ffmpeg); `make all
BUILD_PLATFORM=linux` worked unmodified on macOS, zero errors. Game dir at
`~/Games/mc2-port` (shared-source data only). The user's retail RIP
(`~/Games/MechCommander2`) turned out to lack camera/effect/insignia.fst and
the port's own converted font assets (.bmp/.glyph vs retail .d3f) — pure
shared-source data is the cleaner path; retail remains available for
movies/sound comparison later.

Boot battles, in order:

**dlopen of libmc2res_64.so failed** → the resource DLL replacement is built as
`.dylib` on macOS but mechcmd2.cpp hardcoded `.so` → `__APPLE__` branch loads
the dylib name. (Also: deploy `build/out/res/libmc2res_64.dylib` next to mc2.)

**"Please insert the MechCommander 2 CD" loop** → retail RIP lacked 3 of the 8
FSTs listed in system.cfg → solved properly by building all 8 from mc2srcdata.

**SIGSEGV calling address 0x0 right after GL context creation** → engine
unconditionally calls `glDebugMessageControlARB`/`glDebugMessageCallbackARB`;
ARB_debug_output doesn't exist on Apple's GL (frozen at 4.1, debug output is
4.3-era) so GLEW leaves NULLs → gated on `GLEW_ARB_debug_output`. Note: macOS
happily gave us a real GL **4.1 core** context on the M4 Pro ("4.1 Metal - 90.5"),
and `glGetString(GL_EXTENSIONS)` returning NULL in core profile is expected.

**Every shader failed: "version '420' is not supported"** → the engine injects
`#version 420` (gameos_graphics.cpp); macOS caps at GLSL 410 → inject 410 on
Apple. The only 420 feature used was `layout(binding=N)` on UBOs — removed from
shaders; the engine already sets binding points via `glUniformBlockBinding`
(txmmgr.cpp calls gos_SetRenderMaterialUniformBlockBindingPoint every render).
This answers OQ-3: the port needs nothing beyond 4.1 + those bindings.

**Apple GLSL strictness trio** → (1) `const float x = ubo_member;` — const
locals need constant expressions before GLSL 420 → dropped const. (2)
`uint & 0x00ffffff` — hex literal is signed int, no implicit conversion in
`&` → `u` suffix. (3) link error "fragment input 'CameraPos' not written by
vertex shader" — upstream had commented out the write (undefined behavior that
other drivers tolerate) → write `g_scene.cameraPos.xyz`, the clearly intended
value.

Result: engine boots, audio opens (22050 Hz stereo), all shaders compile and
link, main loop runs until killed. Visual verification of the main menu:
pending user eyes.

Deployment recipe (until scripted): game dir needs mc2, libmc2res_64.dylib,
shaders/, 8 .fst, *.cfg + testtxm.tga from mc2srcdata/root, data/, assets/.

## 2026-07-16 — M0: first native Apple Silicon build (clean `cmake -B build && cmake --build build`)

Vendoring alariq/mc2 @ 35af1c2 and getting it to compile on macOS ARM64 took
one LFS surprise and seven distinct porting battles, all in the predicted
categories (x86 assumptions, Darwin libc differences, LP64 type identity,
missing-on-mac headers). CMake configure passed untouched on the first try.

**Clone checkout produced an empty tree** → upstream stores `3rdparty.zip` in
Git LFS and `git-lfs` wasn't installed, so the smudge filter aborted checkout →
`brew install git-lfs`, copy LFS objects locally. (The zip is prebuilt Windows
libs; mac/linux builds don't use it.)

**`invalid output constraint '=A' in asm` in gameos.hpp** → `rdtsc()` helper is
raw x86 opcode bytes (`.byte 0x0f, 0x31`) → guarded per-arch: x86 keeps the
asm, ARM64 reads the generic-timer virtual counter (`mrs cntvct_el0`), anything
else falls back to `clock_gettime(CLOCK_MONOTONIC_RAW)`.

**`no member named 'st_atim' in 'stat'`** (platform_winbase.cpp, file_utils.cpp)
→ Darwin names the timespec fields `st_atimespec`/`st_mtimespec`/`st_ctimespec`,
not POSIX `st_*tim` → `#define` aliases under `__APPLE__`.

**`<SDL2/SDL.h> not found`** → Homebrew's `/opt/homebrew/include` isn't a
default compiler search path (on Linux `/usr/include` is) → top-level CMake
adds `${SDL2_PREFIX}/include` when `APPLE`.

**`no matching function for call to 'Read'` for `unsigned long*`**
(memorystream, inifile `readIdULong` call sites) → THE recurring Darwin trap:
on glibc LP64 `uint64_t` *is* `unsigned long`, so overloads taking
`uint64_t*` accept `unsigned long*`; on Darwin `uint64_t` is
`unsigned long long` and `unsigned long` is a distinct (same-size) type →
added Darwin-only `long`/`unsigned long` overloads to `MemoryStreamIO`
Read/Write and a delegating `readIdULong(unsigned long&)` to `FitIniFile`,
rather than touching dozens of call sites. Expect this class of error in any
file we later enable.

**`cannot add 'abi_tag' attribute in a redeclaration`** → `stuff/style.hpp`
hand-declared placement `operator new`; libc++ declares it with ABI attributes
and rejects the plain redeclaration (upstream had already `#if 0`-ed the
*definition* with the comment "placement new cannot be overridden") → replaced
the declaration with `#include <new>`.

**`'malloc.h' file not found`** (13 files) → Darwin has no `<malloc.h>`;
malloc is in `<stdlib.h>` → guarded includes tree-wide. Three include
spellings exist (`<malloc.h>`, `"malloc.h"`, no-space) — grep for all of them.

**function-pointer → `void*` argument rejected** (vfx_ellipse.cpp) → GCC's
`-fpermissive` tolerates the implicit conversion, Apple clang does not →
explicit casts at the two call sites.

**x86 stack-walk asm in heap.cpp** (`mov %%rsp`) → debug-only "who called me"
helper → non-x86 now returns early, mirroring upstream's own `_WIN64` bail-out.

Not yet verified: the binary hasn't been *run* (that's M1, needs user assets).
No subsystems were stubbed — every file compiles for real.
