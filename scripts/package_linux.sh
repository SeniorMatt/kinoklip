#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# package_linux.sh -- builds the portable Linux bundle of Kinoklip and the single-file .run.
#
# Usage: scripts/package_linux.sh [--no-build] [--no-run] [--no-tarball]
#
# Outputs (all under dist/, which is gitignored):
#   dist/kinoklip-x86_64/            AppDir-shaped portable folder (start it with ./AppRun)
#   dist/kinoklip-x86_64.tar.zst     the same folder as an archive (.tar.gz without zstd)
#   dist/Kinoklip-x86_64.run         self-extracting single file (double-click it)
#
# Environment knobs:
#   KK_BUILD_DIR      CMake build dir (default: build)
#   KK_JOBS           parallel build jobs (default: 4)
#   KK_COMPRESSION    zstd | xz | gzip for the archive/.run payload (default: zstd if present)
#   KK_ZSTD_LEVEL     zstd level (default: 19)
#   KK_QT_PLUGINS     host Qt plugin dir (default: from qtpaths6)
#
# Only tools from the distribution are used (ldd, readelf, patchelf, tar, zstd); no
# linuxdeploy/appimagetool. See packaging/linux/readme.md for the bundle layout and the
# reasoning behind every excluded library.
#
# Primary build host: the Arch Linux container kk-arch (Qt 6.12 + KDE Frameworks 6 + Breeze):
#   podman exec -w "$PWD" kk-arch env KK_BUILD_DIR=build-arch-release scripts/package_linux.sh
# The Mint/Ubuntu host (Qt 6.4, no KF6) still works and produces a bundle without Breeze.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKG="$ROOT/packaging/linux"
BUILD_DIR="${KK_BUILD_DIR:-$ROOT/build}"
DIST="$ROOT/dist"
APPDIR="$DIST/kinoklip-x86_64"
APP_ID="org.kinoklip.Kinoklip"

DO_BUILD=1
DO_RUN=1
DO_TARBALL=1
for arg in "$@"; do
    case "$arg" in
    --no-build) DO_BUILD=0 ;;
    --no-run) DO_RUN=0 ;;
    --no-tarball) DO_TARBALL=0 ;;
    -h | --help)
        sed -n '3,20p' "$0"
        exit 0
        ;;
    *)
        echo "unknown option: $arg" >&2
        exit 2
        ;;
    esac
done

log() { printf '[package] %s\n' "$*" >&2; }
die() {
    printf '[package] error: %s\n' "$*" >&2
    exit 1
}

for tool in ldd readelf patchelf tar sha256sum file; do
    command -v "$tool" >/dev/null || die "missing tool: $tool"
done

# ---------------------------------------------------------------------------------------
# 1. Release build
# ---------------------------------------------------------------------------------------
if [ "$DO_BUILD" = 1 ]; then
    log "building Release in $BUILD_DIR"
    cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$BUILD_DIR" -j"${KK_JOBS:-4}" --target kinoklip
fi
BIN="$BUILD_DIR/kinoklip"
[ -x "$BIN" ] || die "no binary at $BIN (run without --no-build)"
VERSION="$(sed -n 's/^project([^ ]* VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
VERSION="${VERSION:-0.0.0}"

QT_PLUGINS="${KK_QT_PLUGINS:-$(qtpaths6 --query QT_INSTALL_PLUGINS 2>/dev/null || true)}"
[ -d "$QT_PLUGINS" ] || QT_PLUGINS=/usr/lib/qt6/plugins # Arch
[ -d "$QT_PLUGINS" ] || QT_PLUGINS=/usr/lib/x86_64-linux-gnu/qt6/plugins # Debian/Ubuntu
[ -d "$QT_PLUGINS" ] || die "Qt plugin dir not found (set KK_QT_PLUGINS)"

# ---------------------------------------------------------------------------------------
# 2. Exclude list: soname glob -> "host" | "fallback"
# ---------------------------------------------------------------------------------------
declare -a EX_PAT=() EX_KIND=()
while read -r kind pat _; do
    case "$kind" in '' | '#'*) continue ;; esac
    EX_KIND+=("$kind")
    EX_PAT+=("$pat")
done <"$PKG/excludelist.txt"

# Prints host / fallback / newer / bundle for a soname.
classify() {
    local name="$1" i
    for i in "${!EX_PAT[@]}"; do
        # shellcheck disable=SC2053  # glob match is intended
        if [[ "$name" == ${EX_PAT[$i]} ]]; then
            echo "${EX_KIND[$i]}"
            return
        fi
    done
    echo bundle
}

# ---------------------------------------------------------------------------------------
# 3. Qt plugins we ship for the bundled-Qt mode (relative to the Qt plugin dir).
#    Not shipped: eglfs/linuxfb/vnc platforms and egldeviceintegrations (only used by the
#    eglfs platform: no desktop session needs it), gstreamer multimedia (we use the FFmpeg
#    backend that shares the bundled FFmpeg), platformthemes (gtk3 would drag in GTK; the
#    KDE theme cannot be built for Qt 6.4 -- host-Qt mode exists for that), tls, sql, print.
# ---------------------------------------------------------------------------------------
PLUGIN_GLOBS=(
    # Wayland: Qt >= 6.10 ships a single platforms/libqwayland.so; older Qt (6.4 host) has
    # the -egl/-generic pair. Both are listed; globs that match nothing are skipped.
    platforms/libqwayland.so
    platforms/libqwayland-egl.so
    platforms/libqwayland-generic.so
    platforms/libqxcb.so
    platforms/libqoffscreen.so
    platforms/libqminimal.so
    'wayland-shell-integration/*.so'
    'wayland-decoration-client/*.so'
    'wayland-graphics-integration-client/*.so'
    'xcbglintegrations/*.so'
    'imageformats/*.so'
    'iconengines/*.so'
    multimedia/libffmpegmediaplugin.so
    'platforminputcontexts/*.so'
    # The real Breeze look (KF6 builds only; the globs match nothing on the Qt 6.4 host):
    # the Breeze widget style, KDE's recolouring icon engine (Breeze icons are compiled into
    # libKF6BreezeIcons, pulled in as a dependency) and KWindowSystem's platform plugins
    # (Breeze uses them for window shadows/blur).
    styles/breeze6.so
    'kiconthemes6/iconengines/*.so'
    'kf6/kwindowsystem/*.so'
)

# libheif decodes HEIC/AVIF through runtime plugins (libde265, dav1d, aom, ffmpeg, ...), loaded
# from LIBHEIF_PLUGIN_PATH (set by AppRun to usr/lib/libheif/plugins). Arch keeps them in
# /usr/lib/libheif, Debian/Ubuntu in /usr/lib/<triplet>/libheif/plugins.
HEIF_PLUGIN_DIR=""
for d in /usr/lib/libheif/plugins /usr/lib/libheif /usr/lib/x86_64-linux-gnu/libheif/plugins; do
    if compgen -G "$d/*.so" >/dev/null; then
        HEIF_PLUGIN_DIR="$d"
        break
    fi
done

# KDE colour schemes for KColorSchemeManager (found through XDG_DATA_DIRS, set by AppRun).
COLOR_SCHEMES_DIR=/usr/share/color-schemes

# ---------------------------------------------------------------------------------------
# 4. Dependency walk.
#    We read each ELF's direct NEEDED entries (readelf) and resolve them to host paths via
#    that ELF's own ldd output. "host" libraries stop the walk (their dependencies are the
#    host's business). Two reachability sets decide where a library goes:
#      N = reachable from the app binary without passing through a libQt6* library
#      Q = reachable from Qt libraries / Qt plugins
#    libQt6* and Q-only libraries go to usr/lib/qt/ (used only in bundled-Qt mode, so in
#    host-Qt mode none of them can shadow what the host Qt loads); N goes to usr/lib/;
#    "fallback" libraries go to usr/lib/fallback/.
# ---------------------------------------------------------------------------------------
declare -A PATH_OF=() # soname -> host path
declare -A IN_N=() IN_Q=() SEEN_N=() SEEN_Q=()
declare -A KIND_CACHE=()

# Prints the kind of a soname. Called in $(...), so the cache is filled up front by
# prime_kind (in the main shell) for every name the walker meets.
kind_of() {
    local n="$1"
    if [ -n "${KIND_CACHE[$n]+x}" ]; then
        echo "${KIND_CACHE[$n]}"
    else
        classify "$n"
    fi
}
prime_kind() { [ -n "${KIND_CACHE[$1]+x}" ] || KIND_CACHE[$1]="$(classify "$1")"; }

# Sets DEPS to the direct NEEDED sonames of $1 and fills PATH_OF for them. (Results go
# through globals, not stdout: a $(...) subshell would lose the PATH_OF entries.)
declare -a DEPS=()
direct_deps() {
    local elf="$1" needed line name path
    DEPS=()
    needed="$(readelf -d "$elf" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')"
    [ -n "$needed" ] || return 0
    local -A map=()
    while read -r line; do
        case "$line" in
        *' => '*)
            name="${line%% =>*}"
            path="${line#*=> }"
            path="${path%% (*}"
            map[$name]="$path"
            ;;
        esac
    done < <(ldd "$elf" 2>/dev/null)
    for name in $needed; do
        prime_kind "$name"
        path="${map[$name]:-}"
        if [ "$(kind_of "$name")" != host ]; then
            if [ -z "$path" ] || [ "$path" = "not found" ]; then
                die "$elf needs $name, which is not found on the build host"
            fi
            PATH_OF[$name]="$path"
        fi
        DEPS+=("$name")
    done
}

# walk <set: N|Q> <elf>...  (iterative breadth-first walk)
walk() {
    local set="$1"
    shift
    local -a queue=("$@")
    local elf name
    while [ "${#queue[@]}" -gt 0 ]; do
        elf="${queue[0]}"
        queue=("${queue[@]:1}")
        direct_deps "$elf"
        for name in "${DEPS[@]}"; do
            [ "$(kind_of "$name")" = host ] && continue
            if [ "$set" = N ]; then
                [[ "$name" == libQt6* ]] && continue # Qt boundary: handled by walk Q
                [ -n "${SEEN_N[$name]+x}" ] && continue
                SEEN_N[$name]=1
                IN_N[$name]=1
            else
                [ -n "${SEEN_Q[$name]+x}" ] && continue
                SEEN_Q[$name]=1
                IN_Q[$name]=1
            fi
            queue+=("${PATH_OF[$name]}")
        done
    done
}

log "resolving dependencies"
walk N "$BIN"
declare -a HEIF_PLUGINS=()
if [ -n "$HEIF_PLUGIN_DIR" ] && [ -n "${IN_N[libheif.so.1]+x}" ]; then
    for f in "$HEIF_PLUGIN_DIR"/*.so; do HEIF_PLUGINS+=("$f"); done
    walk N "${HEIF_PLUGINS[@]}"
fi

# Qt roots: the Qt libraries the binary links, plus every shipped plugin.
declare -a QT_ROOTS=() PLUGIN_FILES=()
direct_deps "$BIN"
for name in "${DEPS[@]}"; do
    [[ "$name" == libQt6* ]] && QT_ROOTS+=("${PATH_OF[$name]}") && IN_Q[$name]=1 && SEEN_Q[$name]=1
done
# Qt Multimedia (QAudioSink) is always shipped, even before the playback code links it,
# so the FFmpeg media plugin has its library.
for glob in "${PLUGIN_GLOBS[@]}"; do
    for f in $QT_PLUGINS/$glob; do
        [ -f "$f" ] && PLUGIN_FILES+=("$f")
    done
done
walk Q "${QT_ROOTS[@]}" "${PLUGIN_FILES[@]}"

# ---------------------------------------------------------------------------------------
# 5. Assemble the AppDir
# ---------------------------------------------------------------------------------------
log "assembling $APPDIR"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib/qt/plugins" "$APPDIR/usr/lib/fallback" \
    "$APPDIR/usr/lib/fallback-active" "$APPDIR/usr/share/applications" \
    "$APPDIR/usr/share/icons/hicolor/scalable/apps" "$APPDIR/usr/share/mime/packages" \
    "$APPDIR/usr/share/doc/kinoklip/licenses"

install -m 755 "$BIN" "$APPDIR/usr/bin/kinoklip"
strip --strip-unneeded "$APPDIR/usr/bin/kinoklip"

declare -A DEST_OF=() # soname -> bundle-relative dir
for name in "${!IN_N[@]}" "${!IN_Q[@]}"; do
    [ -n "${DEST_OF[$name]+x}" ] && continue
    kind="$(kind_of "$name")"
    if [ "$kind" = fallback ] || [ "$kind" = newer ]; then
        DEST_OF[$name]=usr/lib/fallback
    elif [ -n "${IN_N[$name]+x}" ]; then
        DEST_OF[$name]=usr/lib
    else
        DEST_OF[$name]=usr/lib/qt
    fi
done
for name in "${!DEST_OF[@]}"; do
    # cp -L: the soname symlink becomes a regular file named after the soname.
    cp -L "${PATH_OF[$name]}" "$APPDIR/${DEST_OF[$name]}/$name"
    chmod 644 "$APPDIR/${DEST_OF[$name]}/$name"
done
for f in "${PLUGIN_FILES[@]}"; do
    rel="${f#"$QT_PLUGINS"/}"
    mkdir -p "$APPDIR/usr/lib/qt/plugins/$(dirname "$rel")"
    cp -L "$f" "$APPDIR/usr/lib/qt/plugins/$rel"
done
if [ "${#HEIF_PLUGINS[@]}" -gt 0 ]; then
    mkdir -p "$APPDIR/usr/lib/libheif/plugins"
    for f in "${HEIF_PLUGINS[@]}"; do cp -L "$f" "$APPDIR/usr/lib/libheif/plugins/"; done
fi
# glibc of the build host, for hosts with an OLDER glibc (e.g. an Arch build on Ubuntu 24.04:
# libm 2.43/2.44 symbol versions). It is not on any RUNPATH: AppRun starts the binary through
# usr/lib/glibc/ld-linux-x86-64.so.2 --library-path usr/lib/glibc only when the host glibc is
# older than glibc-required.txt, so on current hosts the system glibc is used as before and
# child processes never inherit the bundled one (ld.so options are not inherited).
GLIBC_DIR="$(dirname "$(readlink -f "$(ldd "$BIN" | sed -n 's/.*libc\.so\.6 => \([^ ]*\).*/\1/p')")")"
mkdir -p "$APPDIR/usr/lib/glibc"
for name in ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libmvec.so.1 libpthread.so.0 libdl.so.2 \
    librt.so.1 libresolv.so.2 libutil.so.1 libanl.so.1; do
    [ -e "$GLIBC_DIR/$name" ] && cp -L "$GLIBC_DIR/$name" "$APPDIR/usr/lib/glibc/$name"
done
GLIBC_REQUIRED="$(find "$APPDIR/usr" -path "$APPDIR/usr/lib/glibc" -prune -o -type f \( -name '*.so*' -o -path '*/bin/*' \) -print |
    while read -r f; do readelf -V --wide "$f" 2>/dev/null; done |
    grep -oE 'Name: GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?' | sed 's/Name: GLIBC_//' | sort -uV | tail -n 1)"
echo "$GLIBC_REQUIRED" >"$APPDIR/usr/lib/glibc/glibc-required.txt"
log "bundle needs glibc >= $GLIBC_REQUIRED; build host glibc $(getconf GNU_LIBC_VERSION 2>/dev/null | cut -d' ' -f2) shipped for older hosts"

# fallback libraries that are only used when the host copy is older (libstdc++, libgcc_s):
# AppRun compares this version tag with the host library's.
: >"$APPDIR/usr/lib/fallback/newer.txt"
for name in "${!DEST_OF[@]}"; do
    [ "$(kind_of "$name")" = newer ] || continue
    tag="$(grep -aoE 'GLIBCXX_3\.4\.[0-9]+|GCC_[0-9]+\.[0-9]+(\.[0-9]+)?' "$APPDIR/usr/lib/fallback/$name" | sort -uV | tail -n1 || true)"
    if [ -z "$tag" ]; then # no symbol versions: the real file name carries the version
        real="$(readlink -f "${PATH_OF[$name]}")"
        tag="file:${real##*.so.}"
    fi
    echo "$name $tag" >>"$APPDIR/usr/lib/fallback/newer.txt"
done
# KF6 builds: colour schemes and the Breeze icon theme in usr/share (first in XDG_DATA_DIRS,
# set by AppRun). The icons are also compiled into libKF6BreezeIcons, but KIconLoader
# searches the XDG icon dirs before that, so a host with an older Breeze (e.g. Breeze 5 on
# Ubuntu) would otherwise show different icons. ~5 MB compressed (most files are symlinks);
# breeze-dark is not needed (KIconEngine recolours breeze from the palette).
if [ -d "$COLOR_SCHEMES_DIR" ] && [ -f "$QT_PLUGINS/styles/breeze6.so" ]; then
    mkdir -p "$APPDIR/usr/share/color-schemes" "$APPDIR/usr/share/icons"
    cp -L "$COLOR_SCHEMES_DIR"/*.colors "$APPDIR/usr/share/color-schemes/"
    # Kinoklip's own scheme (also compiled into the app for the non-KF6 fallback).
    cp "$ROOT/resources/color-schemes/Kinoklip.colors" "$APPDIR/usr/share/color-schemes/"
    if [ -f /usr/share/icons/breeze/index.theme ]; then
        tar -C /usr/share/icons --exclude='*.rcc' -cf - breeze | tar -C "$APPDIR/usr/share/icons" -xf -
    fi
fi

# RUNPATHs. They make the bundle work without LD_LIBRARY_PATH for everything except Qt
# (which the launcher selects per mode). Never include usr/lib/qt in the binary's RUNPATH,
# or host-Qt mode could not use the system Qt.
#   fallback-active/ holds symlinks to fallback/ copies the host lacks (see AppRun).
log "setting RUNPATHs"
# shellcheck disable=SC2016  # $ORIGIN is for ld.so, not the shell
{
    patchelf --set-rpath '$ORIGIN/../lib:$ORIGIN/../lib/fallback-active' "$APPDIR/usr/bin/kinoklip"
    for f in "$APPDIR"/usr/lib/*.so*; do
        patchelf --set-rpath '$ORIGIN:$ORIGIN/fallback-active' "$f"
    done
    for f in "$APPDIR"/usr/lib/qt/*.so*; do
        patchelf --set-rpath '$ORIGIN:$ORIGIN/..:$ORIGIN/../fallback-active' "$f"
    done
    # loaded through the symlinks in fallback-active/, so $ORIGIN is that directory
    for f in "$APPDIR"/usr/lib/fallback/*.so*; do
        [ -e "$f" ] || continue
        patchelf --set-rpath '$ORIGIN/..:$ORIGIN:$ORIGIN/../qt' "$f"
    done
    for f in "$APPDIR"/usr/lib/qt/plugins/*/*.so; do
        patchelf --set-rpath '$ORIGIN/../..:$ORIGIN/../../..:$ORIGIN/../../../fallback-active' "$f"
    done
    # two levels deeper: kiconthemes6/iconengines/, kf6/kwindowsystem/
    for f in "$APPDIR"/usr/lib/qt/plugins/*/*/*.so; do
        [ -e "$f" ] || continue
        patchelf --set-rpath '$ORIGIN/../../..:$ORIGIN/../../../..:$ORIGIN/../../../../fallback-active' "$f"
    done
    for f in "$APPDIR"/usr/lib/libheif/plugins/*.so; do
        [ -e "$f" ] || continue
        patchelf --set-rpath '$ORIGIN/../..:$ORIGIN/../../fallback-active' "$f"
    done
}

# Host-Qt requirements (for the opt-in KINOKLIP_HOST_QT=1 mode): every Qt library the
# binary needs, directly or through other Qt libraries. AppRun refuses the host Qt unless
# all of them exist there.
# Keep only Qt libs reachable from the binary: walk libQt6 NEEDED from the binary.
declare -A REQ=()
pending=($(readelf -d "$APPDIR/usr/bin/kinoklip" | sed -n 's/.*(NEEDED).*\[\(libQt6.*\)\]/\1/p'))
# Qt Multimedia is part of the contract even if not linked yet (audio output).
pending+=(libQt6Multimedia.so.6)
while [ "${#pending[@]}" -gt 0 ]; do
    q="${pending[0]}"
    pending=("${pending[@]:1}")
    [ -n "${REQ[$q]+x}" ] && continue
    REQ[$q]=1
    if [ -f "$APPDIR/usr/lib/qt/$q" ]; then
        pending+=($(readelf -d "$APPDIR/usr/lib/qt/$q" | sed -n 's/.*(NEEDED).*\[\(libQt6.*\)\]/\1/p'))
    fi
done
{
    echo "# Qt libraries kinoklip needs from a host Qt (generated by scripts/package_linux.sh)."
    printf '%s\n' "${!REQ[@]}" | sort
} >"$APPDIR/usr/lib/qt/host-qt-requirements.txt"
# The binary uses Qt GuiPrivate (the Wayland portal parent window, src/platform), which is tied
# to the exact Qt build: a host Qt must be exactly this version (major.minor.patch).
QT_BUILD_VERSION="$(readlink -f "${PATH_OF[libQt6Core.so.6]}")"
QT_BUILD_VERSION="${QT_BUILD_VERSION##*/libQt6Core.so.}"
echo "$QT_BUILD_VERSION" >"$APPDIR/usr/lib/qt/qt-version.txt"

# Launcher, desktop entry, icon, MIME type
install -m 755 "$PKG/apprun.sh" "$APPDIR/AppRun"
install -m 644 "$PKG/$APP_ID.desktop" "$APPDIR/$APP_ID.desktop"
install -m 644 "$PKG/$APP_ID.desktop" "$APPDIR/usr/share/applications/$APP_ID.desktop"
install -m 644 "$PKG/icons/$APP_ID.svg" "$APPDIR/$APP_ID.svg"
install -m 644 "$PKG/icons/$APP_ID.svg" "$APPDIR/usr/share/icons/hicolor/scalable/apps/$APP_ID.svg"
ln -s "$APP_ID.svg" "$APPDIR/.DirIcon"
install -m 644 "$PKG/$APP_ID.xml" "$APPDIR/usr/share/mime/packages/$APP_ID.xml"
echo "$VERSION" >"$APPDIR/usr/share/doc/kinoklip/version"

# Licence notices of the bundled libraries (GPL/LGPL compliance; see plan.md section 8).
# Debian/Ubuntu: dpkg -S + /usr/share/doc/<pkg>/copyright. Arch: pacman -Qo + /usr/share/licenses/<pkg>/
# (Arch packages put the licence text there unless it is a common licence, whose text is in
# /usr/share/licenses/spdx/ -- copied for every SPDX id a bundled package names).
log "collecting licence files"
LICENSES="$APPDIR/usr/share/doc/kinoklip/licenses"
pkg_of() {
    local f
    f="$(readlink -f "$1")"
    if command -v pacman >/dev/null; then
        pacman -Qqo "$f" 2>/dev/null | head -n1
    elif command -v dpkg >/dev/null; then
        dpkg -S "$f" 2>/dev/null | head -n1 | cut -d: -f1
    fi
}
pkg_version() {
    if command -v pacman >/dev/null; then
        pacman -Q "$1" 2>/dev/null | cut -d' ' -f2
    else
        dpkg-query -W -f='${Version}' "$1" 2>/dev/null
    fi
}
copy_licence() {
    local pkg="$1" id
    if command -v pacman >/dev/null; then
        if [ -d "/usr/share/licenses/$pkg" ]; then
            mkdir -p "$LICENSES/$pkg"
            cp -rL "/usr/share/licenses/$pkg/." "$LICENSES/$pkg/"
        fi
        # SPDX ids from the package metadata, e.g. "LGPL-2.1-or-later  GPL-3.0-only"
        for id in $(LC_ALL=C pacman -Qi "$pkg" 2>/dev/null | sed -n 's/^Licenses *: //p'); do
            id="${id//[()]/}"
            case "$id" in AND | OR | WITH | '') continue ;; esac
            if [ -f "/usr/share/licenses/spdx/$id.txt" ]; then
                mkdir -p "$LICENSES/spdx"
                cp -L "/usr/share/licenses/spdx/$id.txt" "$LICENSES/spdx/"
            fi
        done
    elif [ -f "/usr/share/doc/$pkg/copyright" ]; then
        cp -L "/usr/share/doc/$pkg/copyright" "$LICENSES/$pkg.copyright"
    fi
}
declare -A LICENCE_DONE=()
{
    echo "Bundled third-party libraries and plugins (from the build host's packages)."
    echo "Licence texts: licenses/<package>/ (Arch) or licenses/<package>.copyright (Debian),"
    echo "common licence texts in licenses/spdx/. Source: the distribution's source packages."
    echo
    {
        for name in "${!DEST_OF[@]}"; do echo "$name ${PATH_OF[$name]}"; done
        for f in "${PLUGIN_FILES[@]}" "${HEIF_PLUGINS[@]}"; do echo "${f##*/} $f"; done
        if [ -d "$APPDIR/usr/share/color-schemes" ]; then
            for f in "$COLOR_SCHEMES_DIR"/*.colors; do echo "${f##*/} $f"; done
        fi
        if [ -f "$APPDIR/usr/share/icons/breeze/index.theme" ]; then
            echo "icons/breeze /usr/share/icons/breeze/index.theme"
        fi
    } | sort | while read -r name path; do
        pkg="$(pkg_of "$path")"
        printf '%-44s %-28s %s\n' "$name" "${pkg:-?}" "$( [ -n "$pkg" ] && pkg_version "$pkg")"
    done
} >"$APPDIR/usr/share/doc/kinoklip/bundled-libraries.txt"
for pkg in $(awk 'NR > 4 && $2 != "?" { print $2 }' "$APPDIR/usr/share/doc/kinoklip/bundled-libraries.txt" | sort -u); do
    [ -n "${LICENCE_DONE[$pkg]+x}" ] && continue
    LICENCE_DONE[$pkg]=1
    copy_licence "$pkg"
done

# ---------------------------------------------------------------------------------------
# 6. Archive + single-file .run
# ---------------------------------------------------------------------------------------
COMP="${KK_COMPRESSION:-}"
if [ -z "$COMP" ]; then
    if command -v zstd >/dev/null; then COMP=zstd; else COMP=gzip; fi
fi
case "$COMP" in
zstd) EXT=zst COMPRESS=(zstd -q -T0 "-${KK_ZSTD_LEVEL:-19}" -c) ;;
xz) EXT=xz COMPRESS=(xz -T0 -9 -c) ;;
gzip) EXT=gz COMPRESS=(gzip -9 -n -c) ;;
*) die "KK_COMPRESSION must be zstd, xz or gzip" ;;
esac

if [ "$DO_TARBALL" = 1 ] || [ "$DO_RUN" = 1 ]; then
    TARBALL="$DIST/kinoklip-x86_64.tar.$EXT"
    rm -f "$DIST"/kinoklip-x86_64.tar.*
    log "compressing ($COMP) -> $TARBALL"
    # Reproducible archive: sorted entries, fixed owner and mtime, so identical contents give
    # an identical hash (and the .run reuses its existing cache directory).
    tar -C "$DIST" --sort=name --owner=0 --group=0 --numeric-owner \
        --mtime='2026-01-01 00:00:00Z' -cf - kinoklip-x86_64 | "${COMPRESS[@]}" >"$TARBALL"
fi

if [ "$DO_RUN" = 1 ]; then
    RUN="$DIST/Kinoklip-x86_64.run"
    HASH="$(sha256sum "$TARBALL" | cut -c1-16)"
    HEADER="$(mktemp)"
    # Fill in the template. @OFFSET@ is replaced by a fixed-width (12 digit) number so that
    # the header length -- and thus the payload offset -- does not change by substituting it.
    sed -e "s/@HASH@/$HASH/" -e "s/@VERSION@/$VERSION/" -e "s/@COMPRESSION@/$COMP/" \
        "$PKG/runheader.sh" >"$HEADER"
    size=$(wc -c <"$HEADER")
    offset=$(printf '%012d' $((size + 1))) # tail -c +N is 1-based
    sed -i "s/@@@@OFFSET@@/$offset/" "$HEADER"
    [ "$(wc -c <"$HEADER")" = "$size" ] || die "runheader.sh: offset placeholder must be 12 chars"
    cat "$HEADER" "$TARBALL" >"$RUN"
    chmod 755 "$RUN"
    rm -f "$HEADER"
    [ "$DO_TARBALL" = 1 ] || rm -f "$TARBALL"
fi

# ---------------------------------------------------------------------------------------
# 7. Report
# ---------------------------------------------------------------------------------------
log "bundle: $(du -sh "$APPDIR" | cut -f1) in $APPDIR"
log "  usr/lib: $(ls "$APPDIR/usr/lib" | grep -c '\.so') libs, usr/lib/qt: $(ls "$APPDIR/usr/lib/qt" | grep -c '\.so') libs," \
    "fallback: $(ls "$APPDIR/usr/lib/fallback" | grep -c '\.so') libs, plugins: ${#PLUGIN_FILES[@]} Qt + ${#HEIF_PLUGINS[@]} libheif"
for f in "$DIST"/kinoklip-x86_64.tar.* "$DIST"/Kinoklip-x86_64.run; do
    [ -f "$f" ] && log "$(du -h "$f" | cut -f1)	$f"
done
exit 0
