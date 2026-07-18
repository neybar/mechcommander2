# devinput

Tiny synthetic-input tools for headless GUI testing on macOS, used to drive
the game's mouse/keyboard without a human at the desk (screenshot-diffing
menus, exercising the load screen, poking hotkeys, etc.).

They post real HID events via `CGEvent` / `cghidEventTap` — the same path a
physical mouse/keyboard takes — so the app under test can't tell the
difference. This requires the terminal (or whatever process runs them) to
have **Accessibility** permission in System Settings → Privacy & Security.

**These move the real desktop pointer and send real keystrokes.** Whatever
app is frontmost receives them — always announce before running one, per
[[feedback-announce-machine-control]].

**The target app must actually be frontmost, not just visually on top.**
Desktop-fullscreen (`SDL_WINDOW_FULLSCREEN_DESKTOP`, what this game uses)
covers the screen without forcing macOS app-activation — launching the
game from a terminal leaves the terminal as the frontmost app even though
you can't see it. Cursor *position* tracking works globally regardless of
focus (the game's own `MC2_DEBUG_INPUT` log will show `osMouse` updating
correctly), which can mislead you into thinking clicks should land — but
`mouseDown`/`mouseUp` and key events are only delivered to whichever app
is actually frontmost. Activate it first:

```sh
osascript -e 'tell application "System Events" to set frontmost of process "mc2-vk" to true'
```

Symptom if you skip this: `clickat` visibly moves the cursor to the right
spot (confirmed via screenshot and the game's own mouse-debug log) but
the click has no effect — easy to misdiagnose as a coordinate-math bug
when it's actually a focus problem.

## Build

Each is a single-file Swift script; compile with `swiftc`:

```sh
swiftc mousemove.swift -o mousemove
swiftc clickat.swift   -o clickat
swiftc sendkeys.swift  -o sendkeys
```

## Tools

- **mousemove [seconds]** — sweeps the cursor in a Lissajous pattern for
  `seconds` (default 20) across the middle of the screen. Useful for
  shaking loose anything that depends on continuous mouse movement
  (e.g. cursor-follow rendering, hover state).
- **clickat x y** — moves to `(x, y)` in desktop points and performs a
  left click (move → down → up, with short settle delays so SDL sees a
  clean click rather than a drag).
- **sendkeys <combo>** — sends one of a fixed set of key combos:
  `save` (ctrl+alt+shift+X), `load` (ctrl+alt+shift+Z), `f5`, `f8`, `esc`.
  Modifiers are pressed before and released after the main key so the
  target app's modifier-state tracking (SDL2) stays consistent.

## Notes

- Coordinates are desktop points, not pixels — on a Retina display these
  are half the native pixel resolution.
- `sendkeys`'s combo table is hardcoded to this project's current
  save/load/quicksave hotkeys; update it if those bindings change.
