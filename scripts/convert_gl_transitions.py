#!/usr/bin/env python3
"""Convert the gl-transitions collection (MIT) into the bundled Kinoklip plugin.

Usage: scripts/convert_gl_transitions.py <gl-transitions checkout> [out dir]
       (default out dir: plugins/bundled/dev.kinoklip.gl-transitions)

Every transitions/*.glsl is read: its header (Author, License, Description), the
`uniform` declarations with their `// = default` comments and the `// @range(min, max)`,
`// @param`, `// @color` notes become plugin.json parameters; the uniform lines are removed and
every use of a uniform `x` becomes `u_x` (a vec2 becomes `vec2(u_x_x, u_x_y)`, a colour vec3
`u_x.rgb`). Dropped (listed on stdout): licences other than MIT, texture inputs, links to
Shadertoy (its default licence is not GPL-compatible), anything unparsable.
"""
import json
import re
import sys
from pathlib import Path

PLUGIN_ID = "dev.kinoklip.gl-transitions"
DROP_TEXT = ("shadertoy.com",)

DECL = re.compile(
    r"^\s*uniform\s+(\w+)\s+(\w+)\s*(?:/\*\s*=\s*(.*?)\s*\*/)?\s*;\s*(?://\s*=?\s*(.*?))?\s*$")
RANGE = re.compile(r"^//\s*@range\(\s*([-\d.eE+]+)\s*,\s*([-\d.eE+]+)\s*(?:,\s*([-\d.eE+]+)\s*)?\)\s*(.*)$")
NOTE = re.compile(r"^//\s*@(param|color)\b\s*(.*)$")


def words(name):
    s = re.sub(r"[_\s]+", " ", name)
    s = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", s)
    s = re.sub(r"(?<=[A-Z])(?=[A-Z][a-z])", " ", s)
    return s.strip().title() if s.islower() else s.strip()[:1].upper() + s.strip()[1:]


def snake(name):
    s = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", name)
    s = re.sub(r"(?<=[A-Z])(?=[A-Z][a-z])", "_", s)
    return re.sub(r"[^a-z0-9]+", "_", s.lower()).strip("_")


def nums(text):
    text = re.sub(r"^\s*[iu]?vec\d\s*\(", "(", text or "")
    return [float(x) for x in re.findall(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?", text)]


def fmt(x):
    if abs(x - round(x)) < 1e-9 and abs(x) < 1e9:
        return int(round(x))
    return float(f"{x:.6g}")


def hexcolor(c):
    c = (list(c) + [1.0, 1.0, 1.0, 1.0])[:4] if len(c) < 4 else c[:4]
    v = [max(0, min(255, int(round(x * 255)))) for x in c]
    return "#%02x%02x%02x%02x" % (v[3], v[0], v[1], v[2]) if v[3] != 255 else "#%02x%02x%02x" % tuple(v[:3])


def short(text, n=200):
    text = " ".join(text.split())
    return text if len(text) <= n else text[: n - 1].rstrip() + "..."


def convert(path):
    """Return (manifest item, glsl text) or raise ValueError(reason)."""
    src = path.read_text(encoding="utf-8")
    head = {}
    for line in src.splitlines()[:8]:
        m = re.match(r"^//\s*(Author|License|Description)\s*:\s*(.*?)\s*$", line)
        if m:
            head[m.group(1)] = m.group(2)
    if head.get("License", "").upper() != "MIT":
        raise ValueError("licence " + head.get("License", "missing"))
    low = src.lower()
    for t in DROP_TEXT:
        if t in low:
            raise ValueError("mentions " + t)

    lines = src.splitlines()
    out, params, subs, pending = [], [], {}, {}
    for line in lines:
        s = line.strip()
        m = RANGE.match(s)
        if m:
            pending = {"range": (float(m.group(1)), float(m.group(2)), m.group(3)), "desc": m.group(4)}
            continue
        m = NOTE.match(s)
        if m:
            pending = {"color": m.group(1) == "color", "desc": m.group(2)}
            continue
        m = DECL.match(line)
        if not m:
            if s and not s.startswith("//"):
                pending = {}
            out.append(line)
            continue
        typ, name, d1, d2 = m.groups()
        default = d1 if d1 is not None else d2
        if typ == "sampler2D":
            raise ValueError("texture input")
        if default is None or not nums(default) and typ != "bool":
            raise ValueError("no default for " + name)
        label_src = pending.get("desc", "")
        key = snake(name)
        base = {"label": words(name)}
        if label_src:
            base["description"] = short(label_src)
        rng = pending.get("range")
        iscolor = pending.get("color", False)
        dv = nums(default)

        def number(k, lab, dflt, integer=False):
            lo, hi = (rng[0], rng[1]) if rng else (min(-1.0, 2 * dflt), max(1.0, 2 * dflt))
            if dflt < lo or dflt > hi:
                lo, hi = min(lo, dflt), max(hi, dflt)
            p = {"key": k, "label": lab, "type": "integer" if integer else "number",
                 "min": fmt(lo), "max": fmt(hi), "default": fmt(dflt)}
            if integer:
                p["step"] = 1
            elif rng and rng[2]:
                p["step"] = fmt(float(rng[2]))
            else:
                p["step"] = fmt(max((hi - lo) / 100.0, 0.001))
            return p

        if typ == "bool":
            params.append({**base, "key": key, "type": "boolean", "default": default.strip() == "true"})
            subs[name] = "u_" + key
        elif typ == "float":
            params.append({**base, **number(key, base["label"], dv[0])})
            params[-1].update(base)
            subs[name] = "u_" + key
        elif typ == "int":
            params.append({**number(key, base["label"], dv[0], True), **base})
            subs[name] = "u_" + key
        elif typ in ("vec3", "vec4") and iscolor:
            params.append({**base, "key": key, "type": "color", "default": hexcolor(dv)})
            subs[name] = "u_" + key + (".rgb" if typ == "vec3" else "")
        elif typ in ("vec2", "vec3", "vec4", "ivec2"):
            n = int(typ[-1])
            if len(dv) < n:
                dv = (dv * n)[:n]
            comps = []
            for i, c in enumerate("xyzw"[:n]):
                p = number(f"{key}_{c}", f"{base['label']} {c.upper()}", dv[i], typ == "ivec2")
                if label_src:
                    p["description"] = base["description"]
                params.append(p)
                comps.append(f"u_{key}_{c}")
            if typ == "ivec2":
                subs[name] = "ivec2(%s)" % ", ".join(comps)
            else:
                subs[name] = "vec%d(%s)" % (n, ", ".join(comps))
        else:
            raise ValueError("type " + typ)
        pending = {}

    text = "\n".join(out)
    # Replace each uniform's name where it is an identifier (not a member after a dot).
    for name, rep in subs.items():
        if rep.startswith("vec") or rep.startswith("ivec"):
            rep = "(" + rep + ")"
        pat = re.compile(r"(?<![\w.])" + re.escape(name) + r"\b")
        text = "\n".join(
            pat.sub(rep, c[0]) + (("//" + c[2]) if c[1] else "")
            for c in (l.partition("//") for l in text.split("\n")))
    if re.search(r"\buniform\b", text.replace("// ", "//")) and re.search(r"^\s*uniform\b", text, re.M):
        raise ValueError("uniform left")
    text = re.sub(r"\n{3,}", "\n\n", text).strip() + "\n"

    stem = path.stem
    item = {
        "id": snake(stem),
        "name": words(stem),
        "description": short(head.get("Description", "")),
        "group": "GL Transitions",
        "file": f"transitions/{stem}.glsl",
        "ported_from": f"gl-transitions/{stem}",
    }
    if params:
        item["params"] = params
    return item, text


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = Path(sys.argv[1])
    dest = Path(sys.argv[2] if len(sys.argv) > 2 else Path(__file__).resolve().parent.parent
                / "plugins/bundled" / PLUGIN_ID)
    (dest / "transitions").mkdir(parents=True, exist_ok=True)
    for old in (dest / "transitions").glob("*.glsl"):
        old.unlink()
    items, dropped = [], []
    for f in sorted((root / "transitions").glob("*.glsl")):
        try:
            item, text = convert(f)
        except ValueError as e:
            dropped.append(f"{f.stem}: {e}")
            continue
        (dest / item["file"]).write_text(text, encoding="utf-8")
        items.append(item)
    manifest = {
        "id": PLUGIN_ID,
        "name": "GL Transitions",
        "version": "1.0.0",
        "author": "gl-transitions contributors (converted for Kinoklip)",
        "license": "MIT",
        "description": "The MIT-licensed transitions of gl-transitions (gl-transitions.com) as Kinoklip transition plugins, "
                       "with their uniforms as parameters.",
        "api": 1,
        "transitions": items,
    }
    (dest / "plugin.json").write_text(json.dumps(manifest, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    lic = (root / "LICENSE").read_text(encoding="utf-8")
    (dest / "LICENSE").write_text(lic, encoding="utf-8")
    print(f"{len(items)} transitions written to {dest}")
    print("dropped:", *dropped, sep="\n  ")


if __name__ == "__main__":
    main()
