#!/bin/sh
# Publishes the AppImage in dist/ as the latest GitHub release. (The readme's "Download here" link
# points at the releases page, so it needs no update.) Run from the repository root after packaging:
#   scripts/publish_release.sh v1.1.0-beta.1 "Kinoklip 1.1.0 Beta"
# Before it: scripts/check_leaks.sh and `ctest -L soak` pass (docs/contributing.md, "Memory leaks").
# Needs: gh (logged in), dist/Kinoklip-x86_64.AppImage(.zsync), dist/SHA256SUMS.txt and
# docs/releases/<tag>.md.
set -eu
tag="$1"
title="$2"
notes="docs/releases/$tag.md"
[ -f "$notes" ] || { echo "missing $notes" >&2; exit 1; }
for f in dist/Kinoklip-x86_64.AppImage dist/Kinoklip-x86_64.AppImage.zsync dist/SHA256SUMS.txt; do
    [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
done
(cd dist && sha256sum -c SHA256SUMS.txt)

# The tag may already exist locally (made at the commit the AppImage was built from).
if ! git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    git tag -a "$tag" -m "$title"
fi
echo "Tag $tag -> $(git rev-list -n1 "$tag")"
git push origin "$tag"
gh release create "$tag" --latest --title "$title" --notes-file "$notes" \
    dist/Kinoklip-x86_64.AppImage dist/Kinoklip-x86_64.AppImage.zsync dist/SHA256SUMS.txt

echo "Published $title: https://github.com/SeniorMatt/kinoklip/releases/tag/$tag"
