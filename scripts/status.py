#!/usr/bin/env python3
"""Draws docs/images/status.svg, the picture of how much of Kinoklip is made, and puts the same
numbers in the readme's Status section.

    python3 scripts/status.py          # rewrites docs/images/status.svg and the readme block
    python3 scripts/status.py --check  # exits 1 when either is out of date (for ctest and CI)

Two treemaps side by side, one cell per item, green where it is made and grey where it is not:

* Roadmap items: docs/roadmap.md, its `- [x]` and `- [ ]` lines, grouped by the area before the
  first colon.
* Effects library: docs/effects-library-status.md, one row per entry of the library, grouped by
  the Group column; a row is made when its status is `works`.

The idea, the layout of the picture and the squarified treemap routine follow the status script
of Picasso Suite (https://github.com/fabox-os/Picasso-Suite, GPL-3.0-or-later, the same licence
as Kinoklip); the code here is rewritten for Kinoklip's files. See attribution.md.

Output is deterministic (no dates, stable ordering). Only the standard library is used.
"""
import html
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SVG = ROOT / "docs" / "images" / "status.svg"
README = ROOT / "readme.md"
ROADMAP = ROOT / "docs" / "roadmap.md"
EFFECTS = ROOT / "docs" / "effects-library-status.md"
START, END = "<!-- status:start -->", "<!-- status:end -->"
ANCHOR = "**[Roadmap](docs/roadmap.md)**"

DONE, TODO = "#2ea043", "#3a3d44"
DONE_LINE, TODO_LINE = "#1b7f32", "#2a2c31"
BACK, TEXT = "#16181d", "#e6e6ea"


def read(path):
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def roadmap_groups():
    """[(area, [done?, ...])] from the checklist, areas in order of first appearance."""
    order, areas = [], {}
    for line in read(ROADMAP).split("\n"):
        m = re.match(r"- \[([ x])\] (.*)", line)
        if not m:
            continue
        head = m.group(2).split(":", 1)[0]
        area = head if ":" in m.group(2) and len(head) <= 24 else "Other"
        if area not in areas:
            areas[area] = []
            order.append(area)
        areas[area].append(m.group(1) == "x")
    return [(a, areas[a]) for a in order]


def effects_groups():
    """[(group, [done?, ...])] from the rows of the effects table (columns: name, group, kind,
    status)."""
    order, groups = [], {}
    for line in read(EFFECTS).split("\n"):
        if not line.startswith("|") or line.startswith("|---"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) != 4 or cells[3] not in ("works", "planned", "not planned"):
            continue
        if cells[1] not in groups:
            groups[cells[1]] = []
            order.append(cells[1])
        groups[cells[1]].append(cells[3] == "works")
    return [(g, groups[g]) for g in order]


def squarify(sizes, x, y, w, h):
    """Rectangles (x, y, w, h) for `sizes` (largest first) filling the box: squarified treemap."""
    total = float(sum(sizes))
    if not sizes or total <= 0:
        return []
    scale = w * h / total
    areas = [s * scale for s in sizes]
    out, i = [], 0
    while i < len(areas):
        side = min(w, h)
        row, best = [], float("inf")
        while i + len(row) < len(areas):
            trial = row + [areas[i + len(row)]]
            s = sum(trial)
            worst = max(max(side * side * a / (s * s), s * s / (side * side * a)) for a in trial)
            if row and worst > best:
                break
            row, best = trial, worst
        s = sum(row)
        thick = s / side
        pos = 0.0
        for a in row:
            length = a / thick
            if w >= h:
                out.append((x, y + pos, thick, length))
            else:
                out.append((x + pos, y, length, thick))
            pos += length
        if w >= h:
            x, w = x + thick, w - thick
        else:
            y, h = y + thick, h - thick
        i += len(row)
    return out


def treemap(groups, x, y, w, h):
    """SVG for the groups in the box: a cell for each item, a label on each group that fits."""
    parts = []
    ordered = sorted(groups, key=lambda g: -len(g[1]))
    for (name, items), (gx, gy, gw, gh) in zip(
        ordered, squarify([len(g[1]) for g in ordered], x, y, w, h)
    ):
        items = sorted(items, key=lambda d: not d)  # done cells first, so the green sits together
        for done, (cx, cy, cw, ch) in zip(items, squarify([1] * len(items), gx, gy, gw, gh)):
            parts.append(
                f'<rect x="{cx:.1f}" y="{cy:.1f}" width="{cw:.1f}" height="{ch:.1f}" '
                f'fill="{DONE if done else TODO}" stroke="{DONE_LINE if done else TODO_LINE}" '
                f'stroke-width="1"/>'
            )
        parts.append(
            f'<rect x="{gx:.1f}" y="{gy:.1f}" width="{gw:.1f}" height="{gh:.1f}" fill="none" '
            f'stroke="{BACK}" stroke-width="2.5"/>'
        )
        # Label in the group's top-left corner, only when the whole name fits inside the group.
        if gw >= 6.4 * len(name) + 12 and gh >= 20:
            parts.append(
                f'<text x="{gx + 5:.1f}" y="{gy + 13:.1f}" font-size="10.5" fill="#fff" '
                f'stroke="{BACK}" stroke-width="2.5" paint-order="stroke">{html.escape(name)}</text>'
            )
    return "\n".join(parts)


def share(groups):
    done = sum(sum(items) for _, items in groups)
    total = sum(len(items) for _, items in groups)
    return done, total, 100.0 * done / total if total else 0.0


def colour_for(pct):
    return "#4c1" if pct >= 90 else "#97ca00" if pct >= 75 else "#dfb317" if pct >= 40 else "#e05d44"


def badge(x, y, label, value, colour):
    lw, vw = round(6.6 * len(label) + 16), round(6.6 * len(value) + 14)
    return (
        f'<g transform="translate({x},{y})" font-size="11" font-weight="600">'
        f'<rect width="{lw}" height="20" rx="3" fill="#555"/>'
        f'<rect x="{lw}" width="{vw}" height="20" rx="3" fill="{colour}"/>'
        f'<rect x="{lw - 3}" width="6" height="20" fill="{colour}"/>'
        f'<text x="8" y="14" fill="#fff">{html.escape(label)}</text>'
        f'<text x="{lw + 7}" y="14" fill="#fff">{html.escape(value)}</text></g>'
    ), lw + vw


def build():
    roads, effects = roadmap_groups(), effects_groups()
    (rd, rt, rp), (ed, et, ep) = share(roads), share(effects)
    width, height, pad, gap, top = 900, 460, 12, 16, 78
    box_w = (width - 2 * pad - gap) / 2
    box_h = height - top - pad
    b1, w1 = badge(pad, pad, "roadmap", f"{rp:.2f}%", colour_for(rp))
    b2, _ = badge(pad + w1 + 10, pad, "effects library", f"{ep:.2f}%", colour_for(ep))
    svg = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" font-family="Segoe UI, Helvetica, Arial, sans-serif" '
        f'role="img" aria-label="Roadmap items {rp:.2f}% done ({rd} of {rt}); effects library '
        f'entries {ep:.2f}% done ({ed} of {et})">',
        f'<rect width="{width}" height="{height}" rx="8" fill="{BACK}"/>',
        b1,
        b2,
        f'<text x="{pad}" y="{top - 8}" font-size="13" fill="{TEXT}">Roadmap items: '
        f"{rp:.2f}% ({rd}/{rt})</text>",
        f'<text x="{pad + box_w + gap}" y="{top - 8}" font-size="13" fill="{TEXT}">Effects '
        f"library: {ep:.2f}% ({ed}/{et})</text>",
        treemap(roads, pad, top, box_w, box_h),
        treemap(effects, pad + box_w + gap, top, box_w, box_h),
        "</svg>",
    ]
    block = (
        f"{START}\n"
        f"![Status: {rp:.2f}% of the roadmap items are done, {ep:.2f}% of the effects library "
        f"works](docs/images/status.svg)\n\n"
        f"Green is done, grey is still to do. Details: [roadmap](docs/roadmap.md) and "
        f"[effects library](docs/effects-library-status.md).\n"
        f"{END}"
    )
    return "\n".join(svg) + "\n", block


def new_readme(text, block):
    if START in text and END in text:
        a, b = text.index(START), text.index(END) + len(END)
        return text[:a] + block + text[b:]
    if ANCHOR not in text:
        sys.exit(f"status.py: {ANCHOR} not found in readme.md")
    return text.replace(ANCHOR, f"{ANCHOR}\n\n## Status\n\n{block}", 1)


def main():
    svg, block = build()
    readme = read(README)
    updated = new_readme(readme, block)
    current = read(SVG) if SVG.exists() else None
    if "--check" in sys.argv:
        stale = [n for n, bad in (("docs/images/status.svg", current != svg),
                                  ("readme.md", updated != readme)) if bad]
        if stale:
            print("out of date: " + ", ".join(stale) + " (run python3 scripts/status.py)")
            return 1
        print("status picture and readme block are up to date")
        return 0
    SVG.write_bytes(svg.encode("utf-8"))
    if updated != readme:
        README.write_bytes(updated.encode("utf-8"))
    print("status written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
