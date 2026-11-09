#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# check_licenses.sh -- scans an AppDir's third-party licence notices.
#
# Usage: scripts/check_licenses.sh [AppDir]   (default: dist/kinoklip-x86_64)
# Fails (exit 1) when usr/share/doc/kinoklip/licenses has an empty file or a dangling link, or
# when a package listed in bundled-libraries.txt has no non-empty licence text of its own
# (licenses/<pkg>/ with a file, or licenses/<pkg>.copyright). Read-only.
set -u
APPDIR="${1:-dist/kinoklip-x86_64}"
DOC="$APPDIR/usr/share/doc/kinoklip"
LIC="$DOC/licenses"
[ -d "$LIC" ] || { echo "check_licenses: no $LIC" >&2; exit 1; }
bad=0
while IFS= read -r f; do
    echo "empty licence file: ${f#"$LIC"/}"; bad=1
done < <(find "$LIC" -type f -empty)
while IFS= read -r f; do
    echo "dangling link: ${f#"$LIC"/}"; bad=1
done < <(find "$LIC" -xtype l)
if [ -f "$DOC/bundled-libraries.txt" ]; then
    for pkg in $(awk 'NR > 4 && $2 != "?" { print $2 }' "$DOC/bundled-libraries.txt" | sort -u); do
        if [ -d "$LIC/$pkg" ] && [ -n "$(find -L "$LIC/$pkg" -type f -size +0 -print -quit)" ]; then continue; fi
        [ -s "$LIC/$pkg.copyright" ] && continue
        echo "no licence text for package: $pkg"; bad=1
    done
fi
[ "$bad" = 0 ] && echo "check_licenses: OK ($LIC)"
exit "$bad"
