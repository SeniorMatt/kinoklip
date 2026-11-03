#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerates the application icon PNGs from a PNG of resources/icons/app/kinoklip.aseprite.

The icon is pixel art that fits a 64 x 64 grid (a 32 px sprite doubled works too). Render it
without Aseprite and run this script on the result:

    scripts/aseprite_to_png.py resources/icons/app/kinoklip.aseprite icon.png --scale 2
    scripts/make_app_icons.py icon.png

(an Aseprite export at any integer scale works as well). Sizes that are multiples of 64 are
scaled with nearest neighbour (crisp pixels); the small sizes (16, 24, 32, 48) are
box-filtered, as a desktop shows them. Needs Pillow.
"""
import sys
from pathlib import Path

from PIL import Image

GRID = 64
SIZES = (16, 24, 32, 48, 64, 128, 256, 512)
OUT = Path(__file__).resolve().parent.parent / "resources" / "icons" / "app"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    src = Image.open(sys.argv[1]).convert("RGBA")
    if src.width != src.height or src.width % GRID:
        print(f"expected a square export at a multiple of {GRID} px, got {src.size}")
        return 1
    art = src.resize((GRID, GRID), Image.NEAREST)
    if art.resize(src.size, Image.NEAREST).tobytes() != src.tobytes():
        print(f"the export is not on a {GRID} x {GRID} pixel grid")
        return 1
    for size in SIZES:
        method = Image.NEAREST if size % GRID == 0 else Image.BOX
        art.resize((size, size), method).save(OUT / f"kinoklip-{size}.png", optimize=True)
        print(OUT / f"kinoklip-{size}.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
