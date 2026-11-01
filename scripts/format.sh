#!/usr/bin/env bash
# Formats all C++ sources in place with the repo's .clang-format.
# Usage: scripts/format.sh            (whole tree)
#        scripts/format.sh --check    (CI mode: fail if anything would change)
set -euo pipefail
cd "$(dirname "$0")/.."
mapfile -t files < <(git ls-files -co --exclude-standard -- 'src/*.cpp' 'src/*.h' 'tests/*.cpp' 'tests/*.h')
[[ ${#files[@]} -eq 0 ]] && exit 0
if [[ "${1:-}" == "--check" ]]; then
    clang-format --dry-run --Werror "${files[@]}"
else
    clang-format -i "${files[@]}"
fi
