# Bug-repro save games

Save files kept so a fixed bug can be re-checked later, or a regression caught.
The `.ims` files themselves are **gitignored** (`docs/bugs/**` keeps only `.md`)
— they're derived retail game state, same rule as the screenshots and frame
captures. This README is the tracked index; if you don't have the save on disk,
you'll need to recreate it or get it from jalance.

## `2026-07-31-task4-downed-mech-paint-flip.ims`

Copy of `~/.mechcommander2/savegame/testgame.ims` as of 2026-07-31 18:23
(sha256 `266cf9d0…aba4918c`, 1096923 bytes).

Camera parked with a **downed enemy mech in the viewport** — the repro position
for **task-4 finding 3**, the team-colour skin flipping blue↔red (root-caused and
fixed 2026-07-31; see ENGINEERING_LOG). Archived from jalance's own quicksave
during that hunt.

The flip is **camera-dependent**: it needs an LOD transition on the mech, so the
save alone is not enough — someone has to pan. That makes this a *starting
position*, not a self-contained repro like the pavement-holes save, and it is why
`--play` exists (below): no dev hook can drive the camera.

> **⚠ This save carries corrupted colours and CANNOT be used to judge whether
> team colours are correct.** It was captured with a pre-fix binary, so
> `mover.cpp:7375` baked each mover's colour in at whatever R/B parity it
> happened to be at. In the post-fix log
> (`docs/bugs/2026-07-31-vkprobe/paintfixed_191138.log:59-66`) two movers of the
> *same team* sit permanently at `inst=2b8004d` and `inst=6a80149` — one is the
> exact swap of the other, stable across every LOD transition. Use this save to
> check **"does it still flip?"** only. For **"is the colour right?"** start a
> fresh mission, where colours come from mission data instead of the file. See
> `../2026-07-31-paintscheme-baked-into-saves.md`.

```sh
MC2_PAINT_DEBUG=1 tools/vkprobe/run.sh --play \
  --save "$(pwd)/docs/bugs/saves/2026-07-31-task4-downed-mech-paint-flip.ims"
```

`--play` hands the session to a human — no `MC2_AUTOQUIT_SECS`, no harness
force-kill — so you can pan until it flips. Add `--bin ~/Games/mc2-port/mc2` for
the GL comparison; the code involved (`Mech3DAppearance::resetPaintScheme`) is in
`mclib`, so both backends behave identically. `MC2_PAINT_DEBUG` was temporary
instrumentation, removed after the fix — it is not a live hook, so the line above
is inert unless you re-add it (see the ENGINEERING_LOG entry for what it printed).

**Go through `run.sh`, not a bare `./build-vk/mc2`.** The game resolves its assets
from the CWD unless told otherwise (`resolveAssetDirectory`, `gameosmain.cpp`),
and there is no `data/` at the repo root — a raw invocation from here `exit(1)`s
with "no 'data' directory found" before any dev hook fires. `run.sh` `cd`s into
`$GAMEDIR` first. (An earlier draft of this entry got that wrong.)

The save itself is from **mission 3**, but leave the host mission at the
`vkprobe` default **`mc2_02`**. The host is only a shell to boot into before the
save loads, and **`mc2_03`'s opening script latches "movie mode"** — letterbox
bars, no GUI chrome (`execSetMovieMode`, `code/ablmc2.cpp:5189`, registered as the
ABL call `setmoviemode` at `:6863`). `Camera::inMovieMode` is a process-global
`static bool` (`mclib/camera.h`) that `Camera::save()` never serializes, and it
clears only via `endMovieMode()`'s letterbox animation or a fresh mission init —
neither of which `MC2_LOAD_SAVE` triggers. So a save loaded during that script is
stuck letterboxed with no way out. Mission 2's opening doesn't do this. Verified
the hard way on 2026-07-31.

## `2026-07-27-building13-pavement-holes.ims`

Copy of `~/.mechcommander2/savegame/testgame.ims` as of 2026-07-22 15:32
(sha256 `7d1448da…3168b9e2`, 1285091 bytes).

Mission 1 (`mc2_02`), camera parked at **building 13 / the drop zone**. This is
the repro position for the **vk cement/pavement holes** bug — the fog-white
squares and black band — fixed 2026-07-27 in commit `41f81b3` (zero-alpha
textures; see `../2026-07-24-vk-cement-holes-FINDINGS.md`). Kept because the fix
was verified from one camera position only, so this is the known-good comparison
point if the holes ever come back.

**To run it — pass the path, don't copy over the quicksave.** `MC2_LOAD_SAVE`
takes an explicit `.ims` whenever the value contains a path separator
(`mechcmd2.cpp`, `strpbrk(loadEnv, "/\\")`), so the harness can load an archived
save directly and `~/.mechcommander2/savegame/testgame.ims` is never touched:

```sh
tools/vkprobe/run.sh --capture screenshot --at 45 \
  --save "$(pwd)/docs/bugs/saves/2026-07-27-building13-pavement-holes.ims"
```

Add `--bin ~/Games/mc2-port/mc2` for the GL comparison.

**Why it matters:** `--save 1` uses the *live* `testgame.ims`, and the user's own
play-testing overwrites that constantly — a 2026-07-27 verification run silently
landed in the wrong mission on natural terrain because Mission 2 progress had
replaced the repro. Always pass the explicit path for archived saves; there is
never a reason to `cp` over someone's quicksave.
