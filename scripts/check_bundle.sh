#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# check_bundle.sh -- verifies the Kinoklip AppDir / AppImage: every ELF file resolves all of
# its libraries either inside the bundle or to a library the exclude list leaves to the host,
# and the AppImage parts (desktop entry, icon, .DirIcon, AppRun, AppStream) are in place.
#
# Usage: scripts/check_bundle.sh [AppDir | .AppImage]
#   default: dist/kinoklip-x86_64, then dist/Kinoklip-x86_64.AppImage if it exists.
#   An .AppImage is unpacked (--appimage-extract, no FUSE needed) into a temporary directory
#   and checked like an AppDir, plus its embedded update information.
#
# Two passes, both with ldd (the dynamic loader's real search order):
#   1. normal:   LD_LIBRARY_PATH=usr/lib/qt, like AppRun in bundled-Qt mode
#   2. fallback: also every usr/lib/fallback library first, simulating a host that lacks all
#                of them (so their own dependencies are proven to resolve, too)
# Fails on "not found" and on any library loaded from the host that is not in the exclude
# list (it would be missing or a different version on the user's machine).
# Also fails if the exclude list slips back towards what broke a real system (GitHub issue 1):
# a host-only entry without a stated reason, a host-only soname known to differ between
# distributions, a bundled file needing a host-only library other than glibc, or an ordinary
# bundled library that only a fallback needs (it would shadow what host libraries need).
# Finally prints the host libraries the bundle relies on: the list the target must provide.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_ID=org.kinoklip.Kinoklip

# Without an argument: the AppDir and then the AppImage made from it.
if [ $# -eq 0 ]; then
    "$0" "$ROOT/dist/kinoklip-x86_64" || exit 1
    if [ -f "$ROOT/dist/Kinoklip-x86_64.AppImage" ]; then
        echo
        exec "$0" "$ROOT/dist/Kinoklip-x86_64.AppImage"
    fi
    exit 0
fi

B="$(readlink -f "$1")"
IMAGE=""
if [ -f "$B" ]; then
    IMAGE="$B"
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    echo "unpacking $IMAGE"
    (cd "$TMP" && "$IMAGE" --appimage-extract >/dev/null) || {
        echo "cannot unpack $IMAGE" >&2
        exit 2
    }
    B="$TMP/squashfs-root"
fi
[ -x "$B/usr/bin/kinoklip" ] || {
    echo "no bundle at $B" >&2
    exit 2
}

declare -a EX_PAT=()
while read -r kind pat _; do
    case "$kind" in '' | '#'*) continue ;; esac
    EX_PAT+=("$pat")
done <"$ROOT/packaging/linux/excludelist.txt"

excluded() {
    local p
    for p in "${EX_PAT[@]}"; do
        # shellcheck disable=SC2053
        [[ "$1" == $p ]] && return 0
    done
    return 1
}

errors=0

# ---- the exclude list itself ----
# Sonames that differ between distributions; this is how libbz2.so.1.0 (Debian, Arch) stayed host
# while Fedora only has libbz2.so.1. Never host-only, even if nothing needs them today.
KNOWN_DIFFERENT='libbz2.so.1.0 libbz2.so.1 libtinfo.so.5 libtinfo.so.6 libncursesw.so.5 libncursesw.so.6
libcrypt.so.1 libcrypt.so.2 libpcre.so.1 libpcre.so.3 libffi.so.6 libffi.so.7 libffi.so.8
libgcrypt.so.11 libgcrypt.so.20 libaio.so.1 libaio.so.1t64 libjpeg.so.8 libjpeg.so.62
libsystemd.so.0 libudev.so.1 libOpenGL.so.0'
GLIBC_HOST=''
while read -r line; do
    set -- $line
    [ "${1:-}" = host ] || continue
    pat="$2"
    case "$line" in
    *'#'*) ;;
    *)
        echo "EXCLUDE LIST: host entry without a reason (host <soname> # why): $pat"
        errors=$((errors + 1))
        ;;
    esac
    case "$line" in *'# glibc:'*) GLIBC_HOST="$GLIBC_HOST $pat" ;; esac
    for k in $KNOWN_DIFFERENT; do
        # shellcheck disable=SC2053
        if [[ "$k" == $pat ]]; then
            echo "EXCLUDE LIST: $k is named differently or missing on some distributions, so it must not be host-only"
            errors=$((errors + 1))
        fi
    done
done <"$ROOT/packaging/linux/excludelist.txt"
HOST_PATS=()
while read -r kind pat _; do
    [ "$kind" = host ] && HOST_PATS+=("$pat")
done <"$ROOT/packaging/linux/excludelist.txt"
is_host_only() {
    local p
    for p in "${HOST_PATS[@]}"; do
        # shellcheck disable=SC2053
        [[ "$1" == $p ]] && return 0
    done
    return 1
}

# all ELF files of the bundle (binary, libraries, plugins); fallback-active only holds links
mapfile -t ELFS < <(find "$B/usr" -path "$B/usr/lib/fallback-active" -prune -o -type f -print |
    while read -r f; do file -b "$f" | grep -q '^ELF' && echo "$f"; done)

declare -A HOST_USED=()
# Only the DIRECT dependencies (readelf NEEDED) of each bundled ELF are judged, resolved
# through that file's ldd output: ldd also lists what host libraries pull in themselves
# (libpulse -> libsndfile ...), which is the host's business. Since every bundled ELF is
# checked, the bundled part of the graph is still covered completely.
check_pass() {
    local label="$1" ldpath="$2" f line name path needed
    for f in "${ELFS[@]}"; do
        needed=" $(readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ') "
        while read -r line; do
            case "$line" in
            *' => '*) ;;
            *) continue ;;
            esac
            name="${line%% =>*}"
            case "$needed" in *" $name "*) ;; *) continue ;; esac
            path="${line#*=> }"
            path="${path%% (*}"
            if [ "$path" = "not found" ]; then
                echo "[$label] NOT FOUND: $name (needed by ${f#"$B"/})"
                errors=$((errors + 1))
                continue
            fi
            case "$path" in
            "$B"/*) ;;
            *)
                if excluded "$name"; then
                    HOST_USED[$name]=1
                else
                    echo "[$label] FROM HOST, NOT BUNDLED: $name => $path (needed by ${f#"$B"/})"
                    errors=$((errors + 1))
                fi
                ;;
            esac
        done < <(LD_LIBRARY_PATH="$ldpath" ldd "$f" 2>/dev/null)
    done
}

# Plugins the app cannot start without on the target desktops: a Wayland and an X11 platform
# plugin (Qt renamed the Wayland one in 6.10: libqwayland-egl.so -> libqwayland.so), the
# Wayland xdg-shell integration and a Wayland EGL client buffer integration (OpenGL viewers).
P="$B/usr/lib/qt/plugins"
for want in 'platforms/libqwayland*.so' 'platforms/libqxcb.so' \
    'wayland-shell-integration/libxdg-shell.so' 'wayland-graphics-integration-client/*egl*.so'; do
    if ! compgen -G "$P/$want" >/dev/null; then
        echo "MISSING PLUGIN: usr/lib/qt/plugins/$want"
        errors=$((errors + 1))
    fi
done

# AppImage requirements (AppImageSpec type 2; what Gear Lever and appimaged read): the desktop
# entry, an icon named like its Icon= key and a PNG .DirIcon at the top, AppRun, AppStream
# metadata, and the version Gear Lever shows (X-AppImage-Version).
appdir_fail() {
    echo "APPDIR: $*"
    errors=$((errors + 1))
}
D="$B/$APP_ID.desktop"
if [ ! -f "$D" ]; then
    appdir_fail "missing $APP_ID.desktop at the top"
else
    icon="$(sed -n 's/^Icon=//p' "$D")"
    [ -f "$B/$icon.png" ] || appdir_fail "no top-level icon $icon.png for Icon=$icon"
    [ -f "$B/usr/share/icons/hicolor/256x256/apps/$icon.png" ] || appdir_fail "no hicolor 256 px icon $icon.png"
    grep -q '^X-AppImage-Version=.' "$D" || appdir_fail "no X-AppImage-Version in the desktop entry"
    if command -v desktop-file-validate >/dev/null; then
        desktop-file-validate "$D" || appdir_fail "desktop entry does not validate"
    fi
fi
file -bL "$B/.DirIcon" 2>/dev/null | grep -q '^PNG image data, 256 x 256' || appdir_fail ".DirIcon is not a 256 px PNG"
[ -x "$B/AppRun" ] || appdir_fail "AppRun is missing or not executable"
M="$B/usr/share/metainfo/$APP_ID.appdata.xml"
if [ ! -f "$M" ]; then
    appdir_fail "missing usr/share/metainfo/$APP_ID.appdata.xml"
elif grep -q '@VERSION@\|@DATE@' "$M"; then
    appdir_fail "AppStream placeholders not filled in"
elif command -v appstreamcli >/dev/null; then
    appstreamcli validate --no-net "$M" >/dev/null || appdir_fail "AppStream metadata does not validate"
fi
if [ -n "$IMAGE" ]; then
    upd="$("$IMAGE" --appimage-updateinformation 2>/dev/null || true)"
    echo "update information: ${upd:-none}"
    case "$upd" in
    gh-releases-zsync\|*) ;;
    *) appdir_fail "no gh-releases-zsync update information embedded" ;;
    esac
    [ -f "$IMAGE.zsync" ] || appdir_fail "no $(basename "$IMAGE").zsync next to the AppImage"
fi

echo "checking ${#ELFS[@]} ELF files in $B"

# ---- what the bundle needs from the host: nothing but glibc, and every library it carries
# only for a fallback is a fallback itself ----
declare -A NEEDED_BY_NONFALLBACK=() NEEDED_BY_ANY=()
for f in "${ELFS[@]}"; do
    case "$f" in "$B/usr/lib/glibc/"*) continue ;; esac
    while read -r name; do
        NEEDED_BY_ANY[$name]=1
        case "$f" in "$B/usr/lib/fallback/"*) ;; *) NEEDED_BY_NONFALLBACK[$name]=1 ;; esac
        if is_host_only "$name"; then
            case " $GLIBC_HOST " in
            *" $name "*) ;;
            *)
                echo "HOST-ONLY BUT NEEDED: $name (needed by ${f#"$B"/}): a distribution without it could not start the app"
                errors=$((errors + 1))
                ;;
            esac
        fi
    done < <(readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
done
for f in "$B"/usr/lib/*.so* "$B"/usr/lib/qt/*.so*; do
    [ -f "$f" ] || continue
    name="${f##*/}"
    if [ -n "${NEEDED_BY_ANY[$name]+x}" ] && [ -z "${NEEDED_BY_NONFALLBACK[$name]+x}" ]; then
        echo "ONLY A FALLBACK NEEDS IT: ${f#"$B"/}: make $name a fallback, or it shadows the copy host libraries need"
        errors=$((errors + 1))
    fi
done

check_pass normal "$B/usr/lib/qt"
check_pass fallback "$B/usr/lib/fallback:$B/usr/lib/qt"

# The newest glibc symbol version any bundled ELF references = the oldest host glibc the bundle
# runs on natively; on older hosts AppRun switches to the bundled glibc (usr/lib/glibc).
# libstdc++/libgcc_s are "newer" fallbacks, so the GCC runtime is not a limit.
glibc_need="$(for f in "${ELFS[@]}"; do
    case "$f" in "$B/usr/lib/glibc/"*) continue ;; esac
    readelf -V --wide "$f" 2>/dev/null
done | grep -oE 'GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?' | sort -uV | tail -n 1)"
echo "minimum host glibc without the bundled one: ${glibc_need#GLIBC_}"
if [ -f "$B/usr/lib/glibc/glibc-required.txt" ]; then
    echo "bundled glibc for older hosts: $(ls "$B/usr/lib/glibc" | grep -c '\.so') libraries"
fi
echo "host libraries the bundle needs (from packaging/linux/excludelist.txt):"
printf '%s\n' "${!HOST_USED[@]}" | sort | column -c 100
if [ "$errors" -gt 0 ]; then
    echo "FAILED: $errors problem(s)"
    exit 1
fi
echo "OK: all libraries resolve inside the bundle or to excluded host libraries"
echo "bundle size: $(du -sh "$B" | cut -f1)${IMAGE:+, AppImage: $(du -h "$IMAGE" | cut -f1)}"
