#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Kinoklip @VERSION@ -- self-extracting single-file launcher (x86_64 Linux).
#
# This file is a POSIX shell script followed by a compressed tar archive of the portable
# bundle. Started (double-click in Dolphin, or ./Kinoklip-x86_64.run), it unpacks the
# archive once into ${XDG_CACHE_HOME:-~/.cache}/kinoklip/@VERSION@-<hash>/ and then runs the
# bundle's AppRun with all arguments. Later starts skip the unpacking.
#
#   --extract-only            unpack (if needed), print the bundle directory, exit
#   --uninstall-cache         delete every unpacked Kinoklip version from the cache
#   --install-desktop-entry   add Kinoklip (this .run file) to the application menu
#   --uninstall-desktop-entry remove it again
#   anything else             passed to kinoklip (e.g. a .kkproj project file)
#
# Template: scripts/package_linux.sh fills in the hash, the payload offset (a fixed-width
# number so the header length does not change) and the compression.
# The script must never read past its last line: the binary payload follows it directly.

KK_HASH=@HASH@
KK_VERSION=@VERSION@
KK_COMPRESSION=@COMPRESSION@
KK_OFFSET=@@@@OFFSET@@

kk_fail() {
    printf 'Kinoklip: %s\n' "$*" >&2
    # Started from a file manager there is no terminal; show the error graphically if we can.
    if [ ! -t 2 ]; then
        if command -v kdialog >/dev/null 2>&1; then
            kdialog --title Kinoklip --error "$*" >/dev/null 2>&1
        elif command -v notify-send >/dev/null 2>&1; then
            notify-send Kinoklip "$*" >/dev/null 2>&1
        fi
    fi
    exit 1
}

KK_SELF="$(readlink -f "$0")" || kk_fail "cannot locate myself ($0)"
KK_CACHE_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/kinoklip"
KK_DIR="$KK_CACHE_ROOT/$KK_VERSION-$KK_HASH"
KK_MARKER="$KK_DIR/.kinoklip-complete"
KK_BUNDLE="$KK_DIR/kinoklip-x86_64"

# Decompressor for the payload. zstd is part of every Arch-based system (systemd needs
# it); bsdtar (libarchive) is the fallback that decodes zstd/xz/gzip by itself.
kk_unpack() {
    _dest="$1"
    case "$KK_COMPRESSION" in
    zstd) _dec="zstd -dc" ;;
    xz) _dec="xz -dc" ;;
    gzip) _dec="gzip -dc" ;;
    *) kk_fail "unknown payload compression $KK_COMPRESSION" ;;
    esac
    _off="$(expr "$KK_OFFSET" + 0)" # strip the zero padding
    if command -v "${_dec%% *}" >/dev/null 2>&1 && command -v tar >/dev/null 2>&1; then
        tail -c +"$_off" "$KK_SELF" | $_dec | tar -x -f - -C "$_dest"
    elif command -v bsdtar >/dev/null 2>&1; then
        tail -c +"$_off" "$KK_SELF" | bsdtar -x -f - -C "$_dest"
    else
        kk_fail "need '${_dec%% *}' and 'tar' (or 'bsdtar') to unpack"
    fi
}

# Unpack once. The marker file is written last, and the directory is renamed into place
# only when complete, so an interrupted extraction (power loss, full disk) is redone.
kk_extract() {
    [ -f "$KK_MARKER" ] && return 0
    mkdir -p "$KK_CACHE_ROOT" || kk_fail "cannot create $KK_CACHE_ROOT"
    _tmp="$KK_CACHE_ROOT/.unpack-$KK_HASH-$$"
    rm -rf "$_tmp"
    mkdir -p "$_tmp" || kk_fail "cannot create $_tmp"
    if ! kk_unpack "$_tmp"; then
        rm -rf "$_tmp"
        kk_fail "unpacking to $KK_CACHE_ROOT failed (disk full?)"
    fi
    [ -x "$_tmp/kinoklip-x86_64/AppRun" ] || {
        rm -rf "$_tmp"
        kk_fail "the archive inside $KK_SELF is damaged"
    }
    date >"$_tmp/.kinoklip-complete"
    # another instance may have finished first; then keep its copy
    if [ -f "$KK_MARKER" ]; then
        rm -rf "$_tmp"
        return 0
    fi
    rm -rf "$KK_DIR" # leftovers of an older, incomplete attempt
    mv "$_tmp" "$KK_DIR" || kk_fail "cannot move the bundle into $KK_DIR"
}

case "${1:-}" in
--uninstall-cache)
    rm -rf "$KK_CACHE_ROOT" && echo "Removed $KK_CACHE_ROOT"
    exit $?
    ;;
--extract-only)
    kk_extract
    echo "$KK_BUNDLE"
    exit 0
    ;;
esac

kk_extract
KINOKLIP_RUN_PATH="$KK_SELF"
export KINOKLIP_RUN_PATH
exec "$KK_BUNDLE/AppRun" "$@"
exit 1
