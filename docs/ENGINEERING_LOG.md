# Engineering Log

One entry per significant bug hunt or porting battle: **symptom → cause → fix**.
Newest entries at the top. Practice borrowed from the
[Generals Mac port](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad).

---

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
