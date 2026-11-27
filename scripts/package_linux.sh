#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# package_linux.sh -- builds the Kinoklip AppImage: dist/Kinoklip-x86_64.AppImage (+ .zsync);
# on an ARM64 machine (or KK_ARCH=aarch64) dist/Kinoklip-aarch64.AppImage, same bundle layout.
#
# Usage: scripts/package_linux.sh [--no-build] [--no-appimage] [--run] [--tarball]
#
#   --no-build     reuse $KK_BUILD_DIR/kinoklip instead of building it
#   --no-appimage  stop after the AppDir (dist/kinoklip-x86_64/, start it with ./AppRun)
#   --run          also make the legacy self-extracting dist/Kinoklip-x86_64.run (not released)
#   --tarball      also make dist/kinoklip-x86_64.tar.zst, the AppDir as an archive (not released)
#
# Outputs (all under dist/, which is gitignored):
#   dist/Kinoklip-x86_64.AppImage        the release artefact (type 2, static runtime)
#   dist/Kinoklip-x86_64.AppImage.zsync  delta-update file for Gear Lever / AppImageUpdate
#   dist/kinoklip-x86_64/                the AppDir it is made from
#
# Environment knobs:
#   KK_BUILD_DIR      CMake build dir (default: build; may be outside the source tree)
#   KK_JOBS           parallel build jobs (default: 4)
#   KK_TOOLS_CACHE    where the pinned appimagetool + runtime are cached (default: .cache/appimage-tools)
#   KK_UPDATE_INFO    update information embedded in the AppImage ("" = none)
#   KK_DEV_BUILD      short commit: makes a dev package (scripts/dev_build.sh). Stamps the app
#                     (--version, About: "<version> dev (<commit>)"), names the file
#                     KinoklipDev-<arch>.AppImage (+ .zsync) and embeds the dev channel's update
#                     information (KinoklipDev-*): Gear Lever updates a dev copy from the rolling
#                     "dev" pre-release, and release copies (Kinoklip-*) never see dev files.
#                     Unset (the default) = the release package, unchanged.
#   KK_DIST_DIR       output directory (default: dist; dev_build.sh uses dist/dev)
#   KK_COMPRESSION    zstd | xz | gzip for --run / --tarball (default: zstd if present)
#   KK_ZSTD_LEVEL     zstd level of the AppImage squashfs and --run / --tarball (default: 19)
#   KK_QT_PLUGINS     host Qt plugin dir (default: from qtpaths6)
#
# The AppDir is assembled with distribution tools only (ldd, readelf, patchelf); the AppImage
# is made with the official appimagetool and type 2 runtime, downloaded once at the pinned
# versions below and checked against their SHA-256. See packaging/linux/readme.md for the
# layout and the reasoning behind every excluded library.
#
# Primary build host: the Arch Linux container kk-arch (Qt 6.12 + KDE Frameworks 6 + Breeze):
#   podman exec -w "$PWD" kk-arch env KK_BUILD_DIR=build-arch-release scripts/package_linux.sh
# The Mint/Ubuntu host (Qt 6.4, no KF6) still works and produces a bundle without Breeze.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Target architecture: x86_64 (the default on the Arch container) or aarch64 (GitHub's ARM runner,
# .github/workflows/arm64-release.yml). KK_ARCH overrides; the x86_64 output is the same as ever.
ARCH="${KK_ARCH:-$(uname -m)}"
case "$ARCH" in
x86_64) TRIPLET=x86_64-linux-gnu LDSO_NAME=ld-linux-x86-64.so.2 ;;
aarch64 | arm64) ARCH=aarch64 TRIPLET=aarch64-linux-gnu LDSO_NAME=ld-linux-aarch64.so.1 ;;
*) echo "unsupported architecture: $ARCH" >&2; exit 2 ;;
esac
PKG="$ROOT/packaging/linux"
BUILD_DIR="${KK_BUILD_DIR:-$ROOT/build}"
DIST="${KK_DIST_DIR:-$ROOT/dist}"
DEV_COMMIT="${KK_DEV_BUILD:-}"
APPDIR="$DIST/kinoklip-$ARCH"
APP_ID="org.kinoklip.Kinoklip"

DO_BUILD=1
DO_APPIMAGE=1
DO_RUN=0
DO_TARBALL=0
for arg in "$@"; do
    case "$arg" in
    --no-build) DO_BUILD=0 ;;
    --no-appimage) DO_APPIMAGE=0 ;;
    --run) DO_RUN=1 ;;
    --tarball) DO_TARBALL=1 ;;
    -h | --help)
        sed -n '3,32p' "$0"
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
    cmake --build "$BUILD_DIR" -j"${KK_JOBS:-4}" --target kinoklip kinoklip-ofx-probe kinoklip-audio-probe kinoklip-hwenc-probe
fi
BIN="$BUILD_DIR/kinoklip"
[ -x "$BIN" ] || die "no binary at $BIN (run without --no-build)"
VERSION="$(sed -n 's/^project([^ ]* VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
VERSION="${VERSION:-0.0.0}"

QT_PLUGINS="${KK_QT_PLUGINS:-$(qtpaths6 --query QT_INSTALL_PLUGINS 2>/dev/null || true)}"
[ -d "$QT_PLUGINS" ] || QT_PLUGINS=/usr/lib/qt6/plugins # Arch
[ -d "$QT_PLUGINS" ] || QT_PLUGINS=/usr/lib/$TRIPLET/qt6/plugins # Debian/Ubuntu
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
#    KDE theme cannot be built for Qt 6.4 -- host-Qt mode exists for that), sql, print.
#    tls is shipped: without it every HTTPS download (the AI model) fails with "TLS
#    initialization failed".
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
    tls/libqopensslbackend.so
    tls/libqcertonlybackend.so
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
for d in /usr/lib/libheif/plugins /usr/lib/libheif /usr/lib/$TRIPLET/libheif/plugins /usr/lib64/libheif/plugins; do
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
    "$APPDIR/usr/share/mime/packages" \
    "$APPDIR/usr/share/doc/kinoklip/licenses"

install -m 755 "$BIN" "$APPDIR/usr/bin/kinoklip"
strip --strip-unneeded "$APPDIR/usr/bin/kinoklip"
# The helper that loads one OpenFX bundle in a process of its own (spec 158): Qt Core only.
if [ -x "$BUILD_DIR/kinoklip-ofx-probe" ]; then
    install -m 755 "$BUILD_DIR/kinoklip-ofx-probe" "$APPDIR/usr/bin/kinoklip-ofx-probe"
    strip --strip-unneeded "$APPDIR/usr/bin/kinoklip-ofx-probe"
fi
# The same for LV2 and CLAP audio plugins (spec 159): Qt Core, lilv when the build has it.
if [ -x "$BUILD_DIR/kinoklip-audio-probe" ]; then
    install -m 755 "$BUILD_DIR/kinoklip-audio-probe" "$APPDIR/usr/bin/kinoklip-audio-probe"
    strip --strip-unneeded "$APPDIR/usr/bin/kinoklip-audio-probe"
fi

# The hardware encoder test (issue 18): one process per encoder, so a driver crash costs only the test.
if [ -x "$BUILD_DIR/kinoklip-hwenc-probe" ]; then
    install -m 755 "$BUILD_DIR/kinoklip-hwenc-probe" "$APPDIR/usr/bin/kinoklip-hwenc-probe"
    strip --strip-unneeded "$APPDIR/usr/bin/kinoklip-hwenc-probe"
fi

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
# usr/lib/glibc/$LDSO_NAME --library-path usr/lib/glibc only when the host glibc is
# older than glibc-required.txt, so on current hosts the system glibc is used as before and
# child processes never inherit the bundled one (ld.so options are not inherited).
GLIBC_DIR="$(dirname "$(readlink -f "$(ldd "$BIN" | sed -n 's/.*libc\.so\.6 => \([^ ]*\).*/\1/p')")")"
mkdir -p "$APPDIR/usr/lib/glibc"
for name in "$LDSO_NAME" libc.so.6 libm.so.6 libmvec.so.1 libpthread.so.0 libdl.so.2 \
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

# Qt's own translations (standard buttons, file dialog) for the languages Kinoklip is translated
# into; src/core/Translations.cpp looks in usr/share/kinoklip/translations when the Qt in use
# has no translations folder of its own. Kinoklip's strings are compiled into the binary.
QT_TRANSLATIONS="$(qtpaths6 --query QT_INSTALL_TRANSLATIONS 2>/dev/null || true)"
[ -d "$QT_TRANSLATIONS" ] || QT_TRANSLATIONS=/usr/share/qt6/translations # Arch
[ -d "$QT_TRANSLATIONS" ] || QT_TRANSLATIONS=/usr/share/qt6/translations
mkdir -p "$APPDIR/usr/share/kinoklip/translations"
for code in ru de uk es; do
    if [ -f "$QT_TRANSLATIONS/qtbase_$code.qm" ]; then
        cp -L "$QT_TRANSLATIONS/qtbase_$code.qm" "$APPDIR/usr/share/kinoklip/translations/"
    else
        log "WARNING: no qtbase_$code.qm in $QT_TRANSLATIONS: Qt's own dialogs stay English in that language"
    fi
done

# Colour emoji for titles and captions (spec 69): Noto Color Emoji (SIL OFL 1.1) from the build
# host's font package (Arch: noto-fonts-emoji, Debian: fonts-noto-color-emoji). Kinoklip registers
# it at run time only when the system has no colour emoji font of its own
# (src/render/EmojiText.cpp), so a user's own emoji font still wins.
EMOJI_FONT=
for f in /usr/share/fonts/noto/NotoColorEmoji.ttf /usr/share/fonts/truetype/noto/NotoColorEmoji.ttf \
    /usr/share/fonts/google-noto-color-emoji-fonts/NotoColorEmoji.ttf; do
    if [ -f "$f" ]; then
        EMOJI_FONT=$f
        break
    fi
done
if [ -n "$EMOJI_FONT" ]; then
    log "bundling $EMOJI_FONT"
    mkdir -p "$APPDIR/usr/share/kinoklip/fonts"
    cp -L "$EMOJI_FONT" "$APPDIR/usr/share/kinoklip/fonts/NotoColorEmoji.ttf"
else
    log "WARNING: no Noto Color Emoji on this host (install noto-fonts-emoji): emoji in titles need the user's own font"
fi

# Plugins (docs/plugins): the example plugins people copy from (plugin.copy_builtin, Install Folder)
# and the folder bundled plugins load from (src/plugins/PluginRegistry.cpp: share/kinoklip/plugins).
rm -rf "$APPDIR/usr/share/kinoklip/plugins"
mkdir -p "$APPDIR/usr/share/kinoklip/plugins"
cp -r "$ROOT/plugins/bundled/." "$APPDIR/usr/share/kinoklip/plugins/"
rm -rf "$APPDIR/usr/share/kinoklip/plugin-examples"
cp -r "$ROOT/plugins/examples" "$APPDIR/usr/share/kinoklip/plugin-examples"
install -D -m 644 "$ROOT/docs/plugins/plugin.schema.json" "$APPDIR/usr/share/kinoklip/plugin.schema.json"

# OpenFX pack (opt-in, spec 158): KK_OFX_MISC_DIR holds Misc.ofx.bundle as made by
# scripts/build_openfx_misc.sh (openfx-misc, GPL-2.0-or-later, about 6 MB more in the AppImage).
# It goes to the folder bundled OpenFX plugins load from (src/plugins/OfxRegistry.cpp).
if [ -n "${KK_OFX_MISC_DIR:-}" ] && [ -d "$KK_OFX_MISC_DIR/Misc.ofx.bundle" ]; then
    log "bundling openfx-misc from $KK_OFX_MISC_DIR"
    mkdir -p "$APPDIR/usr/share/kinoklip/ofx"
    rm -rf "$APPDIR/usr/share/kinoklip/ofx/Misc.ofx.bundle"
    cp -r "$KK_OFX_MISC_DIR/Misc.ofx.bundle" "$APPDIR/usr/share/kinoklip/ofx/"
    find "$APPDIR/usr/share/kinoklip/ofx" -name '*.ofx' -exec strip --strip-unneeded {} +
    install -D -m 644 "$KK_OFX_MISC_DIR/Misc.ofx.bundle/Contents/Resources/LICENSE-openfx-misc.txt" \
        "$APPDIR/usr/share/doc/kinoklip/licenses/openfx-misc-LICENSE.txt"
fi

# LV2 specifications (about 1 MB): lilv reads them to understand the plugins' descriptions; the AppImage
# must not depend on /usr/lib/lv2 of the host (src/audioplug/Lv2Host.cpp looks in share/kinoklip/lv2-specs).
if [ -d /usr/lib/lv2 ] && ls /usr/lib/lv2/lv2core.lv2/manifest.ttl >/dev/null 2>&1; then
    mkdir -p "$APPDIR/usr/share/kinoklip/lv2-specs"
    for spec in /usr/lib/lv2/*.lv2; do
        [ -f "$spec/manifest.ttl" ] && cp -r "$spec" "$APPDIR/usr/share/kinoklip/lv2-specs/"
    done
fi

# Free audio plugins (opt-in, spec 159): KK_LV2_BUNDLE_DIR holds *.lv2 folders, KK_CLAP_BUNDLE_DIR *.clap
# files (for example the LSP plugins, LGPL-3.0-or-later, about 4 MB packed for CLAP; not bundled by default:
# the upstream release is 38 MB for all formats). They go to the folders
# bundled plugins load from (src/plugins/AudioPluginRegistry.cpp); each licence text travels with them.
if [ -n "${KK_LV2_BUNDLE_DIR:-}" ] && ls -d "$KK_LV2_BUNDLE_DIR"/*.lv2 >/dev/null 2>&1; then
    log "bundling LV2 plugins from $KK_LV2_BUNDLE_DIR"
    mkdir -p "$APPDIR/usr/share/kinoklip/lv2"
    cp -r "$KK_LV2_BUNDLE_DIR"/*.lv2 "$APPDIR/usr/share/kinoklip/lv2/"
    find "$APPDIR/usr/share/kinoklip/lv2" -name '*.so' -exec strip --strip-unneeded {} +
fi
if [ -n "${KK_CLAP_BUNDLE_DIR:-}" ] && ls "$KK_CLAP_BUNDLE_DIR"/*.clap >/dev/null 2>&1; then
    log "bundling CLAP plugins from $KK_CLAP_BUNDLE_DIR"
    mkdir -p "$APPDIR/usr/share/kinoklip/clap"
    cp "$KK_CLAP_BUNDLE_DIR"/*.clap "$APPDIR/usr/share/kinoklip/clap/"
    strip --strip-unneeded "$APPDIR"/usr/share/kinoklip/clap/*.clap
fi
if [ -n "${KK_AUDIO_PLUGIN_LICENSES:-}" ] && [ -d "$KK_AUDIO_PLUGIN_LICENSES" ]; then
    cp "$KK_AUDIO_PLUGIN_LICENSES"/* "$APPDIR/usr/share/doc/kinoklip/licenses/"
fi

# RUNPATHs. They make the bundle work without LD_LIBRARY_PATH for everything except Qt
# (which the launcher selects per mode). Never include usr/lib/qt in the binary's RUNPATH,
# or host-Qt mode could not use the system Qt.
#   fallback-active/ holds symlinks to fallback/ copies the host lacks (see AppRun).
log "setting RUNPATHs"
# shellcheck disable=SC2016  # $ORIGIN is for ld.so, not the shell
{
    patchelf --set-rpath '$ORIGIN/../lib:$ORIGIN/../lib/fallback-active' "$APPDIR/usr/bin/kinoklip"
    if [ -x "$APPDIR/usr/bin/kinoklip-ofx-probe" ]; then
        patchelf --set-rpath '$ORIGIN/../lib:$ORIGIN/../lib/fallback-active' "$APPDIR/usr/bin/kinoklip-ofx-probe"
    fi
    if [ -x "$APPDIR/usr/bin/kinoklip-audio-probe" ]; then
        patchelf --set-rpath '$ORIGIN/../lib:$ORIGIN/../lib/fallback-active' "$APPDIR/usr/bin/kinoklip-audio-probe"
    fi
    if [ -x "$APPDIR/usr/bin/kinoklip-hwenc-probe" ]; then
        patchelf --set-rpath '$ORIGIN/../lib:$ORIGIN/../lib/fallback-active' "$APPDIR/usr/bin/kinoklip-hwenc-probe"
    fi
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

# Launcher, desktop entry, icon, MIME type, AppStream metadata
PRERELEASE="$(sed -n 's/.*prerelease = "\([^"]*\)".*/\1/p' "$ROOT/src/app/AppInfo.h")"
TAG="v$VERSION${PRERELEASE:+-$PRERELEASE}"
FULL_VERSION="${TAG#v}" # as the app shows it, e.g. 0.4.1-alpha.1
install -m 755 "$PKG/apprun.sh" "$APPDIR/AppRun"
if [ -n "$DEV_COMMIT" ]; then # read by appinfo::devBuildCommit()
    mkdir -p "$APPDIR/usr/share/kinoklip"
    echo "$DEV_COMMIT" >"$APPDIR/usr/share/kinoklip/dev-build"
fi
# X-AppImage-Version: the version Gear Lever / appimaged show (after [Desktop Entry]'s keys).
# A dev build is "Kinoklip Dev" in the launcher (the app shows the same name: appinfo::displayName).
{
    if [ -n "$DEV_COMMIT" ]; then
        sed 's/^Name=Kinoklip$/Name=Kinoklip Dev/' "$PKG/$APP_ID.desktop"
    else
        cat "$PKG/$APP_ID.desktop"
    fi
    # A dev build shows its commit too (1.9.0-2510c03), so builds of one version tell apart.
    echo "X-AppImage-Version=$FULL_VERSION${DEV_COMMIT:+-$DEV_COMMIT}"
} >"$APPDIR/$APP_ID.desktop"
chmod 644 "$APPDIR/$APP_ID.desktop"
cp "$APPDIR/$APP_ID.desktop" "$APPDIR/usr/share/applications/$APP_ID.desktop"
# AppStream: appimagetool looks for usr/share/metainfo/<desktop file stem>.appdata.xml. The
# release date is the commit date (reproducible), else today.
RELEASE_DATE="$(git -c safe.directory="*" -C "$ROOT" log -1 --format=%cs 2>/dev/null || date -u +%F)"
mkdir -p "$APPDIR/usr/share/metainfo"
sed -e "s/@VERSION@/$FULL_VERSION/" -e "s/@DATE@/$RELEASE_DATE/" "$PKG/$APP_ID.metainfo.xml" \
    >"$APPDIR/usr/share/metainfo/$APP_ID.appdata.xml"
if command -v appstreamcli >/dev/null; then
    appstreamcli validate --no-net --explain "$APPDIR/usr/share/metainfo/$APP_ID.appdata.xml" >&2 ||
        die "AppStream metadata does not validate (packaging/linux/$APP_ID.metainfo.xml)"
else
    log "appstreamcli not found: AppStream metadata not validated"
fi
if command -v desktop-file-validate >/dev/null; then
    desktop-file-validate "$APPDIR/$APP_ID.desktop" >&2 || die "desktop entry does not validate"
fi
# The icon: the owner's pixel art, one PNG per size (resources/icons/app, scripts/make_app_icons.py).
# A dev build uses kinoklip-dev-<size>.png (the owner's dev icon, kinoklip-dev.aseprite) where it
# exists, else the release icon.
icon() {
    local dev="$ROOT/resources/icons/app/kinoklip-dev-$1.png"
    if [ -n "$DEV_COMMIT" ] && [ -f "$dev" ]; then echo "$dev"; else echo "$ROOT/resources/icons/app/kinoklip-$1.png"; fi
}
for size in 16 24 32 48 64 128 256 512; do
    install -D -m 644 "$(icon $size)" "$APPDIR/usr/share/icons/hicolor/${size}x${size}/apps/$APP_ID.png"
done
install -m 644 "$(icon 256)" "$APPDIR/$APP_ID.png"
# a real file, not a symlink: some AppImage extractors (7-Zip in Gear Lever) handle links badly
install -m 644 "$(icon 256)" "$APPDIR/.DirIcon"
install -m 644 "$PKG/$APP_ID.xml" "$APPDIR/usr/share/mime/packages/$APP_ID.xml"
echo "$VERSION" >"$APPDIR/usr/share/doc/kinoklip/version"
# Kinoklip's own licence, the name/logo terms, the asset list and where the source code is.
for doc in license.md logo-license.md attribution.md; do
    install -m 644 "$ROOT/$doc" "$APPDIR/usr/share/doc/kinoklip/$doc"
done
cat >"$APPDIR/usr/share/doc/kinoklip/source-code.txt" <<SOURCE
Kinoklip ${TAG#v} is free software under the GNU General Public License, version 3 or later
(license.md). The Kinoklip name, icon and logos are not under the GPL (logo-license.md).
Complete corresponding source code: https://github.com/SeniorMatt/kinoklip (tag $TAG).
Third-party libraries in this bundle and their licences: bundled-libraries.txt, licenses/.
SOURCE

# Licence notices of the bundled libraries (GPL/LGPL compliance; see docs/archive/plan.md section 8).
# Debian/Ubuntu: dpkg -S + /usr/share/doc/<pkg>/copyright. Arch: pacman -Qo + /usr/share/licenses/<pkg>/
# (Arch packages put the licence text there unless it is a common licence, whose text is in
# /usr/share/licenses/spdx/ -- copied for every SPDX id a bundled package names).
log "collecting licence files"
LICENSES="$APPDIR/usr/share/doc/kinoklip/licenses"
# RNNoise is compiled into the program (third_party/rnnoise, BSD-3-Clause): keep its notice.
mkdir -p "$LICENSES/rnnoise"
install -m 644 "$ROOT/third_party/rnnoise/COPYING" "$LICENSES/rnnoise/COPYING"
pkg_of() {
    local f
    f="$(readlink -f "$1")"
    # Microsoft's ONNX Runtime release unpacked to /opt/onnxruntime (the ARM64 workflow; Debian's
    # own package loads onnx twice and aborts at start): not a distribution package.
    case "$f" in /opt/onnxruntime/*) echo onnxruntime-official; return 0 ;; esac
    if command -v pacman >/dev/null; then
        pacman -Qqo "$f" 2>/dev/null | head -n1
    elif command -v dpkg >/dev/null; then
        dpkg -S "$f" 2>/dev/null | head -n1 | cut -d: -f1 || true
    fi
}
pkg_version() {
    if [ "$1" = onnxruntime-official ]; then
        cat /opt/onnxruntime/VERSION_NUMBER
    elif command -v pacman >/dev/null; then
        pacman -Q "$1" 2>/dev/null | cut -d' ' -f2
    else
        dpkg-query -W -f='${Version}' "$1" 2>/dev/null
    fi
}
# Real licence files of one package into $LICENSES/<pkg>/. The package's own files come first
# (cp -L; dangling links, e.g. xz's links into /usr/share/doc that the container image does not
# extract, are skipped one by one and empty files are dropped). Only when that leaves no
# non-empty file, the SPDX text of each licence id the package names is used: from
# /usr/share/licenses/spdx, /usr/share/common-licenses or packaging/linux/licenses/<id>.txt.
# Debian-style copyright files are used as they are. A package left without text is fatal.
have_text() { [ -d "$1" ] && [ -n "$(find "$1" -type f -size +0 -print -quit)" ]; }
copy_licence() {
    local pkg="$1" id src f
    if command -v pacman >/dev/null; then
        if [ -d "/usr/share/licenses/$pkg" ]; then
            mkdir -p "$LICENSES/$pkg"
            for f in "/usr/share/licenses/$pkg"/*; do
                [ -e "$f" ] && [ -s "$f" ] || [ -d "$f" ] || continue
                cp -rL "$f" "$LICENSES/$pkg/" 2>/dev/null || true
            done
            find "$LICENSES/$pkg" -type f -empty -delete
        fi
        # SPDX ids from the package metadata, e.g. "LGPL-2.1-or-later  GPL-3.0-only"
        for id in $(LC_ALL=C pacman -Qi "$pkg" 2>/dev/null | sed -n 's/^Licenses *: //p'); do
            id="${id//[()]/}"
            case "$id" in AND | OR | WITH | '' | LicenseRef-*) continue ;; esac
            if [ -f "/usr/share/licenses/spdx/$id.txt" ]; then
                mkdir -p "$LICENSES/spdx"
                cp -L "/usr/share/licenses/spdx/$id.txt" "$LICENSES/spdx/"
            fi
            have_text "$LICENSES/$pkg" && continue
            src=""
            for f in "/usr/share/licenses/spdx/$id.txt" "/usr/share/common-licenses/$id" \
                "$ROOT/packaging/linux/licenses/$id.txt"; do
                [ -s "$f" ] && { src="$f"; break; }
            done
            if [ -n "$src" ]; then
                mkdir -p "$LICENSES/$pkg"
                cp -L "$src" "$LICENSES/$pkg/$id.txt"
                log "note: $pkg ships no licence text, using the $id text"
            fi
        done
        # Upstream texts kept in the repository for packages whose metadata uses old, non-SPDX ids
        if ! have_text "$LICENSES/$pkg" && have_text "$ROOT/packaging/linux/licenses/$pkg"; then
            mkdir -p "$LICENSES/$pkg"
            cp -L "$ROOT/packaging/linux/licenses/$pkg"/* "$LICENSES/$pkg/"
            log "note: $pkg ships no licence text, using packaging/linux/licenses/$pkg"
        fi
        have_text "$LICENSES/$pkg" || die "no licence text for bundled package $pkg"
    elif [ "$pkg" = onnxruntime-official ]; then
        mkdir -p "$LICENSES/$pkg"
        cp -L /opt/onnxruntime/LICENSE /opt/onnxruntime/ThirdPartyNotices.txt "$LICENSES/$pkg/"
    elif [ -s "/usr/share/doc/$pkg/copyright" ]; then
        cp -L "/usr/share/doc/$pkg/copyright" "$LICENSES/$pkg.copyright"
    else
        die "no licence text for bundled package $pkg"
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
        if [ -f "$APPDIR/usr/share/kinoklip/fonts/NotoColorEmoji.ttf" ]; then
            echo "NotoColorEmoji.ttf $EMOJI_FONT"
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
"$ROOT/scripts/check_licenses.sh" "$APPDIR" || die "licence check failed"

# ---------------------------------------------------------------------------------------
# 6. AppImage (official appimagetool + type 2 static runtime, pinned and checksummed)
# ---------------------------------------------------------------------------------------
# Update with the release page's sha256 digests (gh api repos/AppImage/appimagetool/releases).
APPIMAGETOOL_VERSION=1.9.1
APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 # x86_64
APPIMAGETOOL_SHA256_AARCH64=f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158
RUNTIME_VERSION=20251108 # github.com/AppImage/type2-runtime: static, needs no libfuse on the host
RUNTIME_SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d # x86_64
RUNTIME_SHA256_AARCH64=00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444
if [ "$ARCH" = aarch64 ]; then
    APPIMAGETOOL_SHA256="$APPIMAGETOOL_SHA256_AARCH64"
    RUNTIME_SHA256="$RUNTIME_SHA256_AARCH64"
fi
# gh-releases-zsync: Gear Lever and AppImageUpdate look up the GitHub release and compare the
# .zsync's SHA-1 with the installed file. "latest-all" instead of "latest": every Kinoklip
# release is a GitHub pre-release, and "latest" (GitHub's /releases/latest) never returns
# pre-releases; "latest-all" takes the newest release, pre-release or not.
UPDATE_INFO="${KK_UPDATE_INFO-gh-releases-zsync|SeniorMatt|kinoklip|latest-all|Kinoklip-*$ARCH.AppImage.zsync}"
APPIMAGE="$DIST/Kinoklip-$ARCH.AppImage"
if [ -n "$DEV_COMMIT" ]; then
    # The dev channel: its own file name, so the release pattern (Kinoklip-*) never matches a dev
    # file and a dev copy only updates to newer dev builds (scripts/publish_dev.sh).
    UPDATE_INFO="${KK_UPDATE_INFO-gh-releases-zsync|SeniorMatt|kinoklip|latest-all|KinoklipDev-*$ARCH.AppImage.zsync}"
    APPIMAGE="$DIST/KinoklipDev-$ARCH.AppImage"
fi
TOOLS="${KK_TOOLS_CACHE:-$ROOT/.cache/appimage-tools}"

# fetch <url> <sha256> <dest>: download once, verify every time.
fetch() {
    if [ ! -f "$3" ] || ! echo "$2  $3" | sha256sum -c --status; then
        command -v curl >/dev/null || die "curl is needed to download $1"
        log "downloading $1"
        mkdir -p "$(dirname "$3")"
        curl -fsSL --retry 3 -o "$3.part" "$1" || die "download failed: $1"
        echo "$2  $3.part" | sha256sum -c --status || die "checksum mismatch: $1"
        mv "$3.part" "$3"
    fi
    chmod 755 "$3"
}

rm -f "$APPIMAGE" "$APPIMAGE.zsync"
if [ "$DO_APPIMAGE" = 1 ]; then
    TOOL="$TOOLS/appimagetool-$APPIMAGETOOL_VERSION-$ARCH.AppImage"
    RUNTIME="$TOOLS/runtime-$RUNTIME_VERSION-$ARCH"
    fetch "https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL_VERSION/appimagetool-$ARCH.AppImage" \
        "$APPIMAGETOOL_SHA256" "$TOOL"
    fetch "https://github.com/AppImage/type2-runtime/releases/download/$RUNTIME_VERSION/runtime-$ARCH" \
        "$RUNTIME_SHA256" "$RUNTIME"
    log "making $APPIMAGE"
    # APPIMAGE_EXTRACT_AND_RUN: appimagetool is itself an AppImage and containers have no FUSE.
    # SOURCE_DATE_EPOCH (honoured by mksquashfs): file times from the commit, so unchanged
    # content gives an identical squashfs. -b 1M and zstd level 19: 101 MB instead of 110 MB
    # (1M blocks, appimagetool's default level); decompression is as fast at any level.
    # appimagetool writes the .zsync into the current directory (with -u and its bundled zsyncmake).
    # --no-appstream: the metadata was validated offline above; appimagetool's own check also
    # fetches every URL and fails while the GitHub repository is private.
    export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -c safe.directory="*" -C "$ROOT" log -1 --format=%ct 2>/dev/null || date +%s)}"
    UPDATE_ARGS=()
    [ -n "$UPDATE_INFO" ] && UPDATE_ARGS=(-u "$UPDATE_INFO")
    (
        cd "$DIST"
        ARCH="$ARCH" APPIMAGE_EXTRACT_AND_RUN=1 "$TOOL" --no-appstream \
            --runtime-file "$RUNTIME" --comp zstd --mksquashfs-opt -b --mksquashfs-opt 1M \
            --mksquashfs-opt -Xcompression-level --mksquashfs-opt "${KK_ZSTD_LEVEL:-19}" \
            "${UPDATE_ARGS[@]}" "$APPDIR" "$APPIMAGE" >&2
    ) || die "appimagetool failed"
    [ -x "$APPIMAGE" ] || die "appimagetool made no $APPIMAGE"
    if [ -n "$UPDATE_INFO" ] && [ ! -f "$APPIMAGE.zsync" ]; then
        die "no $APPIMAGE.zsync was written"
    fi
fi

# ---------------------------------------------------------------------------------------
# 7. Legacy outputs, only on request: archive (--tarball) and self-extracting .run (--run).
#    Not release artefacts any more (the AppImage replaced them in 0.4.1).
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

# stale outputs of earlier runs must not end up in a release
rm -f "$DIST"/kinoklip-"$ARCH".tar.* "$DIST/Kinoklip-$ARCH.run"
[ "$DO_RUN" = 0 ] || [ "$ARCH" = x86_64 ] || die "--run (the legacy self-extracting file) is x86_64 only"
if [ "$DO_TARBALL" = 1 ] || [ "$DO_RUN" = 1 ]; then
    TARBALL="$DIST/kinoklip-$ARCH.tar.$EXT"
    log "compressing ($COMP) -> $TARBALL"
    # Reproducible archive: sorted entries, fixed owner and mtime, so identical contents give
    # an identical hash (and the .run reuses its existing cache directory).
    tar -C "$DIST" --sort=name --owner=0 --group=0 --numeric-owner \
        --mtime='2026-01-01 00:00:00Z' -cf - "kinoklip-$ARCH" | "${COMPRESS[@]}" >"$TARBALL"
fi

if [ "$DO_RUN" = 1 ]; then
    RUN="$DIST/Kinoklip-$ARCH.run"
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
# 8. Report
# ---------------------------------------------------------------------------------------
log "AppDir: $(du -sh "$APPDIR" | cut -f1) in $APPDIR"
log "  usr/lib: $(ls "$APPDIR/usr/lib" | grep -c '\.so') libs, usr/lib/qt: $(ls "$APPDIR/usr/lib/qt" | grep -c '\.so') libs," \
    "fallback: $(ls "$APPDIR/usr/lib/fallback" | grep -c '\.so') libs, plugins: ${#PLUGIN_FILES[@]} Qt + ${#HEIF_PLUGINS[@]} libheif"
for f in "$APPIMAGE" "$APPIMAGE.zsync" "$DIST"/kinoklip-"$ARCH".tar.* "$DIST/Kinoklip-$ARCH.run"; do
    [ -f "$f" ] && log "$(du -h "$f" | cut -f1)	$f"
done
[ -z "$UPDATE_INFO" ] || [ "$DO_APPIMAGE" = 0 ] || log "update information: $UPDATE_INFO"
exit 0
