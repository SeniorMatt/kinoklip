#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes the annotated screenshots of the user guide (docs/guide/images/<article>/).

Reproducible from the real app:
  1. generates the demo media with ffmpeg (a sky, a presenter on a pure green screen and in
     front of a wall and a dark, cold copy of it, three clips with sound, a music bed, a small
     .cube LUT of our own) into <work>/media, once;
  2. builds the guide projects with the test_guidedemo test (tests/timeline/test_guidedemo.cpp);
  3. runs `kinoklip --screenshot <target> <png> <project>` for every shot, with
     KK_SCREENSHOT_GEOMETRY so the app reports where its widgets are (src/app/ScreenshotGeometry);
  4. draws red boxes, numbered circles and arrows on those rectangles with Pillow (no hand-tuned
     pixels: the marks follow the layout), crops, scales and writes palette PNGs.

Usage (from the repository root, after building into build/):
  scripts/guide_screenshots.py [--app build/kinoklip] [--tests build] [--work build/guide]
                               [--only <article>[/<shot>]] [--keep]

Graphical runs happen under xvfb-run (1920x1080x24) with a clean environment and a temporary
HOME, so the owner's settings and desktop are never touched. Needs ffmpeg, xvfb-run, Pillow
(`pip install --user pillow`).
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover
    sys.exit("guide_screenshots.py: needs Pillow (pip install --user pillow)")

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "guide" / "images"

RED = (229, 57, 53)
WHITE = (255, 255, 255)
LINE = 4          # px at full size
MARGIN = 24       # px around a crop
DELAY_MS = 3000   # settle time per shot (--delay: longer on a busy machine)
FONT_FILES = ["/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
              "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf"]

# ---------------------------------------------------------------------------------------------
# Demo media

FF = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y"]
X264 = ["-c:v", "libx264", "-preset", "veryfast", "-crf", "24", "-pix_fmt", "yuv420p"]
AAC = ["-c:a", "aac", "-b:a", "128k"]


def make_media(media: Path) -> None:
    media.mkdir(parents=True, exist_ok=True)
    stamp = media / ".complete-v4"
    if stamp.exists():
        return
    size = "1920x1080"

    def video(name, vsrc, asrc=None, seconds=30):
        if (media / name).exists():
            return
        cmd = FF + ["-f", "lavfi", "-i", vsrc]
        if asrc:
            cmd += ["-f", "lavfi", "-i", asrc]
        cmd += X264 + (AAC if asrc else []) + ["-t", str(seconds), str(media / name)]
        print("  media:", name)
        subprocess.run(cmd, check=True)

    grad = "gradients=s={}:rate=25:duration={}:speed={}:c0={}:c1={}:x0=0:y0=0:x1=1920:y1=1080"
    # A sky over a green field: the background for the green screen article.
    video("background.mp4",
          grad.format(size, 12, 0.004, "0x3a7bd5", "0xf6d365")
          + ",drawbox=x=0:y=760:w=1920:h=320:color=0x2e7d32:t=fill"
          + ",drawbox=x=0:y=740:w=1920:h=24:color=0x388e3c:t=fill", None, 12)
    # A presenter (head and shoulders) on a pure green screen, swaying slowly.
    head = "lt(hypot(X-960-60*sin(T),Y-400),140)"
    body = "lt(hypot((X-960-60*sin(T))/300,(Y-1080)/470),1)"
    geq = ("geq=r='if({h},232,if({b},45,0))':g='if({h},186,if({b},80,177))'"
           ":b='if({h},150,if({b},170,64))'").format(h=head, b=body)
    video("greenscreen.mp4", "color=c=0x00b140:s={}:r=25:d=10,format=rgb24,{},format=yuv420p"
          .format(size, geq), None, 10)
    # The same presenter in front of a plain wall (picture-in-picture, vertical video, hiding
    # a face, freeze frame, keyframes, the Compositing page).
    # A face: two eyes and a mouth on the head (so a blur or mosaic over it shows).
    cx = "(960+60*sin(T))"
    face = ("(lt(hypot(X-{c}+45,Y-370),16)+lt(hypot(X-{c}-45,Y-370),16)"
            "+lt(abs(Y-462),9)*lt(abs(X-{c}),52))").format(c=cx)
    wall = ("geq=r='if({f},60,if({h},232,if({b},45,96+20*Y/1080)))'"
            ":g='if({f},40,if({h},186,if({b},80,104+20*Y/1080)))'"
            ":b='if({f},40,if({h},150,if({b},170,120+20*Y/1080)))'").format(h=head, b=body,
                                                                          f=face)
    video("presenter.mp4", "color=c=black:s={}:r=25:d=12,format=rgb24,{},format=yuv420p"
          .format(size, wall), "sine=f=262:sample_rate=48000:d=12,volume=0.15,"
          "aformat=channel_layouts=stereo", 12)
    # The same shot too dark and too blue (the Color page articles correct it).
    video("presenter-cold.mp4", "color=c=black:s={}:r=25:d=12,format=rgb24,{},"
          "colorchannelmixer=rr=0.64:gg=0.70:bb=0.80,format=yuv420p".format(size, wall),
          "sine=f=262:sample_rate=48000:d=12,volume=0.15,aformat=channel_layouts=stereo", 12)
    write_lut(media / "luts" / "warm-look.cube")
    tone = "sine=f={}:sample_rate=48000:d=30,volume=0.15,aformat=channel_layouts=stereo"
    video("interview.mp4", grad.format(size, 30, 0.01, "0x614385", "0x516395"), tone.format(220))
    video("broll.mp4", grad.format(size, 30, 0.02, "0xff7e5f", "0xfeb47b"), tone.format(330))
    video("texture.mp4", grad.format(size, 30, 0.015, "0x11998e", "0x38ef7d"), tone.format(440))
    chord = ("aevalsrc='(0.12*sin(2*PI*220*t)+0.08*sin(2*PI*277.18*t)+0.08*sin(2*PI*329.63*t))"
             "*(0.7+0.3*sin(2*PI*0.5*t))':s=48000:d=40")
    if not (media / "music.wav").exists():
        print("  media: music.wav")
        subprocess.run(FF + ["-f", "lavfi", "-i", chord, "-ac", "2", str(media / "music.wav")],
                       check=True)
    stamp.touch()


def write_lut(path: Path, size: int = 17) -> None:
    """A gentle "warm look" 3D LUT of our own: a soft S-curve, warmer highlights, slightly
    cooler shadows and a little less saturation (the LUT article loads it)."""
    if path.exists():
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = ['TITLE "Warm look (Kinoklip guide demo)"', f"LUT_3D_SIZE {size}"]

    for b in range(size):          # .cube order: red changes fastest
        for g in range(size):
            for r in range(size):
                rgb = [r / (size - 1), g / (size - 1), b / (size - 1)]
                luma = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]
                out = []
                for i, c in enumerate(rgb):
                    c = luma + (c - luma) * 0.9                      # a little less colour
                    c = c - 0.12 * c * (1 - c) * (1 - 2 * c)         # soft S-curve
                    warm = (0.05 * luma, 0.015 * luma, -0.04 * luma)[i]
                    cool = (-0.02, 0.0, 0.02)[i] * (1 - luma)
                    out.append(min(1.0, max(0.0, c + warm + cool)))
                lines.append(" ".join(f"{v:.6f}" for v in out))
    path.write_text("\n".join(lines) + "\n")


# ---------------------------------------------------------------------------------------------
# Running the app

def clean_env(home: Path) -> dict:
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(home),
           "XDG_CONFIG_HOME": str(home / "config"), "XDG_CACHE_HOME": str(home / "cache"),
           "XDG_DATA_HOME": str(home / "data"), "XDG_RUNTIME_DIR": str(home / "runtime"),
           "LANG": "C.UTF-8", "KK_NO_AUDIO": "1"}
    for key in ("DISPLAY", "XAUTHORITY"):
        if key in os.environ:
            env[key] = os.environ[key]
    for sub in ("config", "cache", "data", "runtime"):
        (home / sub).mkdir(parents=True, exist_ok=True)
    os.chmod(home / "runtime", 0o700)
    return env


def build_projects(tests: Path, media: Path, projects: Path) -> None:
    binary = tests / "test_guidedemo"
    if not binary.exists():
        sys.exit(f"guide_screenshots.py: {binary} is missing (build the tests)")
    with tempfile.TemporaryDirectory(prefix="kk-guide-home-") as home:
        env = clean_env(Path(home))
        env.update({"QT_QPA_PLATFORM": "offscreen", "KK_GUIDE_MEDIA": str(media),
                    "KK_GUIDE_DIR": str(projects)})
        subprocess.run([str(binary)], env=env, check=True, stdout=subprocess.DEVNULL)


def run_shot(app: Path, target: str, project: Path, png: Path, geometry: Path,
             extra_env: dict, luts=()) -> None:
    with tempfile.TemporaryDirectory(prefix="kk-guide-home-") as home:
        env = clean_env(Path(home))
        env["KK_SCREENSHOT_GEOMETRY"] = str(geometry)
        env["KK_SCREENSHOT_DELAY_MS"] = str(DELAY_MS)  # thumbnails, waveforms and posters
        env["KK_SCREENSHOT_LOCATION"] = "/home/you/Videos"  # not the temporary folder
        env.update(extra_env)
        for lut in luts:  # the Color page's LUT folder (QStandardPaths::AppDataLocation/LUT)
            folder = Path(home) / "data" / "kinoklip" / "Kinoklip" / "LUT"
            folder.mkdir(parents=True, exist_ok=True)
            shutil.copy(lut, folder / lut.name)
        cmd = [str(app), "--screenshot", target, str(png)]
        if project:
            cmd.append(str(project))
        result = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=180)
        if result.returncode != 0 or not png.exists():
            sys.exit(f"screenshot {target} failed ({result.returncode}):\n{result.stdout[-3000:]}")


# ---------------------------------------------------------------------------------------------
# Geometry and drawing

class Geometry:
    def __init__(self, path: Path):
        self.entries = json.loads(path.read_text())["entries"]

    def find(self, sel: dict) -> tuple:
        """Rectangle (x, y, w, h) of the entry matching every key of @p sel.

        Special keys: "nth" picks among several matches (default 0); "part" = left / right /
        top / bottom with "frac" keeps that fraction of the rectangle; "fx" / "fy" = (a, b)
        keep the horizontal / vertical span from fraction a to b; "pad" grows it."""
        keys = {k: v for k, v in sel.items()
                if k not in ("nth", "part", "frac", "pad", "fx", "fy")}
        hits = [e for e in self.entries
                if all(str(e.get(k)) == str(v) for k, v in keys.items())]
        nth = sel.get("nth", 0)
        if len(hits) <= nth:
            sys.exit(f"guide_screenshots.py: nothing matches {sel}")
        x, y, w, h = hits[nth]["rect"]
        part, frac = sel.get("part"), sel.get("frac", 0.25)
        if part == "left":
            w = max(8, int(w * frac))
        elif part == "right":
            x, w = x + w - max(8, int(w * frac)), max(8, int(w * frac))
        elif part == "top":
            h = max(8, int(h * frac))
        elif part == "bottom":
            y, h = y + h - max(8, int(h * frac)), max(8, int(h * frac))
        if "fx" in sel:
            a, b = sel["fx"]
            x, w = x + int(w * a), max(8, int(w * (b - a)))
        if "fy" in sel:
            a, b = sel["fy"]
            y, h = y + int(h * a), max(8, int(h * (b - a)))
        pad = sel.get("pad", 3)
        return (x - pad, y - pad, w + 2 * pad, h + 2 * pad)


def union(rects):
    x0 = min(r[0] for r in rects)
    y0 = min(r[1] for r in rects)
    x1 = max(r[0] + r[2] for r in rects)
    y1 = max(r[1] + r[3] for r in rects)
    return (x0, y0, x1 - x0, y1 - y0)


def font(size: int):
    for f in FONT_FILES:
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default(size=size)


def draw_number(draw, x, y, n, scale):
    r = int(17 * scale)
    draw.ellipse((x - r, y - r, x + r, y + r), fill=RED, outline=WHITE, width=max(2, int(2 * scale)))
    draw.text((x, y), str(n), fill=WHITE, font=font(int(20 * scale)), anchor="mm")


def draw_arrow(draw, a, b, scale):
    import math
    (x0, y0), (x1, y1) = a, b
    width = max(3, int(LINE * scale))
    draw.line((x0, y0, x1, y1), fill=RED, width=width)
    angle = math.atan2(y1 - y0, x1 - x0)
    size = 22 * scale
    left = (x1 - size * math.cos(angle - 0.45), y1 - size * math.sin(angle - 0.45))
    right = (x1 - size * math.cos(angle + 0.45), y1 - size * math.sin(angle + 0.45))
    draw.polygon([(x1, y1), left, right], fill=RED)


def edge_point(rect, towards):
    """Point on the border of @p rect on the way to the point @p towards."""
    x, y, w, h = rect
    cx, cy = x + w / 2, y + h / 2
    dx, dy = towards[0] - cx, towards[1] - cy
    if dx == 0 and dy == 0:
        return cx, cy
    t = min(abs(w / 2 / dx) if dx else 1e9, abs(h / 2 / dy) if dy else 1e9)
    return cx + dx * t, cy + dy * t


def annotate(raw: Path, geometry: Path, shot: dict, out: Path) -> None:
    geo = Geometry(geometry)
    img = Image.open(raw).convert("RGB")
    # Crop first (in full-size pixels), then mark, then scale: marks keep their thickness.
    crop = shot.get("crop")
    if crop:
        x, y, w, h = union([geo.find(s) for s in crop])
        m = shot.get("margin", MARGIN)
        box = (max(0, x - m), max(0, y - m), min(img.width, x + w + m), min(img.height, y + h + m))
    else:
        box = (0, 0, img.width, img.height)
    img = img.crop(box)
    max_width = shot.get("width", 1280)
    scale = max_width / img.width if shot.get("upscale") else min(1.0, max_width / img.width)
    if scale != 1.0:
        img = img.resize((int(img.width * scale), int(img.height * scale)), Image.LANCZOS)

    mark = 1.0

    def place(rect):
        return ((rect[0] - box[0]) * scale, (rect[1] - box[1]) * scale,
                rect[2] * scale, rect[3] * scale)

    draw = ImageDraw.Draw(img)
    numbers = []
    for item in shot.get("marks", []):
        kind = item[0]
        if kind in ("box", "num"):
            x, y, w, h = place(geo.find(item[1]))
            draw.rectangle((x, y, x + w, y + h), outline=RED, width=LINE)
            if kind == "num":
                numbers.append((x, y, w, h, item[2], item[3] if len(item) > 3 else "tl"))
        elif kind == "arrow":
            a, b = place(geo.find(item[1])), place(geo.find(item[2]))
            ca = (a[0] + a[2] / 2, a[1] + a[3] / 2)
            cb = (b[0] + b[2] / 2, b[1] + b[3] / 2)
            draw_arrow(draw, edge_point(a, cb), edge_point(b, ca), mark)
    r = 17 * mark + 3
    for x, y, w, h, n, corner in numbers:  # on top of every box
        # Corners (tl, tr, bl, br) sit on the box corner; t, b, l, r just outside that side;
        # c in the middle of the box.
        if corner == "c":
            px, py = x + w / 2, y + h / 2
        elif corner in ("t", "b"):
            px, py = x + w / 2, (y - r if corner == "t" else y + h + r)
        elif corner in ("l", "r"):
            px, py = (x - r if corner == "l" else x + w + r), y + h / 2
        else:
            px = x + w if "r" in corner else x
            py = y + h if "b" in corner else y
        px = min(max(px, 20), img.width - 20)
        py = min(max(py, 20), img.height - 20)
        draw_number(draw, px, py, n, mark)
    out.parent.mkdir(parents=True, exist_ok=True)
    # A 256-colour palette keeps UI pictures small and sharp (no dithering on flat colours).
    img.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).save(
        out, optimize=True)
    kb = out.stat().st_size // 1024
    print(f"  {out.relative_to(ROOT)}  {img.width}x{img.height}  {kb} KB")


# ---------------------------------------------------------------------------------------------
# The shots. Selectors match entries of the geometry sidecar (see ScreenshotGeometry.h).

def W(**kw):
    return dict(kind="widget", **kw)


PAGES = ["view.page_media", "view.page_cut", "view.page_edit", "view.page_compositing",
         "view.page_color", "view.page_audio", "view.page_deliver"]
DELIVER = [W(name="PresetTile_youtube_1080p"), W(name="RenderFileName"),
           W(name="RenderLocation"), W(name="RenderRange"), W(text="Add to Render Queue")]

SHOTS = {
    "getting-started": [
        dict(name="1-pages", target="edit", project="edit.kkproj",
             marks=[("num", W(action=a, pad=0), i + 1, "t") for i, a in enumerate(PAGES)]),
        dict(name="2-project-manager", target="project-manager", project="edit.kkproj",
             env={"KK_PROJECT_LIBRARY": "{library}"}, width=1180,
             marks=[("box", W(text="New Project"))]),
        dict(name="3-new-project", target="new-project", width=900,
             marks=[("num", W(name="SingleFileKind"), 1, "tr"),
                    ("num", W(name="LinkedKind"), 2, "tr")]),
        dict(name="4-edit-page", target="edit", project="edit.kkproj",
             marks=[("num", W(**{"class": "kk::MediaPoolPanel"}), 1, "c"),
                    ("num", W(name="TimelineViewer"), 2, "c"),
                    ("num", W(**{"class": "kk::InspectorPanel"}), 3, "c"),
                    ("num", W(**{"class": "kk::TimelinePanel"}), 4, "c")]),
        dict(name="5-timeline-tools", target="timeline", project="edit.kkproj",
             crop=[W(action="tool.select"), W(action="mark.add_marker")], margin=40, width=760,
             upscale=True,
             marks=[("num", W(action="tool.select", pad=1), 1, "b"),
                    ("num", W(action="tool.trim", pad=1), 2, "b"),
                    ("num", W(action="tool.blade", pad=1), 3, "b"),
                    ("box", W(action="edit.insert", pad=1)), ("box", W(action="edit.overwrite", pad=1)),
                    ("box", W(action="edit.place_on_top", pad=1)),
                    ("box", W(action="edit.replace", pad=1)),
                    ("num", W(action="edit.insert", pad=1), 4, "t"),
                    ("num", W(action="edit.split", pad=1), 5, "b")]),
        dict(name="6-title", target="effects:text_title", project="edit.kkproj",
             env={"KK_SCREENSHOT_WINDOW": "1"},
             crop=[W(**{"class": "kk::EffectsLibraryPanel"}), W(**{"class": "kk::TimelinePanel"})],
             marks=[("box", dict(kind="item", data="text_title")),
                    ("box", dict(kind="clip", track="V3", index=0)),
                    ("arrow", dict(kind="item", data="text_title"),
                     dict(kind="clip", track="V3", index=0))]),
        dict(name="7-transition", target="transition", project="edit.kkproj",
             crop=[W(**{"class": "kk::TimelinePanel"})], margin=0,
             marks=[("box", dict(kind="transition", track="V1", index=1, head=True, pad=4))]),
        dict(name="8-deliver", target="deliver:inout", project="edit.kkproj",
             marks=[("num", W(name="PresetTile_youtube_1080p"), 1, "tr"),
                    ("num", W(name="RenderRange"), 2, "tr"),
                    ("num", W(text="Add to Render Queue"), 3, "tr")]),
        dict(name="9-render-queue", target="deliver:queue", project="edit.kkproj",
             crop=[W(**{"class": "kk::RenderQueuePanel"})], margin=0, width=700,
             env={"KK_SCREENSHOT_SIZE": "1440x720"},
             marks=[("box", W(name="RenderJobCard")), ("num", W(text="Render All"), 1, "tr")]),
    ],
    "export-for-youtube": [
        dict(name="1-deliver-page", target="deliver:inout", project="edit.kkproj",
             env={"KK_SCREENSHOT_FRAME": "250"},
             marks=[("num", W(action="view.page_deliver"), 1, "tr"),
                    ("num", W(name="PresetTile_youtube_1080p"), 2, "tr"),
                    ("num", W(name="RenderFileName"), 3, "r"),
                    ("box", W(name="RenderLocation"))]),
        dict(name="2-range-and-queue", target="deliver:inout", project="edit.kkproj",
             crop=[W(**{"class": "kk::RenderSettingsPanel"})], margin=0, width=700,
             env={"KK_SCREENSHOT_SIZE": "1440x900"},
             marks=[("num", W(name="RenderRange"), 1, "t"),
                    ("num", W(text="Add to Render Queue"), 2, "t")]),
        dict(name="3-render-all", target="deliver:queue", project="edit.kkproj",
             crop=[W(**{"class": "kk::RenderQueuePanel"})], margin=0, width=700,
             env={"KK_SCREENSHOT_SIZE": "1440x720"},
             marks=[("box", W(name="RenderJobCard")), ("num", W(text="Render All"), 1, "tr")]),
    ],
    "remove-a-green-screen": [
        dict(name="1-before", target="edit", project="greenscreen-raw.kkproj",
             crop=[W(name="TimelineViewer")], margin=0, width=960, marks=[]),
        dict(name="2-add-chroma-key", target="effects:chroma_key", project="greenscreen-raw.kkproj",
             env={"KK_SCREENSHOT_WINDOW": "1"},
             marks=[("num", W(action="view.toggle_effects"), 1, "b"),
                    ("num", dict(kind="item", data="chroma_key"), 2, "tr"),
                    ("num", dict(kind="clip", track="V2", index=0), 3, "tr"),
                    ("arrow", dict(kind="item", data="chroma_key"),
                     dict(kind="clip", track="V2", index=0))]),
        dict(name="3-eyedropper", target="fx:chroma_key:inspector", project="greenscreen.kkproj",
             env={"KK_SCREENSHOT_PICKER": "1"}, width=700,
             marks=[("num", W(name="ColorPickerButton"), 1, "tr"),
                    ("num", W(name="value:tolerance"), 2, "r"),
                    ("num", W(name="value:spillAmount"), 3, "r"),
                    ("num", W(name="choice:view"), 4, "tr")]),
        dict(name="4-matte", target="fx:chroma_key", project="greenscreen-matte.kkproj",
             crop=[W(name="TimelineViewer"), W(**{"class": "kk::InspectorPanel"})], margin=0,
             marks=[("box", W(name="choice:view"))]),
        dict(name="5-result", target="fx:chroma_key:viewer", project="greenscreen.kkproj",
             width=960, marks=[]),
    ],
    "change-clip-speed": [
        dict(name="1-change-clip-speed", target="change-speed", project="edit.kkproj", width=700,
             marks=[("num", W(name="SpeedPercent"), 1, "l"),
                    ("num", W(name="SpeedReverse", pad=1), 2, "l"),
                    ("num", W(name="SpeedRipple", pad=1), 3, "l"),
                    ("num", W(name="SpeedPitch", pad=1), 4, "l"),
                    ("num", W(text="Change"), 5, "l")]),
        dict(name="2-retime-controls", target="retime", project="edit.kkproj",
             crop=[dict(kind="track", track="V1"), dict(kind="clip", track="V1", index=0),
                   dict(kind="track", track="A1")], margin=12,
             marks=[("box", dict(kind="clip", track="V1", index=0, part="top", frac=0.45))]),
    ],
    "fade-audio-and-add-music": [
        dict(name="1-music-track", target="timeline", project="music.kkproj",
             marks=[("num", dict(kind="clip", track="A2", index=0), 1, "t"),
                    ("num", dict(kind="clip", track="A2", index=0, part="left", frac=0.17, pad=0),
                     2, "t"),
                    ("num", dict(kind="clip", track="A2", index=0, part="right", frac=0.25, pad=0),
                     3, "t")]),
        dict(name="2-audio-transitions", target="effects:audio_crossfade_3db",
             project="music.kkproj", width=900,
             marks=[("box", dict(kind="item", data="audio_crossfade_3db"))]),
    ],
    "fade-to-black": [
        dict(name="1-fade-handles", target="timeline", project="fade.kkproj",
             marks=[("num", dict(kind="clip", track="V1", index=0, part="left", frac=0.2, pad=0),
                     1, "t"),
                    ("num", dict(kind="clip", track="V1", index=1, part="right", frac=0.27, pad=0),
                     2, "t"),
                    ("num", dict(kind="clip", track="A1", index=1, part="right", frac=0.27, pad=0),
                     3, "b")]),
        dict(name="2-transitions", target="effects:cross_dissolve", project="fade.kkproj",
             width=900,
             marks=[("num", dict(kind="item", data="cross_dissolve"), 1, "r"),
                    ("num", dict(kind="item", data="dip_to_color"), 2, "r")]),
    ],
    "freeze-a-frame": [
        dict(name="1-freeze-segment", target="retime", project="freeze.kkproj",
             crop=[dict(kind="track", track="V1"), dict(kind="clip", track="V1", index=0)],
             margin=40,
             marks=[("num", dict(kind="clip", track="V1", index=0, fx=(0.3, 0.5), fy=(0.2, 0.4),
                             pad=1), 1, "t"),
                    ("num", dict(kind="clip", track="V1", index=0, fx=(0.49, 0.51),
                                 fy=(0.82, 1.0), pad=4), 2, "b")]),
    ],
    "zoom-crop-and-picture-in-picture": [
        dict(name="1-picture-in-picture", target="clip:V2:video", project="pip.kkproj",
             env={"KK_SCREENSHOT_ONSCREEN": "transform"},
             marks=[("num", dict(kind="clip", track="V2", index=0), 1, "t"),
                    ("num", W(name="value:zoomX"), 2, "l"),
                    ("num", W(name="value:posX"), 3, "l"),
                    ("num", W(name="value:cropL"), 4, "l"),
                    ("num", W(name="ViewerOnScreenControls"), 5, "t")]),
    ],
    "smooth-playback": [
        dict(name="1-playback-resolution", target="menu:playback:playback.resolution",
             project="edit.kkproj", width=900, upscale=False,
             env={"KK_SCREENSHOT_MENU_ITEM": "playback.resolution_half"},
             marks=[("box", dict(kind="menuitem", action="playback.resolution_half"))]),
        dict(name="2-viewer-resolution", target="edit", project="edit.kkproj",
             crop=[W(name="TimelineViewer")], margin=0, width=900,
             marks=[("num", W(name="ViewerQuality"), 1, "b")]),
    ],
    "vertical-video": [
        dict(name="1-project-settings", target="project-settings:master",
             project="vertical.kkproj", width=820,
             marks=[("num", W(name="TimelineResolution"), 1, "r")]),
        dict(name="2-fill-the-frame", target="clip:V1:video", project="vertical.kkproj",
             crop=[W(name="TimelineViewer"), W(**{"class": "kk::InspectorPanel"})], margin=0,
             marks=[("num", W(name="value:zoomX"), 1, "l"),
                    ("num", W(name="value:posX"), 2, "l")]),
        dict(name="3-render-resolution", target="deliver:inout", project="vertical.kkproj",
             crop=[W(**{"class": "kk::RenderSettingsPanel"})], margin=0, width=700,
             env={"KK_SCREENSHOT_SIZE": "1440x900", "KK_SCREENSHOT_RENDER_RESOLUTION": "0"},
             marks=[("num", W(name="RenderResolution"), 1, "t")]),
    ],
    "hide-part-of-the-picture": [
        dict(name="0-before", target="edit", project="freeze.kkproj",
             crop=[W(name="TimelineViewer")], margin=0, width=960, marks=[]),
        dict(name="1-adjustment-clip", target="effects:adjustment_clip", project="blur.kkproj",
             env={"KK_SCREENSHOT_WINDOW": "1"},
             crop=[W(**{"class": "kk::EffectsLibraryPanel"}), W(**{"class": "kk::TimelinePanel"})],
             marks=[("num", dict(kind="item", data="adjustment_clip"), 1, "r"),
                    ("num", dict(kind="clip", track="V2", index=0), 2, "t"),
                    ("arrow", dict(kind="item", data="adjustment_clip"),
                     dict(kind="clip", track="V2", index=0))]),
        dict(name="2-mask", target="masks", project="blur.kkproj",
             crop=[W(name="TimelineViewer"), W(**{"class": "kk::InspectorPanel"})], margin=0,
             marks=[("num", W(name="add-mask:ellipse"), 1, "b"),
                    ("num", W(name="value:featherOut"), 2, "l")]),
        dict(name="3-result", target="edit", project="blur.kkproj",
             crop=[W(name="TimelineViewer")], margin=0, width=960, marks=[]),
    ],
    "add-subtitles": [
        dict(name="1-captions", target="subtitles", project="subtitles.kkproj",
             marks=[("num", dict(kind="clip", track="ST1", index=1), 1, "t"),
                    ("num", W(name="text:text"), 2, "l"),
                    ("num", W(name="value:size"), 3, "l")]),
        dict(name="2-export", target="deliver:inout", project="subtitles.kkproj",
             crop=[W(**{"class": "kk::RenderSettingsPanel"})], margin=0, width=700,
             env={"KK_SCREENSHOT_SIZE": "1440x900", "KK_SCREENSHOT_RENDER_TAB": "2",
                  "KK_SCREENSHOT_RENDER_SUBTITLES": "0"},
             marks=[("num", W(name="RenderSubtitleExport"), 1, "l"),
                    ("num", W(name="RenderSubtitleMode"), 2, "l")]),
    ],
    "separate-audio-from-video": [
        dict(name="1-linked-selection", target="timeline", project="edit.kkproj",
             crop=[W(action="tool.select"), W(action="timeline.linked_selection")], margin=40,
             width=760, upscale=True,
             marks=[("num", W(action="timeline.linked_selection", pad=1), 1, "b")]),
        dict(name="2-unlink-clips", target="menu:clip", project="edit.kkproj", width=900,
             env={"KK_SCREENSHOT_MENU_ITEM": "clip.unlink"},
             marks=[("box", dict(kind="menuitem", action="clip.unlink"))]),
    ],
    "keyframe-animation": [
        dict(name="1-inspector-keyframes", target="clip:V1:video", project="keyframes.kkproj",
             crop=[W(**{"class": "kk::InspectorPanel"})], margin=0, width=700,
             marks=[("num", W(name="value:zoomX"), 1, "l")]),
        dict(name="2-keyframe-editor", target="keyframes", project="keyframes.kkproj",
             marks=[]),
    ],
    "even-out-clip-volumes": [
        dict(name="1-normalize", target="normalize-audio", project="edit.kkproj", width=560,
             upscale=True,
             marks=[("num", W(name="NormalizeMode"), 1, "r"),
                    ("num", W(name="NormalizeTarget"), 2, "r"),
                    ("num", W(text="Independent"), 3, "b")]),
    ],
    "compositing-page": [
        dict(name="1-compositing-page", target="compositing", project="compositing.kkproj",
             env={"KK_SCREENSHOT_NODE": "Text1"},
             marks=[("num", W(name="CompTool:text"), 1, "t"),
                    ("num", W(name="NodeEditor"), 2, "bl"),
                    ("num", W(name="text:text"), 3, "l"),
                    ("num", W(name="CompViewerRight"), 4, "tl")]),
    ],
    "colour-correct-a-clip": [
        dict(name="1-color-page", target="color:primaries", project="color.kkproj",
             marks=[("num", W(action="view.page_color"), 1, "t"),
                    ("num", W(name="ColorGalleryPanel"), 2, "c"),
                    ("num", W(name="ColorViewerOverlay"), 3, "c"),
                    ("num", W(name="ColorNodeEditor"), 4, "c"),
                    ("num", W(name="ColorClipStrip"), 5, "l"),
                    ("num", W(name="ColorPalettes"), 6, "c"),
                    ("num", W(**{"class": "kk::ScopesPanel"}), 7, "c")]),
        dict(name="2-primaries", target="color:primaries", project="color-graded.kkproj",
             crop=[W(name="ColorPalettes")], margin=0, width=1000,
             marks=[("num", W(name="ColorWheel_Lift", fx=(0.38, 0.62), fy=(0, 0.09)), 1, "r"),
                    ("num", W(name="ColorWheel_Gamma", fx=(0.33, 0.67), fy=(0, 0.09)), 2, "r"),
                    ("num", W(name="ColorWheel_Gain", fx=(0.38, 0.62), fy=(0, 0.09)), 3, "r"),
                    ("num", W(name="Primaries_temperature"), 4, "r"),
                    ("num", W(name="Primaries_tint"), 5, "r")]),
        dict(name="3-waveform-before", target="color:primaries", project="color.kkproj",
             crop=[W(**{"class": "kk::ScopesPanel"})], margin=0, width=520, marks=[]),
        dict(name="4-waveform-after", target="color:primaries", project="color-graded.kkproj",
             crop=[W(**{"class": "kk::ScopesPanel"})], margin=0, width=520, marks=[]),
        dict(name="5-parade", target="color:primaries", project="color-graded.kkproj",
             env={"KK_SCREENSHOT_SCOPE": "parade"},
             crop=[W(**{"class": "kk::ScopesPanel"})], margin=0, width=520,
             marks=[("box", W(name="ScopeType"))]),
        dict(name="6-bypass", target="color:primaries", project="color-graded.kkproj",
             crop=[W(name="ColorClipTargetButton"), W(name="ColorBypassButton")], margin=40,
             width=900,
             marks=[("num", W(name="ColorBypassButton"), 1, "b")]),
        dict(name="7-clips-strip", target="color:primaries", project="color-graded.kkproj",
             env={"KK_SCREENSHOT_COLOR_SELECT": "3"},
             crop=[dict(kind="item", view="ColorClipStrip", nth=0),
                   dict(kind="item", view="ColorClipStrip", nth=2)], margin=40, width=700,
             upscale=True,
             marks=[("num", dict(kind="item", view="ColorClipStrip", nth=0), 1, "b"),
                    ("num", dict(kind="item", view="ColorClipStrip", nth=2), 2, "b")]),
        dict(name="8-gallery", target="color:primaries", project="color-graded.kkproj",
             env={"KK_SCREENSHOT_COLOR_GRAB": "1"},
             crop=[W(action="color.grab_still"), dict(kind="item", view="ColorGallery", nth=0)],
             margin=40, width=420, upscale=True,
             marks=[("num", W(action="color.grab_still"), 1, "l"),
                    ("num", dict(kind="item", view="ColorGallery", nth=0), 2, "r")]),
    ],
    "change-the-colour-of-one-object": [
        dict(name="1-qualifier", target="color:qualifier", project="color-qualifier.kkproj",
             env={"KK_SCREENSHOT_COLOR_NODE": "2", "KK_SCREENSHOT_COLOR_PICK": "1"},
             marks=[("num", W(name="ColorNodeEditor"), 1, "tl"),
                    ("num", W(name="QualifierPick_0"), 2, "b"),
                    ("num", W(name="QualifierHueBar"), 3, "r"),
                    ("num", W(name="ColorViewerOverlay", fx=(0.38, 0.62), fy=(0.65, 1.0)), 4,
                     "r")]),
        dict(name="2-highlight", target="color:qualifier", project="color-qualifier.kkproj",
             env={"KK_SCREENSHOT_COLOR_NODE": "2", "KK_SCREENSHOT_COLOR_HIGHLIGHT": "1"},
             crop=[W(name="ColorClipTargetButton"), W(name="ColorViewerOverlay"),
                   W(name="ColorBypassButton")], margin=4, width=960,
             marks=[("num", W(name="ColorHighlightButton"), 1, "b")]),
        dict(name="3-hue", target="color:primaries", project="color-qualifier.kkproj",
             env={"KK_SCREENSHOT_COLOR_NODE": "2"},
             marks=[("num", W(name="Primaries_hue"), 1, "t"),
                    ("num", W(name="Primaries_saturation"), 2, "t"),
                    ("num", W(name="ColorViewerOverlay", fx=(0.38, 0.62), fy=(0.65, 1.0)), 3,
                     "r")]),
        dict(name="4-window", target="color:windows", project="color-qualifier.kkproj",
             env={"KK_SCREENSHOT_COLOR_NODE": "3"},
             marks=[("num", W(name="WindowAdd_circle"), 1, "b"),
                    ("num", W(name="ColorViewerOverlay", fx=(0.36, 0.64), fy=(0.12, 0.62)), 2,
                     "r"),
                    ("num", W(name="Window_softOut"), 3, "t")]),
    ],
    "use-a-lut": [
        dict(name="1-luts", target="color:luts", project="color-lut.kkproj", luts=True,
             env={"KK_SCREENSHOT_COLOR_NODE": "2"},
             marks=[("num", W(name="ColorPaletteTab_4"), 1, "t"),
                    ("num", dict(kind="item", view="LutTree", nth=1, fx=(0, 0.12)), 2, "r"),
                    ("num", W(name="LutAddFolder"), 3, "t"),
                    ("num", W(name="LutLoadFile"), 4, "t"),
                    ("num", W(name="LutCurrent", fx=(0, 0.3)), 5, "r")]),
    ],
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--app", default=str(ROOT / "build" / "kinoklip"))
    parser.add_argument("--tests", default=str(ROOT / "build"))
    parser.add_argument("--work", default=str(ROOT / "build" / "guide"))
    parser.add_argument("--only", help="article or article/shot")
    parser.add_argument("--keep", action="store_true", help="reuse raw shots from --work")
    parser.add_argument("--delay", type=int, default=DELAY_MS,
                        help="ms to wait for thumbnails and posters before each shot")
    args = parser.parse_args()
    globals()["DELAY_MS"] = args.delay

    if "KK_GUIDE_UNDER_XVFB" not in os.environ:
        if not shutil.which("xvfb-run"):
            sys.exit("guide_screenshots.py: needs xvfb-run")
        env = dict(os.environ, KK_GUIDE_UNDER_XVFB="1")
        sys.exit(subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1920x1080x24",
                                 sys.executable, __file__] + sys.argv[1:], env=env).returncode)

    app, work = Path(args.app).resolve(), Path(args.work).resolve()
    media, projects, raw = work / "media", work / "projects", work / "raw"
    library = work / "library"
    raw.mkdir(parents=True, exist_ok=True)
    print("demo media")
    make_media(media)
    print("demo projects")
    build_projects(Path(args.tests).resolve(), media, projects)
    # A small Project Manager library: copies of the demo project under friendly names.
    if library.exists():
        shutil.rmtree(library)
    library.mkdir(parents=True)
    for name in ("My First Video", "Holiday 2026", "Green Screen Test"):
        shutil.copy(projects / ("greenscreen.kkproj" if "Green" in name else "edit.kkproj"),
                    library / f"{name}.kkproj")

    for article, shots in SHOTS.items():
        for shot in shots:
            key = f"{article}/{shot['name']}"
            if args.only and args.only not in (article, key):
                continue
            png, geo = raw / f"{article}-{shot['name']}.png", raw / f"{article}-{shot['name']}.json"
            if not (args.keep and png.exists() and geo.exists()):
                env = {k: v.format(library=library) for k, v in shot.get("env", {}).items()}
                project = projects / shot["project"] if shot.get("project") else None
                luts = sorted((media / "luts").glob("*.cube")) if shot.get("luts") else []
                run_shot(app, shot["target"], project, png, geo, env, luts)
            annotate(png, geo, shot, OUT / article / f"{shot['name']}.png")


if __name__ == "__main__":
    main()
