#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes the screenshots of the readme's Screenshots section (docs/images/readme/).

One picture per page (Media, Cut, Edit, Compositing, Color, Audio, Deliver), a few feature
shots and themes.png (Breeze Dark and Breeze Light split diagonally), taken from the real app
in Breeze Dark (Breeze widget style and colour scheme) at 1920x1080, then scaled to 1600 px
wide and stored as palette PNGs.

The footage is made from five CC0 photos of people doing creative work (scripts/readme-media,
credited in attribution.md): each becomes a 1920x1080 25 fps clip with a slow zoom or pan, so
timeline thumbnails differ along the clip; filmmaker-camera.mp4 also carries a speech-like
sound track.
Reuses the user guide's tools (scripts/guide_screenshots.py): its generated music, the demo
projects of tests/timeline/test_guidedemo.cpp (readme-edit, readme-compositing and
readme-color.kkproj) and `kinoklip --screenshot <target> <png> <project>`.

Usage (from the repository root, after building the app and the tests into build/):
  scripts/readme_screenshots.py [--app dist/kinoklip-x86_64/AppRun] [--tests build] [--work build/readme]
                                [--only <name>] [--keep] [--delay 30000]

Runs under xvfb-run (1920x1080x24) with a clean environment and a temporary HOME, so the
owner's settings and desktop are never touched. Needs ffmpeg, xvfb-run and Pillow.
"""

import argparse
import json
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
MEDIA_LINK = Path("/tmp/kinoklip-demo/Videos/Shoot")

# name: (target, project, extra environment). The names are the files in docs/images/readme.
SHOTS = {
    "media": ("media:filmmaker-camera.mp4", "readme-edit.kkproj", {}),
    "cut": ("cut", "readme-edit.kkproj", {}),
    "edit": ("clip:V3:title", "readme-edit.kkproj",
             {"KK_SCREENSHOT_SOURCE": "guitarist-stage.mp4"}),
    "compositing": ("compositing", "readme-compositing.kkproj",
                    {"KK_SCREENSHOT_NODE": "Transform1"}),
    "color": ("color:primaries", "readme-color.kkproj",
              {"KK_SCREENSHOT_SCOPE": "parade", "KK_SCREENSHOT_COLOR_NODE": "1",
               "KK_SCREENSHOT_COLOR_GRAB": "1", "KK_SCREENSHOT_SAVE": "1"}),
    "audio": ("audio", "readme-edit.kkproj", {}),
    "deliver": ("deliver:queue", "readme-edit.kkproj",
                {"KK_SCREENSHOT_QUEUE": "master_prores_hq,audio_wav"}),
    "project-manager": ("project-manager", "readme-edit.kkproj",
                        {"KK_PROJECT_LIBRARY": "{library}"}),
}

# The readme's theme picture: the Edit page in Breeze Dark and in Breeze Light, joined along a
# diagonal (dark on the left). name: colour scheme id given with --color-scheme.
THEMES = {"breeze-dark": "BreezeDark", "breeze-light": "BreezeLight"}
SPLIT = (0.62, 0.38)  # where the dividing line meets the top and the bottom edge (of the width)


PHOTOS = ROOT / "scripts" / "readme-media"

# clip: (photo, seconds, (zoom from, to), (x from, to), (y from, to), vertical place of the 16:9
# band in the photo); x and y place the zoomed window from 0 (left / top) to 1 (right / bottom)
# of that band, the last number from 0 (top) to 1 (bottom) of the photo.
CLIPS = {
    "filmmaker-camera.mp4": ("filmmaker-camera.jpg", 36, (1.0, 1.12), (0.6, 0.8), (0.3, 0.3), 0.5),
    "painter-easel.mp4": ("painter-easel.jpg", 20, (1.15, 1.0), (0.7, 0.5), (0.3, 0.4), 0.3),
    "guitarist-stage.mp4": ("guitarist-stage.jpg", 15, (1.0, 1.3), (0.5, 0.5), (0.1, 0.3), 0.1),
    "pianist-recital.mp4": ("pianist-recital.jpg", 15, (1.0, 1.3), (0.7, 0.8), (0.4, 0.4), 0.35),
    "band-studio.mp4": ("band-studio.jpg", 12, (1.35, 1.0), (0.85, 0.7), (0.5, 0.5), 0.5),
}


def speech(seconds: int) -> str:
    """A sound track that looks like speech in a waveform (syllables and pauses)."""
    return ("aevalsrc='0.5*(sin(2*PI*130*t)+0.5*sin(2*PI*260*t)+0.25*sin(2*PI*390*t))"
            "*(0.35+0.65*abs(sin(2*PI*2.7*t)))*(0.6+0.4*sin(2*PI*0.9*t))"
            f"*gt(sin(2*PI*0.23*t)+sin(2*PI*0.71*t)+0.9,0)':s=48000:d={seconds},"
            "aformat=channel_layouts=stereo")


def make_clips(media: Path) -> None:
    """The demo clips: each photo cropped to 16:9 and slowly zoomed or panned (zoompan on a
    2x frame, so the motion stays smooth); filmmaker-camera.mp4 gets the speech-like sound."""
    for name, (photo, seconds, zoom, xs, ys, band) in CLIPS.items():
        out = media / name
        if out.exists():
            continue
        print("  media:", name)
        n = seconds * 25 - 1
        z = f"{zoom[0]}+({zoom[1]}-{zoom[0]})*on/{n}"
        x = f"(iw-iw/zoom)*({xs[0]}+({xs[1]}-{xs[0]})*on/{n})"
        y = f"(ih-ih/zoom)*({ys[0]}+({ys[1]}-{ys[0]})*on/{n})"
        vf = (f"crop=iw:iw*9/16:0:(ih-iw*9/16)*{band},scale=3840:2160,"
              f"zoompan=z='{z}':x='{x}':y='{y}':d=1:s=1920x1080:fps=25,format=yuv420p")
        cmd = guide.FF + ["-loop", "1", "-framerate", "25", "-i", str(PHOTOS / photo)]
        if name == "filmmaker-camera.mp4":
            cmd += ["-f", "lavfi", "-i", speech(seconds)] + guide.AAC
        cmd += ["-vf", vf] + guide.X264 + ["-r", "25", "-t", str(seconds), str(out)]
        subprocess.run(cmd, check=True)


def store(raw: Path, out: Path) -> None:
    image = Image.open(raw).convert("RGB")
    if image.width > WIDTH:
        image = image.resize((WIDTH, round(image.height * WIDTH / image.width)), Image.LANCZOS)
    out.parent.mkdir(parents=True, exist_ok=True)
    image.quantize(COLOURS, method=Image.Quantize.MEDIANCUT,
                   dither=Image.Dither.FLOYDSTEINBERG).save(out, optimize=True)
    print(f"  {out.relative_to(ROOT)}: {image.width}x{image.height}, "
          f"{out.stat().st_size // 1024} KB")


def store_split(dark: Path, light: Path, out: Path) -> None:
    """Joins two shots of the same size along a diagonal and stores the result like store()."""
    from PIL import ImageDraw
    a, b = Image.open(dark).convert("RGB"), Image.open(light).convert("RGB")
    w, h = a.size
    top, bottom = SPLIT[0] * w, SPLIT[1] * w
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).polygon([(0, 0), (top, 0), (bottom, h), (0, h)], fill=255)
    joined = Image.composite(a, b, mask)
    ImageDraw.Draw(joined).line([(top, 0), (bottom, h)], fill=(128, 128, 128), width=4)
    # Kept in full colour: two schemes in one 256-colour palette turn the reds and oranges
    # (playhead, selection) brown.
    joined = joined.resize((WIDTH, round(h * WIDTH / w)), Image.LANCZOS)
    out.parent.mkdir(parents=True, exist_ok=True)
    joined.save(out, optimize=True)
    print(f"  {out.relative_to(ROOT)}: {joined.width}x{joined.height}, "
          f"{out.stat().st_size // 1024} KB")


def breeze_available(app: Path) -> bool:
    """True when the app can load the Breeze widget style: bundled next to it (an AppDir), on
    QT_PLUGIN_PATH or in the system's Qt 6 plugin folders. Without it Qt silently uses Fusion."""
    folders = [app.parent / "usr" / "lib" / "qt" / "plugins" / "styles",
               app.parent.parent / "lib" / "qt" / "plugins" / "styles",
               Path("/usr/lib/x86_64-linux-gnu/qt6/plugins/styles"),
               Path("/usr/lib/qt6/plugins/styles"), Path("/usr/lib64/qt6/plugins/styles")]
    folders += [Path(p) / "styles" for p in os.environ.get("QT_PLUGIN_PATH", "").split(":") if p]
    return any(folder.glob("breeze*.so") for folder in folders if folder.is_dir())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--app", default=str(ROOT / "build" / "kinoklip"))
    parser.add_argument("--tests", default=str(ROOT / "build"))
    parser.add_argument("--work", default=str(ROOT / "build" / "readme"))
    parser.add_argument("--only", help="one shot (" + ", ".join(list(SHOTS) + ["themes"]) + ")")
    parser.add_argument("--keep", action="store_true", help="reuse raw shots from --work")
    parser.add_argument("--delay", type=int, default=30000,
                        help="ms to wait for thumbnails and waveforms before each shot")
    args = parser.parse_args()
    guide.DELAY_MS = args.delay
    if not breeze_available(Path(args.app).resolve()):
        sys.exit("readme_screenshots.py: no Breeze style for " + args.app + ". The app would fall "
                 "back to Fusion and the pictures would not show the real look. Use an AppDir "
                 "(scripts/package_linux.sh --no-appimage, then --app dist/kinoklip-x86_64/AppRun) "
                 "or install the Qt 6 Breeze style.")

    if "KK_GUIDE_UNDER_XVFB" not in os.environ:
        if not shutil.which("xvfb-run"):
            sys.exit("readme_screenshots.py: needs xvfb-run")
        env = dict(os.environ, KK_GUIDE_UNDER_XVFB="1")
        sys.exit(subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1920x1080x24",
                                 sys.executable, __file__] + sys.argv[1:], env=env).returncode)

    app, work = Path(args.app).resolve(), Path(args.work).resolve()
    media, projects, raw = work / "media", work / "projects", work / "raw"
    clips = work / "clips"
    library = work / "library"
    raw.mkdir(parents=True, exist_ok=True)
    print("demo media")
    guide.make_media(media)  # music.wav
    # The demo clips and the music in a folder of their own, so Media Storage lists only them
    # (the guide's demo projects need the rest and are skipped here).
    clips.mkdir(parents=True, exist_ok=True)
    make_clips(clips)
    if not (clips / "music.wav").exists():
        shutil.copy(media / "music.wav", clips / "music.wav")
    MEDIA_LINK.parent.mkdir(parents=True, exist_ok=True)
    if MEDIA_LINK.is_symlink() or MEDIA_LINK.exists():
        MEDIA_LINK.unlink()
    MEDIA_LINK.symlink_to(clips, target_is_directory=True)
    print("demo projects")
    guide.build_projects(Path(args.tests).resolve(), MEDIA_LINK, projects)
    # A small Project Manager library: the demo projects under other names, two of them with
    # the playhead (the poster frame) moved to another clip.
    if library.exists():
        shutil.rmtree(library)
    library.mkdir(parents=True)
    for name, file, seconds in (("Behind the Camera", "readme-edit", None),
                                ("Studio Stories", "readme-edit", 20),
                                ("Piano Recital", "readme-color", 15),
                                ("Live Session", "readme-color", 19)):
        project = json.loads((projects / f"{file}.kkproj").read_text())
        project["project"]["name"] = name
        if seconds is not None:
            fps = project["project"]["fps"]
            for view in project["timelineViews"]:
                view["playhead"] = round(seconds * fps["num"] / fps["den"])
        (library / f"{name}.kkproj").write_text(json.dumps(project, indent=1))

    print("screenshots")
    for name, (target, project, env) in SHOTS.items():
        if args.only and args.only != name:
            continue
        png, geo = raw / f"{name}.png", raw / f"{name}.json"
        if not (args.keep and png.exists()):
            env = {k: v.format(library=library) for k, v in env.items()}
            guide.run_shot(app, target, projects / project, png, geo, env,
                           args=("--color-scheme", "BreezeDark"))
        store(png, OUT / f"{name}.png")
    if not args.only or args.only == "themes":
        target, project, env = SHOTS["edit"]
        for name, scheme in THEMES.items():
            png, geo = raw / f"{name}.png", raw / f"{name}.json"
            if not (args.keep and png.exists()):
                guide.run_shot(app, target, projects / project, png, geo, env,
                               args=("--color-scheme", scheme))
        store_split(raw / "breeze-dark.png", raw / "breeze-light.png", OUT / "themes.png")


if __name__ == "__main__":
    main()
