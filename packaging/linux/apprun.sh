#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# AppRun -- launcher of the Kinoklip AppImage / portable bundle (copied to <AppDir>/AppRun).
#
# Usage: AppRun [kinoklip arguments...]
#        AppRun --install-desktop-entry     add Kinoklip to the application menu (~/.local/share)
#        AppRun --uninstall-desktop-entry   remove it again
#
# What it does:
#   1. Links the bundled copies of "fallback" libraries the host lacks into
#      usr/lib/fallback-active/ (found through RUNPATH; see packaging/linux/excludelist.txt).
#      "newer" fallbacks (libstdc++, libgcc_s) are linked when the host copy is older than
#      the bundled one (compared by their highest GLIBCXX_/GCC_ version tag).
#   2. Chooses the Qt: the bundled Qt (with KDE Frameworks 6 and the Breeze style: identical
#      look everywhere) by default. KINOKLIP_HOST_QT=1 opts in to the host Qt instead (only if
#      it is exactly the Qt release the app was built with -- the app uses Qt private API --,
#      has every needed module from that release, and has a wayland/xcb platform plugin).
#   3. Points XDG_DATA_DIRS at usr/share (KDE colour schemes) and LIBHEIF_PLUGIN_PATH at the
#      bundled libheif decoder plugins.
#   4. Records the original value of every variable it changes (KINOKLIP_ORIG_VARS +
#      KINOKLIP_ORIG_<NAME>; src/platform/ChildProcessEnv restores them for programs the app
#      starts, e.g. xdg-open), then execs usr/bin/kinoklip with all arguments.
#
# Environment it reads:
#   KINOKLIP_HOST_QT=1      try the host Qt (exact version match only; falls back to the
#                           bundled one otherwise)
#   KINOKLIP_BUNDLED_QT=1   force the bundled Qt (wins over KINOKLIP_HOST_QT)
#   KINOKLIP_HOST_LIBDIRS   test hook: colon-separated dirs searched for host libraries
#                           instead of the ld.so cache
#   KINOKLIP_FORCE_FALLBACK=1  test hook: use every bundled fallback library
#   KINOKLIP_QUIET=1        no log line on stderr
#   APPIMAGE                set by the AppImage runtime: the .AppImage file. Then the menu
#                           entry is left to Gear Lever / appimaged (no automatic install;
#                           --install-desktop-entry points it at the AppImage file), and the
#                           fallback links go to a private writable directory (the squashfs
#                           mount is read-only), see shadow_fallback_dir
#   KINOKLIP_RUN_PATH       set by the legacy .run wrapper: the .run file (desktop entries)
#   KINOKLIP_NO_DESKTOP_ENTRY=1  do not install/refresh the menu entry on start (it is
#                           installed automatically otherwise, except in an AppImage;
#                           --uninstall-desktop-entry opts out permanently,
#                           --install-desktop-entry opts back in)
#
#   KINOKLIP_FORCE_GLIBC=1|0  test hook: force the bundled glibc on / off
#
# Never touched: LIBVA_DRIVER_NAME, LIBVA_DRIVERS_PATH (except: set to the host driver dirs
# when the bundled libva is used and the user has not set it), VK_*, __GLX_*, MESA_* -- the host
# GPU drivers stay in charge of VAAPI/OpenGL/Vulkan.

HERE="$(dirname "$(readlink -f "$0")")"
APP_ID=org.kinoklip.Kinoklip
LIB="$HERE/usr/lib"
QTDIR="$LIB/qt"

log() {
    [ "${KINOKLIP_QUIET:-0}" = 1 ] || printf 'kinoklip-launcher: %s\n' "$*" >&2
}

# ---- environment bookkeeping -----------------------------------------------------------
# set_var NAME VALUE: exports NAME=VALUE and remembers the original value once, so the app
# can give child processes the user's environment back (src/platform/ChildProcessEnv.h).
KINOKLIP_ORIG_VARS="${KINOKLIP_ORIG_VARS:-}"
set_var() {
    case " $KINOKLIP_ORIG_VARS " in
    *" $1 "*) ;; # already recorded (e.g. Kinoklip started from Kinoklip)
    *)
        KINOKLIP_ORIG_VARS="${KINOKLIP_ORIG_VARS:+$KINOKLIP_ORIG_VARS }$1"
        if eval "[ -n \"\${$1+x}\" ]"; then
            eval "KINOKLIP_ORIG_$1=\"\${$1}\""
            export "KINOKLIP_ORIG_$1"
        fi
        ;;
    esac
    eval "$1=\"\$2\""
    export "$1"
}
unset_var() {
    eval "[ -n \"\${$1+x}\" ]" || return 0
    set_var "$1" ""
    unset "$1"
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
# newer_tag <soname>: the version tag recorded for a "newer" fallback (usr/lib/fallback/newer.txt)
newer_tag() {
    [ -f "$LIB/fallback/newer.txt" ] || return 0
    while read -r _nn _nt; do
        [ "$_nn" = "$1" ] && printf '%s\n' "$_nt" && return 0
    done <"$LIB/fallback/newer.txt"
    return 0
}
# fallback-active/ must contain exactly the fallback libs the host is missing: RUNPATH is
# searched before the system, so anything linked there overrides the host copy.
setup_fallback() {
    _active="$LIB/fallback-active"
    [ -d "$LIB/fallback" ] || return 0
    _missing=""
    for _f in "$LIB"/fallback/*.so*; do
        [ -e "$_f" ] || continue
        _n="${_f##*/}"
        _host="$(host_lib "$_n")"
        if [ "${KINOKLIP_FORCE_FALLBACK:-0}" = 1 ] || [ -z "$_host" ]; then
            _missing="$_missing $_n"
        elif _tag="$(newer_tag "$_n")" && [ -n "$_tag" ]; then
            # "newer" fallback: use ours if the host's copy is older (by its highest version
            # tag, or for "file:" tags by the version in its real file name)
            case "$_tag" in
            file:*)
                _tag="${_tag#file:}"
                _hosttag="$(readlink -f "$_host")"
                _hosttag="${_hosttag##*.so.}"
                ;;
            *) _hosttag="$(grep -aoE "${_tag%%[0-9]*}[0-9.]+" "$_host" 2>/dev/null | sort -uV | tail -n 1)" ;;
            esac
            if [ "$(printf '%s\n%s\n' "$_hosttag" "$_tag" | sort -V | tail -n 1)" != "$_hosttag" ]; then
                _missing="$_missing $_n"
            fi
        fi
    done
    if [ ! -w "$_active" ]; then
        # Read-only bundle (the AppImage's squashfs mount): the links go into a private
        # writable directory instead, put in front through LD_LIBRARY_PATH (searched before
        # RUNPATH; restored for child processes like every variable set_var changes).
        [ -n "$_missing" ] || return 0
        if _active="$(shadow_fallback_dir "$_missing")"; then
            set_var LD_LIBRARY_PATH "$_active${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
            log "using bundled fallbacks:$_missing (links in $_active)"
        else
            log "warning: $LIB is read-only and no private writable directory was found, cannot use bundled fallbacks for:$_missing"
            return 0
        fi
        fallback_libva_drivers
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
    fallback_libva_drivers
    return 0
}

# A bundled libva searches the build host's driver dir; point it at the host drivers (only
# when the bundled libva is used -- $_missing -- and never over a user setting).
fallback_libva_drivers() {
    case " $_missing " in
    *" libva.so.2 "*)
        if [ -z "${LIBVA_DRIVERS_PATH:-}" ]; then
            _dri=""
            for _d in /usr/lib/x86_64-linux-gnu/dri /usr/lib64/dri /usr/lib/dri /usr/local/lib/dri; do
                [ -d "$_d" ] && _dri="${_dri:+$_dri:}$_d"
            done
            [ -n "$_dri" ] && set_var LIBVA_DRIVERS_PATH "$_dri"
        fi
        ;;
    esac
    return 0
}

# shadow_fallback_dir "<sonames>": prints a private writable directory holding links to the
# given usr/lib/fallback libraries, for a read-only bundle. Layout, so that the fallbacks'
# own RUNPATH ($ORIGIN/..:$ORIGIN:$ORIGIN/../qt; $ORIGIN is the directory of the link) works
# exactly as in the bundle:
#   <dir>/active/<soname> -> <bundle>/usr/lib/fallback/<soname>   (the LD_LIBRARY_PATH entry)
#   <dir>/<lib>.so*       -> <bundle>/usr/lib/<lib>.so*
#   <dir>/qt              -> <bundle>/usr/lib/qt
# Lives in $XDG_RUNTIME_DIR (per user, mode 0700, cleared at logout), else ~/.cache/kinoklip;
# never in a shared /tmp, where another user could plant libraries. The name hashes the
# bundle path and the library list, so a ready directory is reused. An AppImage is mounted
# at a new path on every start: directories whose bundle is gone (dangling qt link) are
# removed first.
shadow_fallback_dir() {
    _base=""
    if [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -d "$XDG_RUNTIME_DIR" ] && [ -w "$XDG_RUNTIME_DIR" ] &&
        [ -O "$XDG_RUNTIME_DIR" ]; then
        _base="$XDG_RUNTIME_DIR"
    elif [ -n "${HOME:-}" ]; then
        _base="${XDG_CACHE_HOME:-$HOME/.cache}/kinoklip"
        mkdir -p "$_base" 2>/dev/null || return 1
    fi
    [ -n "$_base" ] && [ -w "$_base" ] || return 1
    for _d in "$_base"/kinoklip-fallback-*; do
        case "$_d" in *.tmp*) continue ;; esac
        [ -d "$_d" ] && [ ! -e "$_d/qt/" ] && rm -rf "$_d"
    done
    _key="$(printf '%s\n%s\n' "$HERE" "$1" | cksum | cut -d' ' -f1)"
    _sd="$_base/kinoklip-fallback-$_key"
    if [ ! -e "$_sd/.complete" ]; then
        _tmp="$_sd.tmp$$"
        rm -rf "$_tmp"
        mkdir -p "$_tmp/active" || return 1
        ln -s "$LIB"/*.so* "$_tmp/" 2>/dev/null
        ln -s "$QTDIR" "$_tmp/qt" || return 1
        for _n in $1; do
            ln -s "$LIB/fallback/$_n" "$_tmp/active/$_n" || return 1
        done
        : >"$_tmp/.complete"
        rm -rf "$_sd" # an incomplete one (interrupted start)
        mv "$_tmp" "$_sd" 2>/dev/null
        rm -rf "$_tmp" # still there if another start won the race
        [ -e "$_sd/.complete" ] || return 1
    fi
    printf '%s\n' "$_sd/active"
}

# ---- 2. Qt selection -------------------------------------------------------------------
# host_qt_ok: succeeds if the host Qt can run the binary. Sets HOST_QT_REASON otherwise.
host_qt_ok() {
    _core="$(host_lib libQt6Core.so.6)"
    if [ -z "$_core" ]; then
        HOST_QT_REASON="no host libQt6Core.so.6"
        return 1
    fi
    # Version from the real file name: libQt6Core.so.6.12.0 -> 6.12.0
    _real="$(readlink -f "$_core")"
    _ver="${_real##*/libQt6Core.so.}"
    case "$_ver" in
    *[!0-9.]* | .* | *. | '')
        HOST_QT_REASON="cannot tell the host Qt version from $_real"
        return 1
        ;;
    esac
    # Exactly the Qt the app was built with (usr/lib/qt/qt-version.txt, e.g. "6.12.0"): the
    # binary links Qt GuiPrivate (Wayland portal parent window), whose ABI changes with every
    # Qt release, patch releases included. An older bundle wrote only "6.12": then major.minor
    # must match and the patch level cannot be checked, so refuse.
    _need="$(cat "$QTDIR/qt-version.txt" 2>/dev/null)"
    case "$_need" in
    *.*.*) ;;
    *)
        HOST_QT_REASON="the bundle does not record its exact Qt version"
        return 1
        ;;
    esac
    if [ "$_ver" != "$_need" ]; then
        HOST_QT_REASON="host Qt $_ver is not the Qt $_need the app was built with"
        return 1
    fi
    HOST_QT_VERSION="$_ver"
    # every module the binary needs, all from the same Qt release as Core
    while read -r _q; do
        case "$_q" in '' | '#'*) continue ;; esac
        _p="$(host_lib "$_q")"
        if [ -z "$_p" ]; then
            HOST_QT_REASON="host Qt lacks $_q"
            return 1
        fi
        case "$(readlink -f "$_p")" in
        *".so.$_ver") ;;
        *)
            HOST_QT_REASON="$_q is not from Qt $_ver"
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
    set_var LD_LIBRARY_PATH "$QTDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    # Only our plugins (incl. the Breeze style and KDE's icon engine): host plugins are built
    # for another Qt/KF6 and could crash ours.
    set_var QT_PLUGIN_PATH "$QTDIR/plugins"
    # The app brings its own look (Breeze style, KDE colour scheme, Breeze icons) and must
    # look the same on every system: the "generic" theme stops Qt's built-in KDE/GNOME themes
    # from importing host fonts and colours, and a style override is dropped (the user picks
    # the style in Settings > Style).
    set_var QT_QPA_PLATFORMTHEME generic
    unset_var QT_STYLE_OVERRIDE
    # the only multimedia backend we ship (shares the bundled FFmpeg)
    [ -n "${QT_MEDIA_BACKEND:-}" ] || set_var QT_MEDIA_BACKEND ffmpeg
    log "using bundled Qt $(cat "$QTDIR/qt-version.txt" 2>/dev/null)"
}

# ---- desktop integration ---------------------------------------------------------------
data_home() { printf '%s\n' "${XDG_DATA_HOME:-$HOME/.local/share}"; }

refresh_desktop_caches() {
    _dh="$(data_home)"
    command -v update-mime-database >/dev/null 2>&1 && update-mime-database "$_dh/mime" >/dev/null 2>&1
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$_dh/applications" >/dev/null 2>&1
    # KDE reads its own cache; rebuild it in the background if available. Then tell running KDE
    # apps (Plasma's task manager, menus) to drop their cached icons: without the iconChanged
    # signal Plasma kept showing an old icon of ours until the next login.
    if command -v kbuildsycoca6 >/dev/null 2>&1; then
        (
            kbuildsycoca6 >/dev/null 2>&1
            if command -v dbus-send >/dev/null 2>&1; then
                dbus-send --session --type=signal /KIconLoader org.kde.KIconLoader.iconChanged \
                    int32:0 >/dev/null 2>&1
            elif command -v gdbus >/dev/null 2>&1; then
                gdbus emit --session --object-path /KIconLoader \
                    --signal org.kde.KIconLoader.iconChanged 0 >/dev/null 2>&1
            fi
        ) &
    fi
    return 0
}

# write_desktop_file <out>: the bundle's desktop entry with Exec/TryExec pointing at the
# AppImage file ($APPIMAGE, set by the AppImage runtime), the .run or this AppRun, quoted for
# paths with spaces.
write_desktop_file() {
    _target="${APPIMAGE:-${KINOKLIP_RUN_PATH:-$HERE/AppRun}}"
    # Desktop-entry rules: inside "..." the characters " ` $ need "\\" before them and a
    # backslash needs four (string escaping is applied before the quoting rule).
    _q="$(printf '%s' "$_target" | sed -e 's/\\/\\\\\\\\/g' -e 's/["`$]/\\\\&/g')"
    # awk reads the values from ENVIRON (awk -v would interpret the backslashes)
    KK_EXEC="Exec=\"$_q\" %f" KK_TRYEXEC="TryExec=$_target" awk '
        /^Exec=/ { print ENVIRON["KK_EXEC"]; next }
        /^TryExec=/ { print ENVIRON["KK_TRYEXEC"]; next }
        { print }' "$HERE/$APP_ID.desktop" >"$1"
}

install_desktop_entry() {
    _dh="$(data_home)"
    mkdir -p "$_dh/applications" "$_dh/mime/packages" || return 1
    write_desktop_file "$_dh/applications/$APP_ID.desktop" || return 1
    # One PNG per size; the scalable SVG of versions up to 0.3.4 would win over them.
    rm -f "$_dh/icons/hicolor/scalable/apps/$APP_ID.svg"
    for _size in $ICON_SIZES; do
        mkdir -p "$_dh/icons/hicolor/${_size}x${_size}/apps" || return 1
        cp "$HERE/usr/share/icons/hicolor/${_size}x${_size}/apps/$APP_ID.png" \
            "$_dh/icons/hicolor/${_size}x${_size}/apps/$APP_ID.png" || return 1
    done
    touch "$_dh/icons/hicolor" 2>/dev/null
    cp "$HERE/usr/share/mime/packages/$APP_ID.xml" "$_dh/mime/packages/$APP_ID.xml" || return 1
    rm -f "$(optout_file)"
    refresh_desktop_caches
    [ "${1:-}" = quiet ] && return 0
    echo "Installed $_dh/applications/$APP_ID.desktop -> $_target"
    case "$_target" in
    */.cache/* | /tmp/*) echo "Note: this points into a temporary extraction; prefer running --install-desktop-entry from the .AppImage file." ;;
    esac
}

# Sizes of the bundled icon (usr/share/icons/hicolor/<n>x<n>/apps), see scripts/package_linux.sh.
ICON_SIZES="16 24 32 48 64 128 256 512"

# True when the installed icon is not this bundle's (a new icon in a new version).
icon_outdated() {
    ! cmp -s "$HERE/usr/share/icons/hicolor/256x256/apps/$APP_ID.png" \
        "$(data_home)/icons/hicolor/256x256/apps/$APP_ID.png" 2>/dev/null
}

# Written by --uninstall-desktop-entry: the user does not want the menu entry, so the
# automatic install below must not bring it back.
optout_file() { printf '%s\n' "${XDG_CONFIG_HOME:-$HOME/.config}/kinoklip/no-desktop-entry"; }

# Every normal start: make sure ~/.local/share/applications/<app id>.desktop exists and
# points at this copy. Qt registers the app id with the XDG desktop portal at start-up
# ("Failed to register with host portal ... App info not found" otherwise), and Plasma
# uses the entry for the window/task-manager icon on Wayland. Rewritten only when the
# content or the icon differs (first start, the .run moved, a new version), so normal starts
# only read two small files. KINOKLIP_NO_DESKTOP_ENTRY=1 or the opt-out file skips it.
#
# Not for an AppImage ($APPIMAGE set): Gear Lever, appimaged or AppImageLauncher integrate it
# (their own desktop file, Exec= the AppImage), and an entry of ours would be a second menu
# item. --install-desktop-entry still works there for users without such a tool.
ensure_desktop_entry() {
    [ -n "${APPIMAGE:-}" ] && return 0
    [ "${KINOKLIP_NO_DESKTOP_ENTRY:-0}" = 1 ] && return 0
    [ -e "$(optout_file)" ] && return 0
    [ -n "${HOME:-}" ] || return 0
    _want="${TMPDIR:-/tmp}/kinoklip-desktop-$$"
    write_desktop_file "$_want" 2>/dev/null || {
        rm -f "$_want"
        return 0
    }
    _have="$(data_home)/applications/$APP_ID.desktop"
    if ! cmp -s "$_want" "$_have" 2>/dev/null || icon_outdated; then
        if install_desktop_entry quiet >/dev/null 2>&1; then
            log "installed the desktop entry $_have"
        fi
    fi
    rm -f "$_want"
    return 0
}

uninstall_desktop_entry() {
    _dh="$(data_home)"
    rm -f "$_dh/applications/$APP_ID.desktop" "$_dh/icons/hicolor/scalable/apps/$APP_ID.svg" \
        "$_dh/mime/packages/$APP_ID.xml"
    for _size in $ICON_SIZES; do
        rm -f "$_dh/icons/hicolor/${_size}x${_size}/apps/$APP_ID.png"
    done
    mkdir -p "$(dirname "$(optout_file)")" && : >"$(optout_file)"
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
ensure_desktop_entry

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

# KDE colour schemes (usr/share/color-schemes) for KColorSchemeManager; the host's data dirs
# stay searched after ours. Default per the XDG base directory spec if unset.
set_var XDG_DATA_DIRS "$HERE/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
# libheif's decoder plugins (HEIC/AVIF) next to the bundled libheif
if [ -d "$LIB/libheif/plugins" ]; then
    set_var LIBHEIF_PLUGIN_PATH "$LIB/libheif/plugins"
fi

# Wayland first (native on KDE Linux), X11/XWayland as fallback. A user choice wins. Without a
# Wayland compositor (pure X11 session: WAYLAND_DISPLAY unset) go straight to xcb: trying
# wayland first makes the bundled Qt print "Failed to create wl_display" and "Could not load
# the Qt platform plugin wayland" before it falls back.
if [ -z "${QT_QPA_PLATFORM:-}" ]; then
    if [ -n "${WAYLAND_DISPLAY:-}" ]; then
        set_var QT_QPA_PLATFORM "wayland;xcb"
    else
        set_var QT_QPA_PLATFORM xcb
    fi
fi
KINOKLIP_APPDIR="$HERE"
export KINOKLIP_APPDIR KINOKLIP_ORIG_VARS

# ---- glibc -------------------------------------------------------------------------------
# The bundle is built on a current Arch system. If the host glibc is older than what the
# bundled libraries need (usr/lib/glibc/glibc-required.txt), run through the bundled loader
# with the bundled glibc. glibc is backwards compatible, so the host's GPU drivers and other
# host libraries work with it. ld.so's --library-path is not inherited by child processes.
# KINOKLIP_FORCE_GLIBC=1 / =0 forces it on / off (test hooks).
use_bundled_glibc() {
    case "${KINOKLIP_FORCE_GLIBC:-}" in 1) return 0 ;; 0) return 1 ;; esac
    [ -f "$LIB/glibc/glibc-required.txt" ] || return 1
    _need="$(cat "$LIB/glibc/glibc-required.txt")"
    _have="$(getconf GNU_LIBC_VERSION 2>/dev/null)"
    _have="${_have#glibc }"
    if [ -z "$_have" ]; then
        # no getconf: the version is in the loader's --version banner
        _have="$(ldd --version 2>/dev/null | sed -n '1s/.* \([0-9][0-9.]*\)$/\1/p')"
    fi
    [ -n "$_have" ] || return 1
    [ "$(printf '%s\n%s\n' "$_have" "$_need" | sort -V | head -n 1)" != "$_need" ]
}

if use_bundled_glibc; then
    log "using the bundled glibc (host: ${_have:-forced}, bundle needs: ${_need:-forced})"
    exec "$LIB/glibc/ld-linux-x86-64.so.2" --library-path "$LIB/glibc${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        --argv0 "$HERE/usr/bin/kinoklip" "$HERE/usr/bin/kinoklip" "$@"
fi
exec "$HERE/usr/bin/kinoklip" "$@"
