#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# AppRun -- launcher of the portable Kinoklip bundle (copied to <bundle>/AppRun).
#
# Usage: AppRun [kinoklip arguments...]
#        AppRun --install-desktop-entry     add Kinoklip to the application menu (~/.local/share)
#        AppRun --uninstall-desktop-entry   remove it again
#
# What it does:
#   1. Links the bundled copies of "fallback" libraries the host lacks into
#      usr/lib/fallback-active/ (found through RUNPATH; see packaging/linux/excludelist.txt).
#   2. Chooses the Qt: the bundled Qt 6.4 by default (identical look everywhere; the app
#      brings its own style). KINOKLIP_HOST_QT=1 opts in to the host Qt instead (only if it
#      has every needed module, >= 6.4, and a wayland/xcb platform plugin).
#   3. Execs usr/bin/kinoklip with all arguments.
#
# Environment it reads:
#   KINOKLIP_HOST_QT=1      try the host Qt (falls back to the bundled one if unsuitable)
#   KINOKLIP_BUNDLED_QT=1   force the bundled Qt (wins over KINOKLIP_HOST_QT)
#   KINOKLIP_HOST_LIBDIRS   test hook: colon-separated dirs searched for host libraries
#                           instead of the ld.so cache
#   KINOKLIP_FORCE_FALLBACK=1  test hook: use every bundled fallback library
#   KINOKLIP_QUIET=1        no log line on stderr
#   KINOKLIP_RUN_PATH       set by the .run wrapper: the .run file (used for desktop entries)
#
# Never touched: LIBVA_DRIVERS_PATH, LIBVA_DRIVER_NAME, VK_*, __GLX_*, MESA_* -- the host
# GPU drivers stay in charge of VAAPI/OpenGL/Vulkan.

HERE="$(dirname "$(readlink -f "$0")")"
APP_ID=org.kinoklip.Kinoklip
LIB="$HERE/usr/lib"
QTDIR="$LIB/qt"

log() {
    [ "${KINOKLIP_QUIET:-0}" = 1 ] || printf 'kinoklip-launcher: %s\n' "$*" >&2
}

# ---- host library lookup -------------------------------------------------------------
# host_lib <soname>: prints the host path of a library, or nothing.
# Uses the ld.so cache (what the loader really uses, incl. /etc/ld.so.conf.d dirs), else
# the usual directories. The cache listing is read once and kept in a variable.
HOST_CACHE=""
host_lib() {
    if [ -n "${KINOKLIP_HOST_LIBDIRS:-}" ]; then
        _old_ifs=$IFS
        IFS=:
        for _d in $KINOKLIP_HOST_LIBDIRS; do
            if [ -e "$_d/$1" ]; then
                IFS=$_old_ifs
                printf '%s\n' "$_d/$1"
                return
            fi
        done
        IFS=$_old_ifs
        return
    fi
    if [ -z "$HOST_CACHE" ]; then
        HOST_CACHE="$( (ldconfig -p || /sbin/ldconfig -p || /usr/sbin/ldconfig -p) 2>/dev/null | grep 'x86-64')"
        [ -n "$HOST_CACHE" ] || HOST_CACHE=none
    fi
    if [ "$HOST_CACHE" != none ]; then
        # line format: "<tab>libfoo.so.1 (libc6,x86-64) => /usr/lib/libfoo.so.1"
        _p="$(printf '%s\n' "$HOST_CACHE" | grep -F " $1 (" | head -n 1)"
        _p="${_p#*=> }"
        if [ -n "$_p" ] && [ -e "$_p" ]; then
            printf '%s\n' "$_p"
            return
        fi
    fi
    for _d in /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib64 /lib64 /usr/lib /lib; do
        if [ -e "$_d/$1" ]; then
            printf '%s\n' "$_d/$1"
            return
        fi
    done
}

# ---- 1. fallback libraries -------------------------------------------------------------
# fallback-active/ must contain exactly the fallback libs the host is missing: RUNPATH is
# searched before the system, so anything linked there overrides the host copy.
setup_fallback() {
    _active="$LIB/fallback-active"
    [ -d "$LIB/fallback" ] || return 0
    _missing=""
    for _f in "$LIB"/fallback/*.so*; do
        [ -e "$_f" ] || continue
        _n="${_f##*/}"
        if [ "${KINOKLIP_FORCE_FALLBACK:-0}" = 1 ] || [ -z "$(host_lib "$_n")" ]; then
            _missing="$_missing $_n"
        fi
    done
    if [ ! -w "$_active" ]; then
        [ -n "$_missing" ] && log "warning: $LIB is read-only, cannot use bundled fallbacks for:$_missing"
        return 0
    fi
    # drop links that are no longer wanted (host gained the library), add the missing ones
    for _l in "$_active"/*; do
        [ -L "$_l" ] || continue
        case " $_missing " in *" ${_l##*/} "*) ;; *) rm -f "$_l" ;; esac
    done
    for _n in $_missing; do
        [ -L "$_active/$_n" ] || ln -s "../fallback/$_n" "$_active/$_n"
    done
    [ -n "$_missing" ] && log "using bundled fallbacks:$_missing"
    return 0
}

# ---- 2. Qt selection -------------------------------------------------------------------
# host_qt_ok: succeeds if the host Qt can run the binary. Sets HOST_QT_REASON otherwise.
host_qt_ok() {
    _core="$(host_lib libQt6Core.so.6)"
    if [ -z "$_core" ]; then
        HOST_QT_REASON="no host libQt6Core.so.6"
        return 1
    fi
    # Version from the real file name: libQt6Core.so.6.10.1 -> 6 10
    _real="$(readlink -f "$_core")"
    _ver="${_real##*/libQt6Core.so.}"
    _maj="${_ver%%.*}"
    _min="${_ver#*.}"
    _min="${_min%%.*}"
    case "$_maj.$_min" in
    *[!0-9.]* | .* | *.)
        HOST_QT_REASON="cannot tell the host Qt version from $_real"
        return 1
        ;;
    esac
    if [ "$_maj" -ne 6 ] || [ "$_min" -lt 4 ]; then
        HOST_QT_REASON="host Qt $_maj.$_min is older than 6.4"
        return 1
    fi
    HOST_QT_VERSION="$_maj.$_min"
    # every module the binary needs, all from the same Qt release as Core
    while read -r _q; do
        case "$_q" in '' | '#'*) continue ;; esac
        _p="$(host_lib "$_q")"
        if [ -z "$_p" ]; then
            HOST_QT_REASON="host Qt lacks $_q"
            return 1
        fi
        case "$(readlink -f "$_p")" in
        *".so.$_maj.$_min."*) ;;
        *)
            HOST_QT_REASON="$_q is not from Qt $_maj.$_min"
            return 1
            ;;
        esac
    done <"$QTDIR/host-qt-requirements.txt"
    # a platform plugin next to the host Qt (Arch: /usr/lib/qt6/plugins)
    _qtlib="$(dirname "$_real")"
    for _pd in "$_qtlib/qt6/plugins" /usr/lib/qt6/plugins /usr/lib64/qt6/plugins /usr/lib/x86_64-linux-gnu/qt6/plugins; do
        for _pp in "$_pd"/platforms/libqwayland*.so "$_pd"/platforms/libqxcb.so; do
            [ -e "$_pp" ] && return 0
        done
    done
    HOST_QT_REASON="no host wayland/xcb platform plugin"
    return 1
}

use_bundled_qt() {
    # LD_LIBRARY_PATH (not RUNPATH) selects the bundled Qt, so host-Qt mode stays possible.
    LD_LIBRARY_PATH="$QTDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    # Only our plugins: host plugins are built for a newer Qt and would crash Qt 6.4.
    QT_PLUGIN_PATH="$QTDIR/plugins"
    # The app brings its own style and palette and must look the same on every system:
    # the "generic" theme stops Qt's built-in KDE/GNOME themes from importing host fonts
    # and colours, and a style override (e.g. breeze, absent anyway) is dropped.
    QT_QPA_PLATFORMTHEME=generic
    unset QT_STYLE_OVERRIDE
    # the only multimedia backend we ship (shares the bundled FFmpeg)
    QT_MEDIA_BACKEND="${QT_MEDIA_BACKEND:-ffmpeg}"
    export LD_LIBRARY_PATH QT_PLUGIN_PATH QT_QPA_PLATFORMTHEME QT_MEDIA_BACKEND
    log "using bundled Qt 6.4"
}

# ---- desktop integration ---------------------------------------------------------------
data_home() { printf '%s\n' "${XDG_DATA_HOME:-$HOME/.local/share}"; }

refresh_desktop_caches() {
    _dh="$(data_home)"
    command -v update-mime-database >/dev/null 2>&1 && update-mime-database "$_dh/mime" >/dev/null 2>&1
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$_dh/applications" >/dev/null 2>&1
    # KDE reads its own cache; rebuild it in the background if available
    command -v kbuildsycoca6 >/dev/null 2>&1 && (kbuildsycoca6 >/dev/null 2>&1 &)
    return 0
}

install_desktop_entry() {
    _dh="$(data_home)"
    _target="${KINOKLIP_RUN_PATH:-$HERE/AppRun}"
    mkdir -p "$_dh/applications" "$_dh/icons/hicolor/scalable/apps" "$_dh/mime/packages" || return 1
    # Exec/TryExec point at the .run (or this AppRun), quoted for paths with spaces.
    # Desktop-entry rules: inside "..." the characters " ` $ need "\\" before them and a
    # backslash needs four (string escaping is applied before the quoting rule).
    _q="$(printf '%s' "$_target" | sed -e 's/\\/\\\\\\\\/g' -e 's/["`$]/\\\\&/g')"
    # awk reads the values from ENVIRON (awk -v would interpret the backslashes)
    KK_EXEC="Exec=\"$_q\" %f" KK_TRYEXEC="TryExec=$_target" awk '
        /^Exec=/ { print ENVIRON["KK_EXEC"]; next }
        /^TryExec=/ { print ENVIRON["KK_TRYEXEC"]; next }
        { print }' "$HERE/$APP_ID.desktop" >"$_dh/applications/$APP_ID.desktop" || return 1
    cp "$HERE/$APP_ID.svg" "$_dh/icons/hicolor/scalable/apps/$APP_ID.svg" || return 1
    cp "$HERE/usr/share/mime/packages/$APP_ID.xml" "$_dh/mime/packages/$APP_ID.xml" || return 1
    refresh_desktop_caches
    echo "Installed $_dh/applications/$APP_ID.desktop -> $_target"
    case "$_target" in
    */.cache/*) echo "Note: this points into the extraction cache; prefer running --install-desktop-entry from the .run file." ;;
    esac
}

uninstall_desktop_entry() {
    _dh="$(data_home)"
    rm -f "$_dh/applications/$APP_ID.desktop" "$_dh/icons/hicolor/scalable/apps/$APP_ID.svg" \
        "$_dh/mime/packages/$APP_ID.xml"
    refresh_desktop_caches
    echo "Removed the Kinoklip desktop entry from $_dh"
}

case "${1:-}" in
--install-desktop-entry)
    install_desktop_entry
    exit $?
    ;;
--uninstall-desktop-entry)
    uninstall_desktop_entry
    exit $?
    ;;
esac

# ---- run -------------------------------------------------------------------------------
setup_fallback

if [ "${KINOKLIP_BUNDLED_QT:-0}" = 1 ]; then
    use_bundled_qt
elif [ "${KINOKLIP_HOST_QT:-0}" = 1 ]; then
    if host_qt_ok; then
        # Nothing to set: the binary's RUNPATH has no Qt dir, so ld.so finds the system Qt,
        # and the system Qt finds its own plugins (incl. the KDE platform theme).
        log "using host Qt $HOST_QT_VERSION (KINOKLIP_HOST_QT=1)"
    else
        log "host Qt unsuitable ($HOST_QT_REASON)"
        use_bundled_qt
    fi
else
    use_bundled_qt
fi

# Wayland first (native on KDE Linux), X11/XWayland as fallback. A user choice wins.
QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-wayland;xcb}"
export QT_QPA_PLATFORM
KINOKLIP_APPDIR="$HERE"
export KINOKLIP_APPDIR

exec "$HERE/usr/bin/kinoklip" "$@"
