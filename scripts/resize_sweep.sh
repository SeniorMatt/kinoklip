#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Interactive live-resize measurement under KWin: shows the Edit page at 1280x720
# (test_resizebench interactive), resizes it with Meta + right-button drag (KWin's own
# interactive resize, so every step is a compositor configure the client must answer with a
# frame) to about 1920x1080 in 8 px steps at 60 Hz through kk_fakeinput, and prints the
# per-step statistics of ResizeProfiler (resize event -> committed frame).
#
# Under weston (the default compositor of wayland_session.sh) the drag goes through xdotool and
# a press in the window starts the resize (QWindow::startSystemResize, like a client-side
# decoration edge). Run in the Arch container, inside the nested KWin session:
#   podman exec -w "$PWD" kk-arch env KK_COMPOSITOR=kwin KK_WESTON_SIZE=2560x1600 \
#       scripts/wayland_session.sh scripts/resize_sweep.sh [dual|single|clip|keyframes]
# KK_NO_OPENGL=1 measures the raster viewers instead of the OpenGL ones. Set BUILD to the build
# directory (default build-arch).
set -euo pipefail

CONFIG="${1:-dual}"
BUILD="${BUILD:-build-arch}"
STEPS="${KK_SWEEP_STEPS:-80}"
LOG="${KK_SWEEP_LOG:-$(mktemp /tmp/kk-resize-sweep-XXXXXX.log)}" # KK_SWEEP_LOG: keep the full log

read -r OW OH <<<"${KK_WESTON_SIZE/x/ }"
KK_RESIZEBENCH_PRESS_RESIZES=$([ "${KK_COMPOSITOR:-weston}" = kwin ] || echo 1) \
    KK_NO_AUDIO=1 KK_RESIZEBENCH_WAIT_MS=$((STEPS * 17 + 4000)) KK_RESIZEBENCH_CONFIG="$CONFIG" \
    QT_LOGGING_RULES="${QT_LOGGING_RULES:-kk.ui.resize.debug=true}" \
    "$BUILD/test_resizebench" interactive >"$LOG" 2>&1 &
APP=$!
for _ in $(seq 300); do
    grep -q "waiting .* for an interactive resize" "$LOG" && break
    sleep 0.1
done
sleep 0.5

if [ "${KK_COMPOSITOR:-weston}" = kwin ]; then
    # KWin places the first window in the centre of the output: Meta + left drag moves it to the
    # top left corner, then Meta + right drag from a point in its bottom-right quarter resizes that
    # corner.
    X=1000
    Y=560
    {
        echo "keydown 125" # left Meta
        echo "move $((OW / 2)) $((OH / 2))"
        echo "sleep 100"
        echo "down"
        for i in $(seq 10); do
            echo "move $((OW / 2 - (OW / 2 - 640) * i / 10)) $((OH / 2 - (OH / 2 - 360) * i / 10))"
            echo "sleep 20"
        done
        echo "up"
        echo "sleep 300"
        echo "move $X $Y"
        echo "sleep 100"
        echo "rdown"
        echo "sleep 100"
        for i in $(seq "$STEPS"); do
            echo "move $((X + i * 8)) $((Y + i * 9 / 2))"
            echo "sleep 16"
        done
        echo "sleep 200"
        echo "rup"
        echo "keyup 125"
    } | env -u WAYLAND_DEBUG "$BUILD/kk_fakeinput"
else
    # weston places windows at random; a 1280x720 window on the 2560x1600 output nearly always
    # covers the output's centre. A press there starts the client's interactive resize of the
    # bottom-right corner (KK_RESIZEBENCH_PRESS_RESIZES), then the pointer moves in 8 px steps.
    X=$((OW / 2))
    Y=$((OH / 2))
    xdotool mousemove "$X" "$Y"
    sleep 0.2
    xdotool mousedown 1
    sleep 0.1
    for i in $(seq "$STEPS"); do
        xdotool mousemove "$((X + i * 8))" "$((Y + i * 9 / 2))"
        sleep 0.016
    done
    sleep 0.2
    xdotool mouseup 1
fi

wait "$APP" || true
grep -E "resize gesture|interactive " "$LOG"
[ -n "${KK_SWEEP_LOG:-}" ] || rm -f "$LOG"
