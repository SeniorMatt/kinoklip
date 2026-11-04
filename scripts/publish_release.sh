#!/bin/sh
# Publishes the AppImage in dist/ as a GitHub pre-release and points the readme's
# "Download here" link at it. Run from the repository root after packaging:
#   scripts/publish_release.sh v1.1.0-beta.1 "Kinoklip 1.1.0 Beta"
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
gh release create "$tag" --prerelease --title "$title" --notes-file "$notes" \
    dist/Kinoklip-x86_64.AppImage dist/Kinoklip-x86_64.AppImage.zsync dist/SHA256SUMS.txt

# Point the readme's download link at the new release.
sed -i "s#releases/download/v[^/]*/Kinoklip-x86_64.AppImage#releases/download/$tag/Kinoklip-x86_64.AppImage#" readme.md
git add readme.md
git commit -m "readme: download link to $title" \
    -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01X5N5x5ieBdJ1QWiroo1Sgc"
git push origin main
echo "Published $title: https://github.com/SeniorMatt/kinoklip/releases/tag/$tag"
