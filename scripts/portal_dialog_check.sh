#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Shows File > Import Media through the XDG desktop portal (KDE backend) on a headless X server
# and records the D-Bus traffic. Run in the Arch container (needs xorg-server-xvfb, dbus,
# xdg-desktop-portal, xdg-desktop-portal-kde, imagemagick):
#
#   podman exec -w "$PWD" kk-arch scripts/portal_dialog_check.sh [kinoklip binary] [out dir]
#
# Default binary: build-arch-release/kinoklip (or dist/kinoklip-x86_64/AppRun to test the
# bundle). Writes <out>/import-dialog.png (the app's own screen grab), <out>/root.png (an
# ImageMagick grab of the whole X screen) and <out>/dbus.log (dbus-monitor of the
# FileChooser traffic). KK_PORTAL_CHOOSE="<path>" types that path into the dialog and accepts
# it (proves the Response -> import path); set KK_SCREENSHOT_DIALOG_DELAY=12000 with it.
set -euo pipefail
cd "$(dirname "$0")/.."
BIN="${1:-build-arch-release/kinoklip}"
OUT="${2:-build-arch-release/portal-check}"
mkdir -p "$OUT"
OUT="$(readlink -f "$OUT")"
BIN="$(readlink -f "$BIN")"

if [ -z "${KK_PORTAL_INNER:-}" ]; then
    export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-$(mktemp -d /tmp/kk-runtime-XXXXXX)}"
    chmod 700 "$XDG_RUNTIME_DIR"
    Xvfb :77 -screen 0 1920x1080x24 -nolisten tcp >"$OUT/xvfb.log" 2>&1 &
    XVFB=$!
    trap 'kill $XVFB 2>/dev/null || true' EXIT
    sleep 1
    rc=0
    KK_PORTAL_INNER=1 DISPLAY=:77 dbus-run-session -- "$0" "$@" </dev/null || rc=$?
    exit $rc
fi

# ---- inside the private session bus ----
export XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=x11 QT_QPA_PLATFORM=xcb
dbus-monitor --session "interface='org.freedesktop.portal.FileChooser'" \
    "interface='org.freedesktop.portal.Request'" \
    "interface='org.freedesktop.impl.portal.FileChooser'" >"$OUT/dbus.log" 2>&1 &
MON=$!
# The portal frontend is D-Bus activated on the first call; start it explicitly to log it.
/usr/lib/xdg-desktop-portal >"$OUT/portal.log" 2>&1 &
PORTAL=$!
sleep 2
rc=0
KINOKLIP_NO_DESKTOP_ENTRY=1 KK_NO_AUDIO=1 KK_SCREENSHOT_DIALOG_DELAY="${KK_SCREENSHOT_DIALOG_DELAY:-8000}" \
    QT_LOGGING_RULES="kinoklip.filedialogs.debug=true" QT_FORCE_STDERR_LOGGING=1 \
    "$BIN" --screenshot import-dialog "$OUT/import-dialog.png" >"$OUT/app.log" 2>&1 &
APP=$!
if [ -n "${KK_PORTAL_CHOOSE:-}" ]; then
    # Answer the KDE dialog like a user: type the file(s) into "Name" and press Enter. The
    # app then imports them, which the screenshot (taken later) shows in the media pool.
    sleep 5
    import -window root "$OUT/dialog.png" 2>/dev/null || true
    xdotool search --sync --name "Import Media" windowactivate --sync 2>/dev/null || true
    xdotool type --delay 20 "$KK_PORTAL_CHOOSE"
    xdotool key Return
fi
sleep "${KK_ROOT_GRAB_AFTER:-9}"
import -window root "$OUT/root.png" 2>/dev/null || true
wait $APP || rc=$?
kill $MON $PORTAL 2>/dev/null || true
pkill -f xdg-desktop-portal-kde 2>/dev/null || true
echo "app exit code: $rc"
echo "--- FileChooser calls:"
grep -A40 "member=OpenFile\|member=SaveFile" "$OUT/dbus.log" | head -80 || true
echo "--- outputs in $OUT"
ls -l "$OUT"
exit $rc
