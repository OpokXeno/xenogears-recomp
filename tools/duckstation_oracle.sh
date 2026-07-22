#!/usr/bin/env bash
# Launch the patched DuckStation oracle (psxrecomp JSON-over-TCP on port 4371).
#
# Modes:
#   bios            headless BIOS boot — live oracle for the recompiled BIOS
#   disc [cue]      headless boot of a game disc (default: ../../game/disc1.cue)
#                   — live oracle for Xenogears itself
#   gui [cue]       GUI with full debugger (CPU/VRAM/breakpoints); optionally
#                   booting a disc
#   ping            query the running oracle (uses tools/dbg.py on port 4371)
#   stop            kill the oracle
#
# Examples:
#   bash tools/duckstation_oracle.sh bios
#   bash tools/duckstation_oracle.sh disc /home/pc/xenogears-port/game/disc2.cue
#   bash tools/duckstation_oracle.sh ping '{"cmd":"ping"}'
#   PSX_DBG_PORT=4371 python3 tools/dbg.py '{"cmd":"state"}'   # same, manual

set -euo pipefail

GAME_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$GAME_ROOT/psxrecomp/duckstation/build/bin"
EXE="$BIN_DIR/duckstation-qt"
DEFAULT_DISC="/home/pc/xenogears-port/XenogearsRecomp/game/disc1.cue"
LOG=/tmp/duckstation_oracle.log

mode="${1:-}"; shift || true

case "$mode" in
    bios|disc|gui)
        if [ ! -x "$EXE" ]; then
            echo "not found: $EXE — run bash tools/duckstation_oracle_setup.sh first" >&2
            exit 1
        fi
        args=()
        case "$mode" in
            bios) args=(-bios -nogui -fastboot) ;;
            disc) args=(-nogui -fastboot -- "${1:-$DEFAULT_DISC}") ;;
            gui)  if [ $# -gt 0 ]; then args=(-- "$1"); fi ;;
        esac
        # DuckStation resolves settings relative to its cwd in portable mode.
        # -nogui only hides the main window; the render window still opens, so
        # headless modes run under Xvfb (windows land on a virtual display,
        # invisible on the desktop; Qt offscreen platform makes DS exit).
        cd "$BIN_DIR"
        if [ "$mode" != gui ]; then
            setsid xvfb-run -a -s "-screen 0 640x512x24" "$EXE" "${args[@]}" </dev/null >"$LOG" 2>&1 &
        else
            setsid "$EXE" "${args[@]}" </dev/null >"$LOG" 2>&1 &
        fi
        echo "oracle launched (pid $!, log $LOG): $EXE ${args[*]}"
        echo "wait ~5s, then: bash tools/duckstation_oracle.sh ping"
        ;;
    ping)
        payload="${1:-{\"cmd\":\"ping\"}}"
        PSX_DBG_PORT=4371 python3 "$GAME_ROOT/tools/dbg.py" "$payload"
        ;;
    stop)
        # Bracket trick: matches the binary without matching this script's own
        # command line. TERM first (ConfirmPowerOff=false in settings.ini lets
        # it exit without a dialog), KILL as fallback.
        pkill -f 'duckstation-q[t]' 2>/dev/null || true
        for _ in 1 2 3 4 5; do
            pgrep -f 'duckstation-q[t]' >/dev/null || { echo "oracle stopped"; exit 0; }
            sleep 1
        done
        pkill -9 -f 'duckstation-q[t]' 2>/dev/null || true
        pkill -f 'xvfb-ru[n].*duckstation' 2>/dev/null || true
        echo "oracle killed"
        ;;
    *)
        sed -n '2,20p' "$0"
        exit 2
        ;;
esac
