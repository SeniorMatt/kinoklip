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
#      starts, e.g. xdg-open).
#   5. Asks the dynamic loader, without running anything, whether the app can load (see
#      "preflight" below), then execs usr/bin/kinoklip with all arguments. Only if the loader
#      reports a problem does it stay around as the app's parent, so that a failed start ends
#      in a visible message instead of nothing (nothing is sent anywhere, see docs/privacy.md).
#
# Environment it reads:
#   KINOKLIP_HOST_QT=1      try the host Qt (exact version match only; falls back to the
#                           bundled one otherwise)
#   KINOKLIP_BUNDLED_QT=1   force the bundled Qt (wins over KINOKLIP_HOST_QT)
#   KINOKLIP_HOST_LIBDIRS   test hook: colon-separated dirs searched for host libraries
#                           instead of the ld.so cache
#   KINOKLIP_FORCE_FALLBACK=1  test hook: use every bundled fallback library
#   KINOKLIP_QUIET=1        no log line on stderr
#   KINOKLIP_NO_PREFLIGHT=1 skip the loader check and exec the app directly
#   KINOKLIP_NO_FAILURE_DIALOG=1  a failed start only prints to stderr (test hook)
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

# ---- glibc decision ----------------------------------------------------------------------
# The bundle is built on a current Arch system. If the host glibc is older than what the
# bundled libraries need (usr/lib/glibc/glibc-required.txt), the app runs through the bundled
# loader with the bundled glibc. glibc is backwards compatible, so the host's GPU drivers and
# other host libraries work with it. ld.so's --library-path is not inherited by child
# processes. KINOKLIP_FORCE_GLIBC=1 / =0 forces it on / off (test hooks). Decided first, since
# it also decides where the loader looks for host libraries.
use_bundled_glibc() {
    case "${KINOKLIP_FORCE_GLIBC:-}" in 1) return 0 ;; 0) return 1 ;; esac
    [ -f "$LIB/glibc/glibc-required.txt" ] || return 1
    _glibc_need="$(cat "$LIB/glibc/glibc-required.txt")"
    _glibc_have="$(getconf GNU_LIBC_VERSION 2>/dev/null)"
    _glibc_have="${_glibc_have#glibc }"
    if [ -z "$_glibc_have" ]; then
        # no getconf: the version is in the loader's --version banner
        _glibc_have="$(ldd --version 2>/dev/null | sed -n '1s/.* \([0-9][0-9.]*\)$/\1/p')"
    fi
    [ -n "$_glibc_have" ] || return 1
    [ "$(printf '%s\n%s\n' "$_glibc_have" "$_glibc_need" | sort -V | head -n 1)" != "$_glibc_need" ]
}
if use_bundled_glibc; then BUNDLED_GLIBC=1; else BUNDLED_GLIBC=0; fi

# ---- host library lookup -------------------------------------------------------------
# load_host_cache <soname>...: remembers where the ld.so cache puts these libraries, one shell
# variable per library (HOST_CACHE_<key>). The launcher checks about a hundred libraries on every
# start, each in a fixed small number of shell steps: extracting them from one long string would
# cost 15 ms apiece. Line format: "<tab>libfoo.so.1 (libc6,x86-64) => /usr/lib/libfoo.so.1".
# The first line of a library wins, as in the loader.
cache_key() { # <soname>: sets KEY to the soname with all but [A-Za-z0-9] replaced by "_"
    KEY=""
    _r="$1"
    while :; do
        case "$_r" in
        *[!A-Za-z0-9]*)
            _h="${_r%%[!A-Za-z0-9]*}"
            KEY="${KEY}${_h}_"
            _r="${_r#"$_h"?}"
            ;;
        *)
            KEY="$KEY$_r"
            return 0
            ;;
        esac
    done
}
load_host_cache() {
    _pats=""
    for _n in "$@"; do
        _pats="${_pats:+$_pats
}	$_n ("
    done
    [ -n "$_pats" ] || return 0
    while read -r _n _rest; do
        [ -n "$_n" ] || continue
        cache_key "$_n"
        eval "[ -n \"\${HOST_CACHE_$KEY+x}\" ]" || eval "HOST_CACHE_$KEY=\"\${_rest##*=> }\""
    done <<EOF
$( (ldconfig -p || /sbin/ldconfig -p || /usr/sbin/ldconfig -p) 2>/dev/null | grep -F "$_pats" | grep -F 'x86-64')
EOF
}

# The directories the loader that will run the app searches after the cache. Not simply "all
# the usual ones": a library is only a usable host copy if this loader will really find it.
#   bundled glibc: Arch's ld.so knows only /usr/lib, so Fedora's /usr/lib64 and Debian's
#                  multiarch directories are reachable through /etc/ld.so.cache alone
#   native glibc:  Debian multiarch, or Fedora/openSUSE lib64, or Arch's plain /usr/lib
if [ "$BUNDLED_GLIBC" = 1 ]; then
    HOST_DEFAULT_DIRS="/usr/lib"
elif [ -d /usr/lib/x86_64-linux-gnu ]; then
    HOST_DEFAULT_DIRS="/lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu /lib /usr/lib"
elif [ -d /usr/lib64 ] && [ ! -L /usr/lib64 ]; then
    HOST_DEFAULT_DIRS="/lib64 /usr/lib64"
else
    HOST_DEFAULT_DIRS="/usr/lib /lib"
fi

# is_elf64 <file>: on Fedora /usr/lib holds 32-bit libraries (Steam pulls in many). The loader
# skips them, so one of those must not count as "the host has it".
is_elf64() {
    [ "$(od -An -tu1 -j4 -N1 "$1" 2>/dev/null | tr -d ' ')" = 2 ]
}

# host_lib <soname>: sets HOST_LIB to the host path of a library the loader will find, or to
# nothing. Runs without a subshell. The cache is what the loader really uses first (incl.
# /etc/ld.so.conf.d directories); an entry whose file is gone falls through to the directories,
# just as in the loader. Call load_host_cache for the name first.
host_lib() {
    HOST_LIB=""
    if [ -n "${KINOKLIP_HOST_LIBDIRS:-}" ]; then
        _old_ifs=$IFS
        IFS=:
        for _d in $KINOKLIP_HOST_LIBDIRS; do
            if [ -e "$_d/$1" ]; then
                HOST_LIB="$_d/$1"
                break
            fi
        done
        IFS=$_old_ifs
        return 0
    fi
    cache_key "$1"
    eval "_p=\"\${HOST_CACHE_$KEY:-}\""
    if [ -n "$_p" ] && [ -e "$_p" ]; then
        HOST_LIB="$_p"
        return 0
    fi
    for _d in $HOST_DEFAULT_DIRS; do
        if [ -e "$_d/$1" ] && is_elf64 "$_d/$1"; then
            HOST_LIB="$_d/$1"
            return 0
        fi
    done
    return 0
}

# ---- 1. fallback libraries -------------------------------------------------------------
# fallback-active/ must contain exactly the fallback libs the host is missing: RUNPATH is
# searched before the system, so anything linked there overrides the host copy.
setup_fallback() {
    _active="$LIB/fallback-active"
    [ -d "$LIB/fallback" ] || return 0
    # version tags of the "newer" fallbacks, "<soname> <tag>" per line
    _newer=""
    [ -f "$LIB/fallback/newer.txt" ] && _newer="
$(cat "$LIB/fallback/newer.txt")"
    set --
    for _f in "$LIB"/fallback/*.so*; do
        [ -e "$_f" ] && set -- "$@" "${_f##*/}"
    done
    [ -n "${KINOKLIP_HOST_LIBDIRS:-}" ] || load_host_cache "$@"
    _missing=""
    for _n in "$@"; do
        host_lib "$_n"
        case "$_n" in
        libpulsecommon-*)
            # libpulse's private part, in a subdirectory no cache lists: it goes with libpulse
            host_lib libpulse.so.0
            [ -n "$HOST_LIB" ] && continue
            ;;
        esac
        if [ "${KINOKLIP_FORCE_FALLBACK:-0}" = 1 ] || [ -z "$HOST_LIB" ]; then
            _missing="$_missing $_n"
            continue
        fi
        case "$_newer" in
        *"
$_n "*)
            # "newer" fallback: use ours if the host's copy is older (by its highest version
            # tag, or for "file:" tags by the version in its real file name)
            _tag="${_newer#*"
$_n "}"
            _tag="${_tag%%"
"*}"
            case "$_tag" in
            file:*)
                _tag="${_tag#file:}"
                _hosttag="$(readlink -f "$HOST_LIB")"
                _hosttag="${_hosttag##*.so.}"
                ;;
            *) _hosttag="$(grep -aoE "${_tag%%[0-9]*}[0-9.]+" "$HOST_LIB" 2>/dev/null | sort -uV | tail -n 1)" ;;
            esac
            if [ "$(printf '%s\n%s\n' "$_hosttag" "$_tag" | sort -V | tail -n 1)" != "$_hosttag" ]; then
                _missing="$_missing $_n"
            fi
            ;;
        esac
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
            # Nowhere to put the links. All bundled fallbacks win over the host's then, which is
            # less tidy but starts; without them a host lacking one cannot start at all.
            _active=""
            set_var LD_LIBRARY_PATH "$LIB/fallback${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
            log "warning: no private writable directory found, using all bundled fallbacks"
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
# Lives in $XDG_RUNTIME_DIR (per user, mode 0700, cleared at logout), else ~/.cache/kinoklip,
# else a new mktemp directory (mode 0700 under an unguessable name, so another user cannot plant
# libraries; only for a read-only home without a runtime dir). Only symbolic links live here,
# so a noexec mount does not matter: the loader maps the files they point to, on the bundle's
# mount. The name hashes the bundle path and the library list, so a ready directory is reused.
# An AppImage is mounted at a new path on every start: directories whose bundle is gone
# (dangling qt link) are removed first.
shadow_fallback_dir() {
    _base=""
    if [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -d "$XDG_RUNTIME_DIR" ] && [ -w "$XDG_RUNTIME_DIR" ] &&
        [ -O "$XDG_RUNTIME_DIR" ]; then
        _base="$XDG_RUNTIME_DIR"
    elif [ -n "${HOME:-}" ]; then
        _base="${XDG_CACHE_HOME:-$HOME/.cache}/kinoklip"
        mkdir -p "$_base" 2>/dev/null || _base=""
    fi
    if [ -z "$_base" ] || [ ! -w "$_base" ]; then
        _base="$(mktemp -d "${TMPDIR:-/tmp}/kinoklip-XXXXXX" 2>/dev/null)" || return 1
    fi
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
        # one ln for all of them: a host without a desktop can need a hundred
        (
            _names="$1"
            cd "$_tmp/active" || exit 1
            set --
            for _n in $_names; do set -- "$@" "$LIB/fallback/$_n"; done
            ln -s "$@" .
        ) || return 1
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
    # shellcheck disable=SC2046  # one soname per line
    load_host_cache libQt6Core.so.6 $(grep -v '^#' "$QTDIR/host-qt-requirements.txt")
    host_lib libQt6Core.so.6
    _core="$HOST_LIB"
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
        host_lib "$_q"
        if [ -z "$HOST_LIB" ]; then
            HOST_QT_REASON="host Qt lacks $_q"
            return 1
        fi
        case "$(readlink -f "$HOST_LIB")" in
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
    # Qt 6.12's PipeWire audio backend crashes (SIGSEGV in QPlatformAudioDevices::create, at the
    # first look at the audio devices) when libpipewire is installed but its client
    # configuration is not: Ubuntu 24.04 / Debian 13 with a KDE library set and no "pipewire"
    # package. PulseAudio's API (also served by PipeWire) works there.
    if [ -z "${QT_AUDIO_BACKEND:-}" ] && ! pipewire_client_conf_exists; then
        set_var QT_AUDIO_BACKEND pulseaudio
    fi
    log "using bundled Qt $(cat "$QTDIR/qt-version.txt" 2>/dev/null)"
}

# pipewire_client_conf_exists: succeeds if libpipewire can find its client configuration (the
# first of these files it looks at, as pw_conf_load_conf does).
pipewire_client_conf_exists() {
    for _f in "${XDG_CONFIG_HOME:-$HOME/.config}/pipewire/client.conf" /etc/pipewire/client.conf \
        /usr/share/pipewire/client.conf; do
        [ -f "$_f" ] && return 0
    done
    return 1
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

# ---- start ------------------------------------------------------------------------------
BIN="$HERE/usr/bin/kinoklip"
if [ "$BUNDLED_GLIBC" = 1 ]; then
    log "using the bundled glibc (host: ${_glibc_have:-forced}, bundle needs: ${_glibc_need:-forced})"
    LDSO="$LIB/glibc/ld-linux-x86-64.so.2"
    LDSO_PATH="$LIB/glibc${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
else
    LDSO=/lib64/ld-linux-x86-64.so.2 # the x86-64 ABI's fixed path, on every distribution
fi

# start_app <args...>: becomes the app; called in a background subshell (the supervised path
# below), whose process id is then the app's, so signals sent to that id reach it.
start_app() {
    if [ "$BUNDLED_GLIBC" = 1 ]; then
        exec "$LDSO" --library-path "$LDSO_PATH" --argv0 "$BIN" "$BIN" "$@"
    fi
    exec "$BIN" "$@"
}

# ---- preflight: a failed start must never be silent -----------------------------------------
# The app is exec'ed, so that it IS this process (signals, exit code, process id, no shell left
# waiting), which means nothing can report a loader failure after the exec. So the loader is
# asked first. Its trace mode (what ldd uses) resolves every library and checks symbol versions
# without running any code of the app:
#   - every start: the app binary, about 15 ms;
#   - additionally, once per bundle + host library state (the size and time of ld.so.cache change
#     with every package update) and remembered in ~/.cache/kinoklip: also every symbol, and
#     the Qt platform plugins, which are only loaded at run time. That costs a few hundred ms,
#     and the libraries are bound eagerly on Arch, so an undefined symbol is a real start failure.
# A problem is first repaired where possible (preflight_repair). What is left never stops the app:
# lazily bound symbols may never be used, so the app is started anyway, but as a child, to see
# whether it fails. Everything stays on this computer
# (docs/privacy.md); the optional dialog only shows text.
preflight_trace() { # <deep: "" or 1> <file>: prints only the lines that report a problem
    {
        # An empty LD_WARN / LD_BIND_NOW counts as unset. LD_WARN alone adds 60 ms: it relocates.
        if [ "$BUNDLED_GLIBC" = 1 ]; then
            LD_TRACE_LOADED_OBJECTS=1 LD_WARN="$1" LD_BIND_NOW="$1" "$LDSO" --library-path "$LDSO_PATH" "$2" 2>&1
        else
            LD_TRACE_LOADED_OBJECTS=1 LD_WARN="$1" LD_BIND_NOW="$1" "$LDSO" "$2" 2>&1
        fi || echo "loader exit status $?"
    } | grep -E 'not found|undefined symbol|error while loading|cannot |wrong ELF|Permission denied|No such file|loader exit status'
}
# What the deep check last passed for, on one line.
preflight_state_key() {
    # shellcheck disable=SC2046  # flattening stat's two lines is intended
    echo $(stat -L -c '%Y:%s' "$BIN" /etc/ld.so.cache 2>/dev/null) "glibc=$BUNDLED_GLIBC" \
        "fallbacks=$_missing" "ldpath=${KINOKLIP_ORIG_LD_LIBRARY_PATH:-}"
}
preflight_state_file() {
    [ -n "${HOME:-}" ] && printf '%s\n' "${XDG_CACHE_HOME:-$HOME/.cache}/kinoklip/loader-check"
}
remember_preflight_ok() { # second line: "all" if only all the fallbacks together passed
    _state="$(preflight_state_file)" || return 0
    if mkdir -p "${_state%/*}" 2>/dev/null; then
        {
            preflight_state_key
            [ "${FORCED_ALL:-0}" = 0 ] || echo all
        } >"$_state" 2>/dev/null
    fi
    return 0
}

# Bundled libraries are all from one Arch build, so one the host lacks can need a newer library than
# the host's copy of another one: on Ubuntu 24.04 without a desktop, the bundled libgio wants a libmount
# with MOUNT_2_40, and the host has an older libmount. The host's copy still wins by default, and is
# replaced only where the loader shows it does not do, so that the host's graphics and font stack
# keeps its libraries everywhere else. No shell variables change: the links go where the other
# active fallbacks are, already on the search path.
activate_fallback() { # <soname>
    [ -n "$_active" ] && [ -e "$LIB/fallback/$1" ] && [ ! -e "$_active/$1" ] || return 1
    if [ "$_active" = "$LIB/fallback-active" ]; then
        ln -s "../fallback/$1" "$_active/$1"
    else
        ln -s "$LIB/fallback/$1" "$_active/$1"
    fi 2>/dev/null
}
# Last step: every bundled fallback at once. They were built together, so this set fits together.
force_all_fallbacks() {
    [ "${FORCED_ALL:-0}" = 0 ] || return 1
    FORCED_ALL=1
    log "the host's libraries do not fit the bundled ones; using all bundled fallbacks"
    for _f in "$LIB"/fallback/*.so*; do
        activate_fallback "${_f##*/}"
    done
    return 0
}
# Sets PREFLIGHT_OUT for a file, repairing what can be repaired. Succeeds if nothing is left.
preflight_repair() { # <deep: "" or 1> <file>
    _round=0
    while :; do
        PREFLIGHT_OUT="$(preflight_trace "$1" "$2")"
        [ -n "$PREFLIGHT_OUT" ] || return 0
        _round=$((_round + 1))
        [ "$_round" -le 8 ] || break
        # "<x>: /usr/lib/libmount.so.1: version `MOUNT_2_40' not found (required by ...)"
        _fixed=0
        for _n in $(printf '%s\n' "$PREFLIGHT_OUT" | sed -n 's/^.*\/\(lib[^ \/:]*\): version .* not found.*$/\1/p' | sort -u); do
            if activate_fallback "$_n"; then
                log "the host's $_n is too old for a bundled library; using the bundled one"
                _fixed=1
            fi
        done
        [ "$_fixed" = 1 ] && continue
        force_all_fallbacks && continue
        break
    done
    return 1
}
# Succeeds if the app can be exec'ed; otherwise PREFLIGHT_OUT holds what the loader said.
preflight() {
    _old=""
    _flag=""
    if _state="$(preflight_state_file)" && [ -r "$_state" ]; then
        { read -r _old; read -r _flag; } <"$_state"
    fi
    _key="$(preflight_state_key)"
    [ "$_old" = "$_key" ] && [ "$_flag" = all ] && force_all_fallbacks
    preflight_repair "" "$BIN" || return 1
    [ "$_old" = "$_key" ] && return 0
    for _pl in "$BIN" "$QTDIR"/plugins/platforms/libqwayland*.so "$QTDIR"/plugins/platforms/libqxcb.so; do
        [ -e "$_pl" ] || continue
        preflight_repair 1 "$_pl" || return 1
    done
    remember_preflight_ok
    return 0
}

ISSUES_URL=https://github.com/SeniorMatt/kinoklip/issues

# show_failure <details>: makes a failed start visible: always on stderr and in a log file,
# and in a dialog or notification with whatever this desktop has. The tools run with the user's
# original environment, not with the bundled libraries the launcher selected.
show_failure() {
    _msg="Kinoklip could not start.

$1

The bundled libraries could not make up for this on your system. Please report it, with this
text and your Linux distribution, at $ISSUES_URL"
    printf 'kinoklip-launcher: %s\n' "$_msg" >&2
    _state="$(preflight_state_file)" && mkdir -p "${_state%/*}" 2>/dev/null &&
        printf '%s\n' "$_msg" >"${_state%/*}/start-failure.log" 2>/dev/null
    [ "${KINOKLIP_NO_FAILURE_DIALOG:-0}" = 1 ] && return 0
    [ -t 2 ] && return 0 # started from a terminal: the message is on the screen already
    [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] || return 0
    (
        for _v in $KINOKLIP_ORIG_VARS; do
            if eval "[ -n \"\${KINOKLIP_ORIG_$_v+x}\" ]"; then
                eval "$_v=\"\${KINOKLIP_ORIG_$_v}\""
                export "${_v?}"
            else
                unset "$_v"
            fi
        done
        case "${XDG_CURRENT_DESKTOP:-}" in
        *KDE*) _tools="kdialog zenity" ;;
        *) _tools="zenity kdialog" ;;
        esac
        # Dialogs exit with 0 or 1 once shown (closing one is 1); more means it could not show.
        for _t in $_tools; do
            command -v "$_t" >/dev/null 2>&1 || continue
            case "$_t" in
            kdialog) kdialog --title "Kinoklip" --error "$_msg" >/dev/null 2>&1 ;;
            zenity) zenity --error --title="Kinoklip" --no-markup --text="$_msg" >/dev/null 2>&1 ;;
            esac
            [ $? -le 1 ] && exit 0
        done
        if command -v notify-send >/dev/null 2>&1 &&
            notify-send -u critical -a Kinoklip "Kinoklip could not start" "$_msg" >/dev/null 2>&1; then
            exit 0
        fi
        # gdbus comes with GLib, so a GNOME without zenity and libnotify's tools still has it
        if command -v gdbus >/dev/null 2>&1 &&
            gdbus call --session --dest org.freedesktop.Notifications \
                --object-path /org/freedesktop/Notifications \
                --method org.freedesktop.Notifications.Notify Kinoklip 0 "" \
                "Kinoklip could not start" "$_msg" "[]" "{}" 0 >/dev/null 2>&1; then
            exit 0
        fi
        command -v xmessage >/dev/null 2>&1 && xmessage -center "$_msg" >/dev/null 2>&1
    )
    return 0
}

# The loader reported a problem: the only path where this shell stays alive. The app runs as a
# background child, since a shell runs traps only between commands, so that TERM, HUP and INT
# can be passed on. Its stderr still reaches the same place, through tee, and is kept to name
# the cause. A start that works is remembered, so the next one is exec'ed again.
supervised_start() {
    log "the loader reports a problem; starting the app under watch: ${PREFLIGHT_OUT%%"
"*}"
    _sv_dir="$(mktemp -d "${TMPDIR:-/tmp}/kinoklip-start-XXXXXX" 2>/dev/null)" || _sv_dir=""
    if [ -n "$_sv_dir" ] && ! mkfifo "$_sv_dir/fifo" 2>/dev/null; then
        rm -rf "$_sv_dir"
        _sv_dir=""
    fi
    if [ -n "$_sv_dir" ]; then
        tee "$_sv_dir/log" <"$_sv_dir/fifo" >&2 &
        _tee=$!
        exec 3>"$_sv_dir/fifo" # held open until the end, so tee does not stop with an early exit
    else
        exec 3>&2
    fi
    { true <&0; } 2>/dev/null || exec </dev/null # a background job needs an explicit stdin
    trap 'kill -TERM "$_app" 2>/dev/null' TERM HUP
    trap 'kill -INT "$_app" 2>/dev/null; kill -TERM "$_app" 2>/dev/null' INT
    _started="$(date +%s)"
    start_app "$@" <&0 2>&3 3>&- &
    _app=$!
    while :; do
        wait "$_app"
        _status=$?
        kill -0 "$_app" 2>/dev/null || break
    done
    trap - TERM HUP INT
    exec 3>&-
    _why=""
    if [ -n "$_sv_dir" ]; then
        # tee ends now unless a program the app started still holds its stderr: not worth a hang
        _i=0
        while [ "$_i" -lt 10 ] && kill -0 "$_tee" 2>/dev/null; do
            sleep 0.05
            _i=$((_i + 1))
        done
        _why="$(grep -E 'error while loading shared libraries|symbol lookup error|version .* not found|undefined symbol|Qt platform plugin' \
            "$_sv_dir/log" 2>/dev/null | head -n 3)"
        rm -rf "$_sv_dir"
    fi
    case "$_status" in
    126 | 127) ;;
    *)
        # Qt aborts when no platform plugin loads, without a word if the message goes elsewhere: an
        # abort or crash right after the start counts. Anything later is the app's own business.
        _failed=0
        case "$_why" in *"Qt platform plugin"*) _failed=1 ;; esac
        case "$_status" in 134 | 139) [ $(($(date +%s) - _started)) -le 3 ] && _failed=1 ;; esac
        if [ "$_failed" = 0 ]; then
            remember_preflight_ok
            exit "$_status"
        fi
        ;;
    esac
    show_failure "${_why:-${PREFLIGHT_OUT:-The program exited with status $_status before it started.}}"
    exit "$_status"
}

if [ "${KINOKLIP_NO_PREFLIGHT:-0}" != 1 ] && ! preflight; then
    supervised_start "$@"
fi
if [ "$BUNDLED_GLIBC" = 1 ]; then
    exec "$LDSO" --library-path "$LDSO_PATH" --argv0 "$BIN" "$BIN" "$@"
fi
exec "$BIN" "$@"
