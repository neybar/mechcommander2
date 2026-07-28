#!/usr/bin/env bash
#
# vkprobe/run.sh — headless MC2 renderer-probe harness.
#
# Encodes the launch -> auto-load-save -> settle -> capture -> clean-quit dance
# we kept re-deriving by hand every bug-hunt session. It leans on the
# MC2_LOAD_SAVE engine dev hook (code/mission.cpp) to reach a known camera
# position WITHOUT synthetic keyboard input — no `sendkeys`, no window
# activation, no desktop-input takeover. (The game still runs full-screen and
# covers the display for the duration; announce before running, per
# docs/ENGINEERING_LOG + feedback-announce-machine-control.)
#
# Capture modes:
#   screenshot  screencapture the display N secs in (default)
#   gputrace    Xcode Metal frame capture (needs full Xcode.app to open)
#   log         just collect stdout (pair with MC2_VK_DEBUG / MC2_VK_TRACEPX=…)
#
# Evidence is written under an out dir (default docs/bugs/<date>-vkprobe/, which
# is gitignored — retail-derived pixels never get committed). Only the caller's
# .md writeups are tracked.
#
# Examples:
#   # Cement-holes screenshot at building 13 (the testgame.ims quicksave):
#   tools/vkprobe/run.sh --save 1 --at 40 --quit 50 --out /tmp/cement
#
#   # Per-pixel triangle trace at an apron pixel, log only:
#   MC2_VK_DEBUG=1 MC2_VK_TRACEPX="1004,117" \
#     tools/vkprobe/run.sh --save 1 --capture log --quit 45
#
#   # GL vs vk side-by-side (run twice, --bin each):
#   tools/vkprobe/run.sh --bin ~/Games/mc2-port/mc2    --save 1 --out /tmp/gl
#   tools/vkprobe/run.sh --bin ~/Games/mc2-port/mc2-vk --save 1 --out /tmp/vk
#
set -euo pipefail

# ---- defaults ---------------------------------------------------------------
BIN="${MC2_VKPROBE_BIN:-$HOME/Games/mc2-port/mc2-vk}"
GAMEDIR="${MC2_VKPROBE_GAMEDIR:-$HOME/Games/mc2-port}"
MISSION="mc2_02"          # host mission started before the save auto-loads
SAVE=""                    # MC2_LOAD_SAVE value: "1" = default testgame.ims, or an .ims path
LOAD_SECS=4                # MC2_LOAD_SAVE_SECS: secs into host mission before load fires
CAPTURE="screenshot"       # screenshot | gputrace | log
AT=40                      # secs after launch to grab the screenshot / gputrace
QUIT=""                    # MC2_AUTOQUIT_SECS; default = AT + 12
OUT=""                     # evidence dir; default docs/bugs/<date>-vkprobe
TAG="probe"                # basename stem for artifacts

usage() { sed -n '2,40p' "$0"; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --bin)       BIN="$2"; shift 2;;
    --gamedir)   GAMEDIR="$2"; shift 2;;
    --mission)   MISSION="$2"; shift 2;;
    --save)      SAVE="$2"; shift 2;;
    --load-secs) LOAD_SECS="$2"; shift 2;;
    --capture)   CAPTURE="$2"; shift 2;;
    --at)        AT="$2"; shift 2;;
    --quit)      QUIT="$2"; shift 2;;
    --out)       OUT="$2"; shift 2;;
    --tag)       TAG="$2"; shift 2;;
    -h|--help)   usage 0;;
    *) echo "unknown arg: $1" >&2; usage 1;;
  esac
done

[ -x "$BIN" ] || { echo "vkprobe: binary not found/executable: $BIN" >&2; exit 1; }
[ -d "$GAMEDIR" ] || { echo "vkprobe: game dir not found: $GAMEDIR" >&2; exit 1; }
[ -n "$QUIT" ] || QUIT=$(( AT + 12 ))
[ -n "$OUT" ] || OUT="$(cd "$(dirname "$0")/../.." && pwd)/docs/bugs/$(date +%Y-%m-%d)-vkprobe"
mkdir -p "$OUT"
# Make OUT absolute: the game runs from GAMEDIR, so a relative --out would make
# the game-written .gputrace (and any game-side dump) land under GAMEDIR, not here.
OUT="$(cd "$OUT" && pwd)"

STAMP="$(date +%H%M%S)"
LOG="$OUT/${TAG}_${STAMP}.log"

# ---- env for the child ------------------------------------------------------
# Pass through any MC2_* the caller already exported (MC2_VK_DEBUG, TRACEPX,
# RIALOG, FOG_DEBUG, …); we only add the harness-controlled ones.
export MC2_AUTOQUIT_SECS="$QUIT"
if [ -n "$SAVE" ]; then
  export MC2_LOAD_SAVE="$SAVE"
  export MC2_LOAD_SAVE_SECS="$LOAD_SECS"
fi
if [ "$CAPTURE" = "gputrace" ]; then
  export METAL_CAPTURE_ENABLED=1
  export MC2_VK_CAPTURE_AT_SECS="$AT"
  export MC2_VK_CAPTURE_FILE="$OUT/${TAG}_${STAMP}.gputrace"
fi

echo "vkprobe: bin=$BIN"
echo "vkprobe: mission=$MISSION save='${SAVE:-<none>}' load_secs=$LOAD_SECS"
echo "vkprobe: capture=$CAPTURE at=${AT}s quit=${QUIT}s"
echo "vkprobe: out=$OUT  log=$LOG"

# ---- launch -----------------------------------------------------------------
( cd "$GAMEDIR" && "$BIN" -mission "$MISSION" ) >"$LOG" 2>&1 &
PID=$!
echo "vkprobe: launched pid=$PID; the game will cover the display until it quits"

cleanup() { kill "$PID" 2>/dev/null || true; }
trap cleanup EXIT

# ---- capture ----------------------------------------------------------------
case "$CAPTURE" in
  screenshot)
    sleep "$AT"
    SHOT="$OUT/${TAG}_${STAMP}.png"
    # -x: silent. Full display; the game is full-screen-desktop so this is it.
    screencapture -x "$SHOT" || echo "vkprobe: screencapture failed" >&2
    echo "vkprobe: screenshot -> $SHOT"
    ;;
  gputrace)
    # capture is driven inside the engine at MC2_VK_CAPTURE_AT_SECS; just wait
    sleep $(( AT + 5 ))
    echo "vkprobe: gputrace (if written) -> $MC2_VK_CAPTURE_FILE"
    ;;
  log)
    : # nothing to grab mid-run; stdout is already going to $LOG
    ;;
  *)
    echo "vkprobe: unknown capture mode: $CAPTURE" >&2; exit 1;;
esac

# ---- wait for clean quit ----------------------------------------------------
# AUTOQUIT drives the normal quit path; give it a grace window, then force.
for _ in $(seq 1 30); do
  kill -0 "$PID" 2>/dev/null || break
  sleep 1
done
if kill -0 "$PID" 2>/dev/null; then
  echo "vkprobe: still running past AUTOQUIT+grace; terminating"
  kill "$PID" 2>/dev/null || true
fi
trap - EXIT
wait "$PID" 2>/dev/null || true

echo "vkprobe: done. artifacts in $OUT"
[ "$CAPTURE" = "log" ] && echo "vkprobe: --- tail of log ---" && tail -30 "$LOG"
exit 0
