# Pre-fix savegames have R↔B-swapped team colours baked in

**Found:** 2026-07-31, while fixing task-4 finding 3 (the team-colour flip).
**Status:** open, not fixed. Low severity, cosmetic, affects old saves only.
**Backends:** both — this is `mclib`/`code` logic, nothing renderer-specific.

## What

Mover paint schemes are serialized into savegames:

- **save:** `code/mover.cpp:7375` — `getAppearance()->getPaintScheme(data->psRed, data->psGreen, data->psBlue)`
- **load:** `code/mover.cpp:7541` — `getAppearance()->resetPaintScheme(data->psRed, data->psGreen, data->psBlue)`

Before the 2026-07-31 fix, `-DBGR` made `getPaintScheme` return an **R↔B-swapped**
colour, and the `resetPaintScheme` early-return path made that swap oscillate on
every LOD transition (see the ENGINEERING_LOG entry). A save therefore captured
each mover at **whatever parity it happened to be at that instant** — the
corruption is in the data, not just in memory, and it is *per-mover*, not
uniform.

Visible in `docs/bugs/2026-07-31-vkprobe/paintfixed_191138.log`: two movers of
the same team sit permanently at `inst=2b8004d` and `inst=6a80149`, one the exact
R/B swap of the other, stable across every LOD transition.

## Consequence of the fix

With `-DBGR` gone, `get`/`set` are the identity, so a stored value is replayed
verbatim — which also removes the accidental self-correction. For a **pre-fix
save**:

- **before:** the unit flipped between correct and swapped on every LOD
  crossing — visibly flickering
- **after:** the unit loads at whatever parity was captured and **stays there**

The fix does not make the stored data worse, but it changes the symptom from
"flickers" to "silently wrong", which is easier to mistake for correct behaviour.
Worth knowing before someone reports "a mech is the wrong colour" off an old save
and it gets hunted as a new bug.

**Corollary for bug-hunting:** a save written by a pre-fix build cannot be used
to judge whether colours are *correct* — only whether they *flip*. Two
conclusions were briefly drawn from exactly that mistake during the original
hunt; see the ENGINEERING_LOG entry. Use a fresh mission for colour checks.

**Saves written after the fix are correct.** This only affects files written by a
build from before 2026-07-31 — including
`docs/bugs/saves/2026-07-31-task4-downed-mech-paint-flip.ims`, which was captured
mid-bug specifically to reproduce it.

## Why it wasn't fixed here

Detecting a swapped stored colour is not reliably possible: an R↔B swap of a
valid colour is another valid colour, and there's no checksum or version marker
to distinguish "authored teal" from "swapped orange". The options are all worse
than the bug:

- **Re-derive on load** instead of trusting the file — ignore `data->ps*` and
  rebuild from the commander/mission colours the way `mission.cpp:1088-1114`
  does at spawn. Correct for stock units, but would **discard any legitimately
  customised scheme**, and multiplayer/`MPlayer` colours take a different path.
- **Bump the savegame version and migrate** — the honest fix, but it means
  touching save compatibility for a purely cosmetic issue on files that are
  themselves mid-campaign scratch state.

Filed rather than fixed, per the "note it, don't slide into fixing it" habit. If
a save-format version bump happens for another reason, fold the migration in
then.
