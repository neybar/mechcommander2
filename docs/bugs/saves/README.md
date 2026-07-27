# Bug-repro save games

Save files kept so a fixed bug can be re-checked later, or a regression caught.
The `.ims` files themselves are **gitignored** (`docs/bugs/**` keeps only `.md`)
— they're derived retail game state, same rule as the screenshots and frame
captures. This README is the tracked index; if you don't have the save on disk,
you'll need to recreate it or get it from jalance.

## `2026-07-27-building13-pavement-holes.ims`

Copy of `~/.mechcommander2/savegame/testgame.ims` as of 2026-07-22 15:32
(sha256 `7d1448da…3168b9e2`, 1285091 bytes).

Mission 1 (`mc2_02`), camera parked at **building 13 / the drop zone**. This is
the repro position for the **vk cement/pavement holes** bug — the fog-white
squares and black band — fixed 2026-07-27 in commit `41f81b3` (zero-alpha
textures; see `../2026-07-24-vk-cement-holes-FINDINGS.md`). Kept because the fix
was verified from one camera position only, so this is the known-good comparison
point if the holes ever come back.

**To restore:**

```sh
cp docs/bugs/saves/2026-07-27-building13-pavement-holes.ims \
   ~/.mechcommander2/savegame/testgame.ims
```

**To reach it headlessly** (no synthetic input, ~60s, game covers the display):

```sh
tools/vkprobe/run.sh --save 1 --capture screenshot --at 45
```

`--save 1` resolves to this default `testgame.ims` via the `MC2_LOAD_SAVE`
engine hook. Use `--bin ~/Games/mc2-port/mc2` for the GL comparison.
