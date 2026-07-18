# Engineering Log

One entry per significant bug hunt or porting battle: **symptom → cause → fix**.
Newest entries at the top. Practice borrowed from the
[Generals Mac port](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad).

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
