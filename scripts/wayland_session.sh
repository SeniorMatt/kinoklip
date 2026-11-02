#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs a command inside a nested Wayland session that can be driven like a real desktop:
# Xvfb (virtual X screen) -> weston --backend=x11 (a Wayland compositor shown as one X window)
# -> the command with QT_QPA_PLATFORM=wayland. xdotool events sent to the X screen reach weston
# as real pointer / keyboard input, so Wayland drags (wl_data_device.start_drag with the press
# serial), xdg-toplevel moves and pointer grabs behave as on a Wayland desktop. weston's
# headless backend has no input devices, which is why it is not used here.
#
# KK_COMPOSITOR=kwin uses KWin (Plasma's compositor) instead, with its virtual backend: its
# X11 / Wayland windowed backends need a GPU, which the container lacks. The virtual output has
# no input devices and is not visible on the X screen: drive it with build-arch/kk_fakeinput
# (KWin's fake-input protocol, enabled by KWIN_WAYLAND_NO_PERMISSION_CHECKS=1) and read the
# app's logs (QT_LOGGING_RULES=kk.ui.dnd.debug=true). KWin composites with QPainter there and
# has no effects, so its screenshot API is unavailable too. KK_WAYLAND_SCALE=1.5 sets the
# output scale with kscreen-doctor (fractional scaling like a laptop panel). KWin's binary
# carries file capabilities that a rootless container refuses to exec, so a copy without them
# is used (named kwin_wayland: KWin's QPA plugin checks the executable name). KWin needs a
# session bus: the command runs under dbus-run-session.
#
# Run in the Arch container (weston, xorg-server-xvfb, xdotool, imagemagick):
#   podman exec -w "$PWD" kk-arch scripts/wayland_session.sh <command> [args...]
# Inside the command: $WAYLAND_DISPLAY, $DISPLAY (the X screen, for xdotool / import) and
# $KK_WESTON_SIZE are set. Exits with the command's status. The weston log goes to
# $KK_WESTON_LOG (default: /tmp/kk-weston.log).
set -euo pipefail

SIZE="${KK_WESTON_SIZE:-1600x1000}"
XDISPLAY="${KK_XDISPLAY:-:87}"
SOCKET="kk-wl-$$"
LOG="${KK_WESTON_LOG:-/tmp/kk-weston.log}"

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-$(mktemp -d /tmp/kk-runtime-XXXXXX)}"
chmod 700 "$XDG_RUNTIME_DIR"

Xvfb "$XDISPLAY" -screen 0 "${SIZE}x24" -nolisten tcp >/dev/null 2>&1 &
XVFB=$!
cleanup() {
    kill "${WESTON:-}" 2>/dev/null || true
    kill "$XVFB" 2>/dev/null || true
}
trap cleanup EXIT
for _ in $(seq 50); do
    [ -e "/tmp/.X11-unix/X${XDISPLAY#:}" ] && break
    sleep 0.1
done

W="${SIZE%x*}"
H="${SIZE#*x}"
if [ "${KK_COMPOSITOR:-weston}" = kwin ]; then
    if [ -z "${KK_WAYLAND_SESSION_BUS:-}" ]; then
        # restart this script inside a private session bus (KWin and the app share it)
        kill "$XVFB" 2>/dev/null || true
        trap - EXIT
        export KK_WAYLAND_SESSION_BUS=1
        exec dbus-run-session -- "$0" "$@"
    fi
    KWIN=/usr/local/kwin/kwin_wayland
    if [ ! -x "$KWIN" ]; then
        mkdir -p /usr/local/kwin
        cp "$(command -v kwin_wayland)" "$KWIN" # the copy has no file capabilities
    fi
    # Own config dir: KWin saves output settings (scale) in kwinoutputconfig.json.
    XDG_CONFIG_HOME="$(mktemp -d /tmp/kk-kwin-config-XXXXXX)" \
        KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 \
        KWIN_COMPOSE=Q "$KWIN" --virtual --width "$W" \
        --height "$H" --socket "$SOCKET" --no-lockscreen >"$LOG" 2>&1 &
else
    DISPLAY="$XDISPLAY" weston --backend=x11 --width="$W" --height="$H" --socket="$SOCKET" \
        --idle-time=0 >"$LOG" 2>&1 &
fi
WESTON=$!
for _ in $(seq 100); do
    [ -e "$XDG_RUNTIME_DIR/$SOCKET" ] && break
    sleep 0.1
done
sleep 1 # desktop-shell client start-up
if [ "${KK_COMPOSITOR:-weston}" = kwin ] && [ -n "${KK_WAYLAND_SCALE:-}" ]; then
    # A real output scale (wl_output / wp_fractional_scale), like Display Configuration does;
    # KWin's --scale option only multiplies the virtual output's size.
    WAYLAND_DISPLAY="$SOCKET" QT_QPA_PLATFORM=wayland \
        kscreen-doctor "output.Virtual-0.scale.$KK_WAYLAND_SCALE" >>"$LOG" 2>&1 || true
    sleep 1
fi

export WAYLAND_DISPLAY="$SOCKET" DISPLAY="$XDISPLAY" KK_WESTON_SIZE="$SIZE"
export QT_QPA_PLATFORM=wayland
if [ "${KK_COMPOSITOR:-weston}" = kwin ]; then
    # KWin's virtual backend composites with QPainter and has no effects; its xdg-activation
    # handler then crashes (EffectsHandler::startupAdded on a null effects handler) when a
    # client asks for an activation token. Not a Plasma desktop issue: hide the interface.
    export QT_WAYLAND_DISABLED_INTERFACES=xdg_activation_v1
fi
rc=0
"$@" || rc=$?
exit $rc
