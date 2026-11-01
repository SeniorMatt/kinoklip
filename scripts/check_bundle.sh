#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# check_bundle.sh -- verifies that every ELF file of the Linux bundle resolves all of its
# libraries either inside the bundle or to a library the exclude list leaves to the host.
#
# Usage: scripts/check_bundle.sh [bundle dir]     (default: dist/kinoklip-x86_64)
#
# Two passes, both with ldd (the dynamic loader's real search order):
#   1. normal:   LD_LIBRARY_PATH=usr/lib/qt, like AppRun in bundled-Qt mode
#   2. fallback: also every usr/lib/fallback library first, simulating a host that lacks all
#                of them (so their own dependencies are proven to resolve, too)
# Fails on "not found" and on any library loaded from the host that is not in the exclude
# list (it would be missing or a different version on the user's machine).
# Finally prints the host libraries the bundle relies on: the list the target must provide.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
B="$(readlink -f "${1:-$ROOT/dist/kinoklip-x86_64}")"
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

# all ELF files of the bundle (binary, libraries, plugins); fallback-active only holds links
mapfile -t ELFS < <(find "$B/usr" -path "$B/usr/lib/fallback-active" -prune -o -type f -print |
    while read -r f; do file -b "$f" | grep -q '^ELF' && echo "$f"; done)

errors=0
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

echo "checking ${#ELFS[@]} ELF files in $B"
check_pass normal "$B/usr/lib/qt"
check_pass fallback "$B/usr/lib/fallback:$B/usr/lib/qt"

echo "host libraries the bundle needs (from packaging/linux/excludelist.txt):"
printf '%s\n' "${!HOST_USED[@]}" | sort | column -c 100
if [ "$errors" -gt 0 ]; then
    echo "FAILED: $errors problem(s)"
    exit 1
fi
echo "OK: all libraries resolve inside the bundle or to excluded host libraries"
echo "bundle size: $(du -sh "$B" | cut -f1)"
