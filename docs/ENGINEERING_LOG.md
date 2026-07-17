# Engineering Log

One entry per significant bug hunt or porting battle: **symptom → cause → fix**.
Newest entries at the top. Practice borrowed from the
[Generals Mac port](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad).

---

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
