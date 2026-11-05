#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# check_distros.sh -- starts the AppImage in official container images of other distributions, so
# a library that a distribution lacks or names differently is found before a user finds it
# (GitHub issue 1). See packaging/linux/readme.md, "Checking other distributions".
#
# Usage: scripts/check_distros.sh [options] [AppImage]      (default: dist/Kinoklip-x86_64.AppImage)
#   --only <text>    only the rows whose "image set" contains this text (may repeat)
#   --keep-images    do not remove the images this run pulled
#   --logs <dir>     keep the per-row logs and screenshots there (default: a temporary directory)
# Environment: KK_DEBIAN_MIRROR  e.g. http://mirror.yandex.ru/debian, if deb.debian.org is slow here
#
# Each row is a fresh `podman run --rm` of an image with a package set installed, without weak
# dependencies: a "gnome" or "kde" shell's library set, or "bare", the image as it is (for the
# Fedora Atomic images, that is the desktop's real package set). The bundle
# is mounted read-only, as the AppImage's FUSE mount would be (containers have no FUSE). In the
# container, from a clean environment:
#   version   AppRun --version (bare: this is what must always work), before anything else is
#             installed; then the AppImage file itself with --appimage-extract-and-run
#   xcb       --screenshot edit out.png under Xvfb, must write the picture
#   wayland   the same on a headless weston with QT_QPA_PLATFORM=wayland (no X server involved)
#   norun     --version without XDG_RUNTIME_DIR and with a read-only home (links go under TMPDIR)
#   rotmp     the same with a read-only TMPDIR too: nowhere is left for the launcher's links
#   noexec    --version with XDG_RUNTIME_DIR and TMPDIR on a noexec mount
# Prints a table and exits non-zero if a row failed. Not covered: real GPUs, real desktop sessions,
# FUSE, SELinux. Run outside kk-arch (it needs podman), after packaging.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_FILE=""
KEEP=0
LOGS=""
ONLY=()
while [ $# -gt 0 ]; do
    case "$1" in
    --only)
        ONLY+=("$2")
        shift
        ;;
    --keep-images) KEEP=1 ;;
    --logs)
        LOGS="$2"
        shift
        ;;
    -h | --help)
        sed -n '3,24p' "$0"
        exit 0
        ;;
    -*)
        echo "unknown option: $1" >&2
        exit 2
        ;;
    *) IMAGE_FILE="$1" ;;
    esac
    shift
done
IMAGE_FILE="$(readlink -f "${IMAGE_FILE:-$ROOT/dist/Kinoklip-x86_64.AppImage}")"
[ -x "$IMAGE_FILE" ] || {
    echo "no AppImage at $IMAGE_FILE" >&2
    exit 2
}
command -v podman >/dev/null || {
    echo "needs podman" >&2
    exit 2
}

# image                            package manager  set
ROWS=(
    "docker.io/library/fedora:44 dnf gnome"
    "docker.io/library/fedora:44 dnf kde"
    "docker.io/library/fedora:44 dnf bare"
    "docker.io/library/fedora:43 dnf gnome"
    "docker.io/library/ubuntu:24.04 apt gnome"
    "docker.io/library/ubuntu:24.04 apt kde"
    "docker.io/library/ubuntu:24.04 apt bare"
    "docker.io/library/ubuntu:26.04 apt gnome"
    "docker.io/library/debian:13 apt gnome"
    "docker.io/library/debian:13 apt kde"
    "docker.io/library/debian:13 apt bare"
    "docker.io/opensuse/tumbleweed zypper gnome"
    "docker.io/opensuse/tumbleweed zypper kde"
    "docker.io/opensuse/tumbleweed zypper bare"
    "docker.io/library/archlinux pacman gnome"
    "docker.io/library/archlinux pacman kde"
    "docker.io/library/archlinux pacman bare"
    "quay.io/fedora/fedora-silverblue:44 dnf bare"
    "quay.io/fedora/fedora-kinoite:44 dnf bare"
)

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
[ -n "$LOGS" ] || LOGS="$WORK/logs"
mkdir -p "$LOGS"
LOGS="$(readlink -f "$LOGS")"
(cd "$WORK" && "$IMAGE_FILE" --appimage-extract >/dev/null) || {
    echo "cannot unpack $IMAGE_FILE" >&2
    exit 2
}
chmod -R a+rX "$WORK/squashfs-root"
cp "$IMAGE_FILE" "$WORK/Kinoklip.AppImage"

# ---- runs in the container ------------------------------------------------------------------
cat >"$WORK/inner.sh" <<'INNER'
#!/bin/sh
pm=$1
set=$2
B=/k/squashfs-root
OUT=/logs
export DEBIAN_FRONTEND=noninteractive
install_pkgs() {
    case $pm in
    dnf) dnf -y -q install --setopt=install_weak_deps=False "$@" ;;
    apt) apt-get -y -qq --no-install-recommends install "$@" ;;
    zypper) zypper -q -n in --no-recommends "$@" ;;
    pacman) pacman -S --noconfirm --needed "$@" ;;
    esac
}
if [ -n "${KK_DEBIAN_MIRROR:-}" ] && [ -f /etc/apt/sources.list.d/debian.sources ]; then
    sed -i "s|http://deb.debian.org/debian\$|$KK_DEBIAN_MIRROR|; s|URIs: http://deb.debian.org/debian |URIs: $KK_DEBIAN_MIRROR |" /etc/apt/sources.list.d/debian.sources
fi
case $pm in
apt) apt-get -qq update ;;
pacman) pacman -Sy --noconfirm --needed archlinux-keyring >/dev/null 2>&1 && pacman -Syu --noconfirm ;;
esac >/dev/null 2>&1
if [ "$set" != bare ]; then
    case "$pm:$set" in
    *:gnome) P="gnome-shell nautilus" ;;
    zypper:kde) P="plasma6-workspace dolphin" ;;
    *:kde) P="plasma-workspace dolphin" ;;
    esac
    # shellcheck disable=SC2086
    install_pkgs $P >"$OUT/install.log" 2>&1 || echo "RESULT install=FAILED"
    ldconfig 2>/dev/null
fi

# a clean environment, as a desktop launcher would give
BASE_ENV="env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin LANG=C.UTF-8 APPIMAGE=/k/Kinoklip.AppImage KINOKLIP_NO_FAILURE_DIALOG=1"
CLEAN="$BASE_ENV HOME=/tmp/h XDG_RUNTIME_DIR=/tmp/run"
mkdir -p /tmp/h /tmp/run
chmod 700 /tmp/run
loader_error() { # the first line that looks like the cause
    grep -E 'error while loading|symbol lookup|not found|undefined symbol|Qt platform plugin|could not start|cannot' "$1" | head -n 1
}
result() { # <name> <exit status> <log>
    if [ "$2" = 0 ]; then
        echo "RESULT $1=pass"
    else
        echo "RESULT $1=FAIL($2)"
        echo "ERROR $1: $(loader_error "$3")"
    fi
}

. /etc/os-release
echo "OS $PRETTY_NAME, $(getconf GNU_LIBC_VERSION 2>/dev/null)"
timeout 60 $CLEAN "$B/AppRun" --version >"$OUT/version.log" 2>&1
st=$?
[ $st = 0 ] && ! grep -q '^Kinoklip ' "$OUT/version.log" && st=1
result version "$st" "$OUT/version.log"
grep -E 'using bundled fallbacks' "$OUT/version.log" | sed 's/.*using bundled fallbacks:/FALLBACKS/; s/ (links in.*//'
# the AppImage file itself: the runtime, extracted here since containers have no FUSE
cp /k/Kinoklip.AppImage /tmp/k.AppImage
(cd /tmp && timeout 180 $CLEAN /tmp/k.AppImage --appimage-extract-and-run --version) >"$OUT/appimage.log" 2>&1
result appimage $? "$OUT/appimage.log"
rm -f /tmp/k.AppImage

# no writable place for the launcher's links but a new directory under TMPDIR, then none at all
timeout 60 $BASE_ENV HOME=/kkro TMPDIR=/tmp "$B/AppRun" --version >"$OUT/norun.log" 2>&1
result norun $? "$OUT/norun.log"
timeout 60 $BASE_ENV HOME=/kkro TMPDIR=/kkro "$B/AppRun" --version >"$OUT/rotmp.log" 2>&1
result rotmp $? "$OUT/rotmp.log"
timeout 60 $BASE_ENV HOME=/tmp/h TMPDIR=/kknoexec XDG_RUNTIME_DIR=/kknoexec "$B/AppRun" --version >"$OUT/noexec.log" 2>&1
result noexec $? "$OUT/noexec.log"

if [ "$set" = bare ]; then
    # no display and no driver at all: still draws, in software
    timeout 120 $CLEAN QT_QPA_PLATFORM=offscreen "$B/AppRun" --screenshot edit /tmp/offscreen.png >"$OUT/offscreen.log" 2>&1
    st=$?
    [ $st = 0 ] && [ ! -s /tmp/offscreen.png ] && st=1
    result offscreen "$st" "$OUT/offscreen.log"
    [ -s /tmp/offscreen.png ] && cp /tmp/offscreen.png "$OUT/offscreen.png"
    exit 0
fi

# ---- GUI on X11 and on Wayland. Only now the servers get installed, so they do not hide a
# missing library above. ----
case $pm in
dnf) install_pkgs xorg-x11-server-Xvfb weston ;;
apt) install_pkgs xvfb weston ;;
zypper) install_pkgs xorg-x11-server-Xvfb weston ;;
pacman) install_pkgs xorg-server-xvfb weston ;;
esac >"$OUT/install-servers.log" 2>&1 || echo "NOTE servers: install failed"
ldconfig 2>/dev/null

if command -v Xvfb >/dev/null; then
    Xvfb :77 -screen 0 1920x1080x24 >/dev/null 2>&1 &
    xp=$!
    sleep 2
    timeout 120 $CLEAN DISPLAY=:77 "$B/AppRun" --screenshot edit /tmp/xcb.png >"$OUT/xcb.log" 2>&1
    st=$?
    [ $st = 0 ] && [ ! -s /tmp/xcb.png ] && st=1
    result xcb "$st" "$OUT/xcb.log"
    [ -s /tmp/xcb.png ] && cp /tmp/xcb.png "$OUT/xcb.png"
    kill "$xp" 2>/dev/null
else
    echo "RESULT xcb=skipped"
fi

if command -v weston >/dev/null; then
    XDG_RUNTIME_DIR=/tmp/run weston --backend=headless --socket=kkwl --width=1920 --height=1080 \
        >"$OUT/weston.log" 2>&1 &
    wp=$!
    sleep 3
    if kill -0 "$wp" 2>/dev/null; then
        timeout 120 $CLEAN WAYLAND_DISPLAY=kkwl QT_QPA_PLATFORM=wayland "$B/AppRun" --screenshot edit /tmp/wl.png >"$OUT/wayland.log" 2>&1
        st=$?
        [ $st = 0 ] && [ ! -s /tmp/wl.png ] && st=1
        result wayland "$st" "$OUT/wayland.log"
        [ -s /tmp/wl.png ] && cp /tmp/wl.png "$OUT/wayland.png"
        kill "$wp" 2>/dev/null
    else
        echo "RESULT wayland=skipped"
    fi
else
    echo "RESULT wayland=skipped"
fi
INNER
chmod a+rx "$WORK/inner.sh"

# ---- the rows --------------------------------------------------------------------------------
PULLED=()
fails=0
printf '%-38s %-6s %-8s %-8s %-8s %-8s %-8s %-8s %-8s %s\n' image set version appimage norun rotmp noexec xcb wayland 'first error' | tee "$LOGS/table.txt"
for row in "${ROWS[@]}"; do
    read -r image pm set <<<"$row"
    if [ "${#ONLY[@]}" -gt 0 ]; then
        hit=0
        for o in "${ONLY[@]}"; do [[ "$image $set" == *"$o"* ]] && hit=1; done
        [ "$hit" = 1 ] || continue
    fi
    avail="$(df --output=avail -BG "$HOME" | tail -n 1 | tr -dc 0-9)"
    if [ "$avail" -lt 15 ]; then
        echo "stopping: only ${avail} GB free" >&2
        break
    fi
    if ! podman image exists "$image"; then
        podman pull -q "$image" >/dev/null || {
            printf '%-38s %-6s cannot pull\n' "${image#docker.io/}" "$set" | tee -a "$LOGS/table.txt"
            continue
        }
        PULLED+=("$image")
    fi
    log="$LOGS/$(printf '%s-%s' "${image#docker.io/}" "$set" | tr '/:' '__')"
    mkdir -p "$log"
    chmod a+rwx "$log"
    nice -n 15 podman run --rm -e KK_DEBIAN_MIRROR="${KK_DEBIAN_MIRROR:-}" \
        -v "$WORK:/k:ro" -v "$WORK/inner.sh:/inner.sh:ro" -v "$log:/logs" \
        --tmpfs /kkro:ro --tmpfs /kknoexec:rw,noexec,mode=0700 \
        "$image" sh /inner.sh "$pm" "$set" >"$log/result.txt" 2>&1
    val() { sed -n "s/^RESULT $1=//p" "$log/result.txt" | head -n 1; }
    wl="$(val wayland)"
    xc="$(val xcb)"
    [ "$set" = bare ] && xc="offscrn:$(val offscreen)" && wl="-"
    err="$(sed -n 's/^ERROR //p' "$log/result.txt" | head -n 1)"
    grep -q 'RESULT install=FAILED' "$log/result.txt" && err="package install failed; $err"
    printf '%-38s %-6s %-8s %-8s %-8s %-8s %-8s %-8s %-8s %s\n' "${image#docker.io/}" "$set" \
        "$(val version)" "$(val appimage)" "$(val norun)" "$(val rotmp)" "$(val noexec)" "$xc" "$wl" "$err" |
        tee -a "$LOGS/table.txt"
    grep -q 'FAIL' "$log/result.txt" && fails=$((fails + 1))
done
if [ "$KEEP" = 0 ] && [ "${#PULLED[@]}" -gt 0 ]; then
    podman rmi "${PULLED[@]}" >/dev/null 2>&1
fi
[ "$fails" = 0 ] && echo "OK: every row passed" || echo "FAILED: $fails row(s)"
[ "$fails" = 0 ]
