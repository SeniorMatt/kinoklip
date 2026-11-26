#!/usr/bin/env bash
# Builds G'MIC-OFX (openfx-gmic, Natron / GreycLab; G'MIC is CeCILL v2.0 or CeCILL-C, the GPL build
# adds the GPL-2.0-or-later parts of openfx-supportext) from source, for the pack test of
# tests/ofx/test_ofx.cpp and for trying it in Kinoklip (docs/plugins/openfx.md).
#
#   scripts/build_gmic_ofx.sh <output dir> [jobs]
#
# Leaves <output dir>/GMIC.ofx.bundle. Then
#   KK_OFX_GMIC_DIR=<output dir> ctest -R test_ofx        # runs the pack test
#   cp -r <output dir>/GMIC.ofx.bundle ~/.local/share/Kinoklip/ofx/
# Needs: git, make, curl, a C++17 compiler, libpng, libjpeg, libtiff, zlib headers (and OpenGL
# headers). The build downloads the G'MIC release tarball and pocketfft (no FFTW: BSD-3-Clause FFT).
# It takes a long time and about 4 GB of memory for the one big file (gmic.cpp), so use few jobs.
set -euo pipefail

OUT="${1:?usage: build_gmic_ofx.sh <output dir> [jobs]}"
JOBS="${2:-1}"
# A fixed commit, so a CI run is repeatable (openfx-gmic master, 2026-11-27).
COMMIT=5484df09b6a1071b4e786a6247c1ae2f8511e06b
SRC="$(mktemp -d)"
trap 'rm -rf "$SRC"' EXIT

git clone -q https://github.com/NatronGitHub/openfx-gmic.git "$SRC/openfx-gmic"
git -C "$SRC/openfx-gmic" checkout -q "$COMMIT"
git -C "$SRC/openfx-gmic" submodule update -q --init --depth 1
make -C "$SRC/openfx-gmic" -j"$JOBS" CONFIG=release OPENMP=1 CXXSTD=c++17 GMIC_STATIC_DEPS=0 LICENSE=GPL FFT=pocketfft
BUNDLE="$(find "$SRC/openfx-gmic/GMIC_OFX" -maxdepth 3 -name 'GMIC.ofx.bundle' -type d | head -1)"
[ -n "$BUNDLE" ] || { echo "build_gmic_ofx: no GMIC.ofx.bundle was made" >&2; exit 1; }
mkdir -p "$OUT"
rm -rf "$OUT/GMIC.ofx.bundle"
cp -r "$BUNDLE" "$OUT/"
du -sh "$OUT/GMIC.ofx.bundle"
