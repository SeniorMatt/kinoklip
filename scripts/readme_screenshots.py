#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes the screenshots of the readme's Screenshots section (docs/images/readme/).

One picture per page (Media, Cut, Edit, Compositing, Color, Audio, Deliver) and a few feature
shots, taken from the real app with its default look (Breeze widget style, Kinoklip colour
scheme) at 1920x1080, then scaled to 1600 px wide and stored as palette PNGs.

Reuses the user guide's tools (scripts/guide_screenshots.py): its generated demo media (plus
host.mp4, the presenter with a speech-like sound track, made here), the demo projects of
tests/timeline/test_guidedemo.cpp (readme-edit, readme-compositing and readme-color.kkproj)
and `kinoklip --screenshot <target> <png> <project>`.

Usage (from the repository root, after building the app and the tests into build/):
  scripts/readme_screenshots.py [--app build/kinoklip] [--tests build] [--work build/readme]
                                [--only <name>] [--keep] [--delay 8000]

Runs under xvfb-run (1920x1080x24) with a clean environment and a temporary HOME, so the
owner's settings and desktop are never touched. Needs ffmpeg, xvfb-run and Pillow.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import guide_screenshots as guide  # noqa: E402
from PIL import Image  # noqa: E402

ROOT = guide.ROOT
OUT = ROOT / "docs" / "images" / "readme"
WIDTH = 1600   # px of the stored pictures (the readme shows them at about half that)
COLOURS = 256  # palette size of the stored PNGs
# The projects refer to the demo media through this link, so the pictures (Metadata, Media
# Storage) show a short neutral folder instead of the build directory.
MEDIA_LINK = Path("/tmp/kinoklip-demo/Videos/Field Notes")

# name: (target, project, extra environment). The names are the files in docs/images/readme.
SHOTS = {
    "media": ("media:host.mp4", "readme-edit.kkproj", {}),
    "cut": ("cut", "readme-edit.kkproj", {}),
    "edit": ("clip:V3:title", "readme-edit.kkproj", {"KK_SCREENSHOT_SOURCE": "background.mp4"}),
    "compositing": ("compositing", "readme-compositing.kkproj",
                    {"KK_SCREENSHOT_NODE": "ChromaKey1"}),
    "color": ("color:primaries", "readme-color.kkproj",
              {"KK_SCREENSHOT_SCOPE": "parade", "KK_SCREENSHOT_COLOR_NODE": "1",
               "KK_SCREENSHOT_COLOR_GRAB": "1"}),
    "audio": ("audio", "readme-edit.kkproj", {}),
    "deliver": ("deliver:queue", "readme-edit.kkproj",
                {"KK_SCREENSHOT_QUEUE": "master_prores_hq,audio_wav"}),
    "project-manager": ("project-manager", "readme-edit.kkproj",
                        {"KK_PROJECT_LIBRARY": "{library}"}),
}


def make_host(media: Path) -> None:
    """host.mp4: the guide's presenter (in front of a wall) looped to 36 s, with a sound track
    that looks like speech in a waveform (syllables and pauses) instead of a steady tone."""
    out = media / "host.mp4"
    if out.exists():
        return
    print("  media: host.mp4")
    voice = ("aevalsrc='0.5*(sin(2*PI*130*t)+0.5*sin(2*PI*260*t)+0.25*sin(2*PI*390*t))"
             "*(0.35+0.65*abs(sin(2*PI*2.7*t)))*(0.6+0.4*sin(2*PI*0.9*t))"
             "*gt(sin(2*PI*0.23*t)+sin(2*PI*0.71*t)+0.9,0)':s=48000:d=36,"
             "aformat=channel_layouts=stereo")
    subprocess.run(guide.FF + ["-stream_loop", "2", "-i", str(media / "presenter.mp4"),
                               "-f", "lavfi", "-i", voice, "-map", "0:v", "-map", "1:a",
                               "-c:v", "copy"] + guide.AAC + ["-t", "36", str(out)],
                   check=True)


def store(raw: Path, out: Path) -> None:
    image = Image.open(raw).convert("RGB")
    if image.width > WIDTH:
        image = image.resize((WIDTH, round(image.height * WIDTH / image.width)), Image.LANCZOS)
    out.parent.mkdir(parents=True, exist_ok=True)
    image.quantize(COLOURS, method=Image.Quantize.MEDIANCUT,
                   dither=Image.Dither.FLOYDSTEINBERG).save(out, optimize=True)
    print(f"  {out.relative_to(ROOT)}: {image.width}x{image.height}, "
          f"{out.stat().st_size // 1024} KB")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--app", default=str(ROOT / "build" / "kinoklip"))
    parser.add_argument("--tests", default=str(ROOT / "build"))
    parser.add_argument("--work", default=str(ROOT / "build" / "readme"))
    parser.add_argument("--only", help="one shot (" + ", ".join(SHOTS) + ")")
    parser.add_argument("--keep", action="store_true", help="reuse raw shots from --work")
    parser.add_argument("--delay", type=int, default=8000,
                        help="ms to wait for thumbnails and waveforms before each shot")
    args = parser.parse_args()
    guide.DELAY_MS = args.delay

    if "KK_GUIDE_UNDER_XVFB" not in os.environ:
        if not shutil.which("xvfb-run"):
            sys.exit("readme_screenshots.py: needs xvfb-run")
        env = dict(os.environ, KK_GUIDE_UNDER_XVFB="1")
        sys.exit(subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1920x1080x24",
                                 sys.executable, __file__] + sys.argv[1:], env=env).returncode)

    app, work = Path(args.app).resolve(), Path(args.work).resolve()
    media, projects, raw = work / "media", work / "projects", work / "raw"
    library = work / "library"
    raw.mkdir(parents=True, exist_ok=True)
    print("demo media")
    guide.make_media(media)
    make_host(media)
    MEDIA_LINK.parent.mkdir(parents=True, exist_ok=True)
    if MEDIA_LINK.is_symlink() or MEDIA_LINK.exists():
        MEDIA_LINK.unlink()
    MEDIA_LINK.symlink_to(media, target_is_directory=True)
    print("demo projects")
    guide.build_projects(Path(args.tests).resolve(), MEDIA_LINK, projects)
    # A small Project Manager library: the demo projects under their own names.
    if library.exists():
        shutil.rmtree(library)
    library.mkdir(parents=True)
    for name, file in (("Field Notes", "readme-edit"), ("Weather Report", "readme-compositing"),
                       ("Interview", "readme-color"), ("My First Video", "edit")):
        shutil.copy(projects / f"{file}.kkproj", library / f"{name}.kkproj")

    print("screenshots")
    for name, (target, project, env) in SHOTS.items():
        if args.only and args.only != name:
            continue
        png, geo = raw / f"{name}.png", raw / f"{name}.json"
        if not (args.keep and png.exists()):
            env = {k: v.format(library=library) for k, v in env.items()}
            guide.run_shot(app, target, projects / project, png, geo, env)
        store(png, OUT / f"{name}.png")


if __name__ == "__main__":
    main()
