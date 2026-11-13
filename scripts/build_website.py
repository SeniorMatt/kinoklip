#!/usr/bin/env python3
"""Build the Kinoklip website (static, for GitHub Pages under /kinoklip/).

Uses only the Python standard library and the `markdown` module.

    python3 scripts/build_website.py              # writes _site/
    python3 scripts/build_website.py --out dir    # another output directory
    python3 scripts/build_website.py --check      # build, then check every link, image and tag
    python3 -m http.server -d _site               # look at it

Every link is relative, so the site works under any base path. Only 404.html needs an
absolute base (--base, default /kinoklip/).
"""
import argparse
import html
import json
import os
import re
import shutil
import sys
from html.parser import HTMLParser

try:
    import markdown
except ImportError:
    sys.exit("The 'markdown' module is missing: pip install markdown")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "website")
GUIDE = os.path.join(REPO, "docs", "guide")
RELEASES = os.path.join(REPO, "docs", "releases")
SHOTS = os.path.join(REPO, "docs", "images", "readme")  # the readme's screenshots, also used on the site
GITHUB = "https://github.com/SeniorMatt/kinoklip"

# Guide articles by topic. Anything not listed ends up under "More".
GROUPS = [
    ("Start here", ["getting-started", "use-the-shortcuts-of-your-old-editor", "smooth-playback"]),
    ("Editing", ["change-clip-speed", "freeze-a-frame", "transitions", "fade-to-black", "zoom-crop-and-picture-in-picture",
                 "keyframe-animation", "stabilize-a-shaky-clip", "track-and-warp-a-picture", "hide-part-of-the-picture",
                 "change-the-frame-rate-of-a-timeline", "vertical-video"]),
    ("Titles, subtitles and effects", ["add-layers-to-a-title", "emoji-in-titles-and-captions", "type-text-on-with-write-on",
                                       "add-subtitles", "test-patterns-and-looks", "camera-and-screen-looks",
                                       "sharpen-repair-and-damage-effects", "effects-that-use-other-frames"]),
    ("Compositing and color", ["compositing-page", "remove-a-green-screen", "remove-a-background", "colour-correct-a-clip",
                               "change-the-colour-of-one-object", "use-a-lut"]),
    ("Audio", ["record-a-voice-over", "fade-audio-and-add-music", "even-out-clip-volumes",
               "change-pitch-and-equalize-a-clip", "separate-audio-from-video"]),
    ("Deliver and exchange", ["export-for-youtube", "move-a-timeline-to-another-editor"]),
    ("Keyboard", ["vim-quick-start", "edit-with-vim-keys", "vim-cheat-sheet"]),
    ("Automation", ["control-kinoklip-from-the-command-line", "control-kinoklip-from-an-ai-assistant"]),
    ("Settings and help", ["move-your-settings-to-another-computer", "send-a-log-file-with-a-bug-report"]),
]

VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr", "path", "circle", "rect", "line", "stop"}


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


# ---------------------------------------------------------------- templates

def version_info():
    m = re.search(r"project\([^)]*VERSION\s+(\d+)\.(\d+)\.(\d+)", read(os.path.join(REPO, "CMakeLists.txt")))
    if not m:
        sys.exit("No VERSION in CMakeLists.txt")
    a, b, c = m.groups()
    full = "%s.%s.%s" % (a, b, c)
    short = "%s.%s" % (a, b) if c == "0" else full
    return full, short


class Site:
    def __init__(self, out, base):
        self.out = out
        self.base = base
        self.version, self.version_short = version_info()
        self.partials = {}
        pdir = os.path.join(SRC, "partials")
        for name in os.listdir(pdir):
            self.partials[os.path.splitext(name)[0]] = read(os.path.join(pdir, name)).rstrip("\n")
        self.layout = read(os.path.join(SRC, "layout.html"))

    def expand(self, text, depth=0):
        def sub(m):
            name = m.group(1)
            if name not in self.partials:
                sys.exit("Unknown partial: " + name)
            return self.expand(self.partials[name], depth + 1) if depth < 5 else ""
        return re.sub(r"\{\{>\s*([\w-]+)\s*\}\}", sub, text)

    def fill(self, text, ctx):
        text = self.expand(text)

        def sub(m):
            if m.group(1) not in ctx:
                sys.exit("Unknown placeholder: " + m.group(0))
            return ctx[m.group(1)]
        return re.sub(r"\{\{(\w+)\}\}", sub, text)

    def page(self, rel, title, description, body, nav=None, root=None, body_is_template=False, extra=None):
        """Write one page. rel is the output path like 'features/index.html'."""
        depth = rel.count("/")
        root = root if root is not None else "../" * depth
        ctx = {"root": root, "version": self.version, "version_short": self.version_short}
        for k in ("features", "guide", "download", "report"):
            ctx["cur_" + k] = ' aria-current="page"' if nav == k else ""
        ctx.update(extra or {})
        if body_is_template:
            body = self.fill(body, ctx)
        header = self.fill(self.partials["header"], ctx)
        footer = self.fill(self.partials["footer"], ctx)
        page = self.layout
        for k, v in (("title", html.escape(title)), ("description", html.escape(description, quote=True)), ("root", root),
                     ("header", header), ("footer", footer)):
            page = page.replace("{{%s}}" % k, v)
        page = page.replace("{{body}}", body)
        write(os.path.join(self.out, rel), page)


# ---------------------------------------------------------------- markdown

def gh_slug(value, sep="-"):
    value = re.sub(r"<[^>]+>", "", value).lower().strip()
    value = re.sub(r"[^\w\- ]", "", value, flags=re.UNICODE)
    return value.replace(" ", sep)


LIST_RE = re.compile(r"^(\s*)([-*+]|\d+[.)])\s+")


def normalize_md(text):
    """GitHub accepts 2 and 3 space list indents and lists right after a paragraph; Python-Markdown wants 4 and a blank line."""
    lines = text.split("\n")
    out = []
    stack = [0]
    fence = None  # (old_indent, new_indent)
    prev = ""
    for line in lines:
        stripped = line.strip()
        indent = len(line) - len(line.lstrip(" "))
        if fence is not None:
            if stripped.startswith("```"):
                out.append(" " * fence[1] + stripped)
                fence = None
            else:
                out.append(" " * fence[1] + line[min(indent, fence[0]):] if stripped else "")
            prev = line
            continue
        if not stripped:
            out.append("")
            prev = ""
            continue
        while stack[-1] > indent:
            stack.pop()
        if indent > stack[-1]:
            stack.append(indent)
        new = 4 * (len(stack) - 1) + (indent - stack[-1])
        if indent == 0:
            stack = [0]
        is_list = bool(LIST_RE.match(line))
        is_table = stripped.startswith("|")
        if prev.strip() and (is_list or is_table) and not LIST_RE.match(prev) and not prev.lstrip().startswith("|") \
                and (len(prev) - len(prev.lstrip(" "))) == 0 and indent == 0 and out and out[-1] != "":
            out.append("")
        if is_list and prev.strip() and (len(prev) - len(prev.lstrip(" "))) > indent and out and out[-1] != "":
            out.append("")
        if stripped.startswith("```"):
            fence = (indent, new)
            out.append(" " * new + stripped)
        else:
            out.append(" " * new + line.lstrip(" "))
        prev = line
    return "\n".join(out)


def render_md(text):
    md = markdown.Markdown(extensions=["tables", "fenced_code", "toc", "sane_lists", "attr_list"],
                           extension_configs={"toc": {"slugify": gh_slug, "permalink": False}})
    return md.convert(normalize_md(text))


class LinkFixer:
    """Rewrites links in rendered markdown: guide articles to site pages, images to copied files, other repo files to GitHub."""

    def __init__(self, src_dir, root, article_files, warnings):
        self.src_dir = src_dir
        self.root = root
        self.article_files = article_files  # abs path -> id
        self.warnings = warnings

    def fix(self, target, is_img):
        if re.match(r"^([a-z][a-z0-9+.-]*:|#|//)", target, re.I):
            return target
        path, _, frag = target.partition("#")
        frag = "#" + frag if frag else ""
        if not path:
            return target
        full = os.path.normpath(os.path.join(self.src_dir, html.unescape(path)))
        if full in self.article_files:
            return "%sguide/%s/%s" % (self.root, self.article_files[full], frag)
        if full == os.path.join(GUIDE, "readme.md"):
            return "%sguide/%s" % (self.root, frag)
        gi = os.path.join(GUIDE, "images") + os.sep
        if is_img and full.startswith(gi):
            return "%sguide/images/%s" % (self.root, os.path.relpath(full, gi))
        rel = os.path.relpath(full, REPO)
        if rel.startswith(".."):
            self.warnings.append("link leaves the repo: " + target)
            return target
        if not os.path.exists(full):
            self.warnings.append("missing repo file: %s (from %s)" % (rel, self.src_dir))
        kind = "tree" if os.path.isdir(full) else "blob"
        return "%s/%s/main/%s%s" % (GITHUB, kind, rel.replace(os.sep, "/"), frag)

    def apply(self, htmltext):
        def sub(m):
            attr, q, val = m.group(1), m.group(2), m.group(3)
            return '%s=%s%s%s' % (attr, q, self.fix(val, attr == "src"), q)
        return re.sub(r'\b(href|src)=(["\'])(.*?)\2', sub, htmltext)


# ---------------------------------------------------------------- guide

def build_guide(site, warnings):
    index = json.loads(read(os.path.join(GUIDE, "index.json")))
    articles = index["articles"]
    # An article that is not in index.json yet still gets a page (title and summary taken from the file).
    known = {a["file"] for a in articles}
    for f in sorted(os.listdir(GUIDE)):
        if f.endswith(".md") and f not in known and f not in ("readme.md", "topics.md"):
            text = read(os.path.join(GUIDE, f))
            title = re.match(r"\s*# (.+)", text)
            para = re.search(r"^(?!#)(\S.*(?:\n\S.*)*)", text.split("\n", 1)[1].lstrip(), re.M)
            summary = re.sub(r"\s+", " ", para.group(1)).split(". ")[0].rstrip(".") + "." if para else ""
            summary = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", summary).replace("**", "")
            articles.append({"id": f[:-3], "title": title.group(1) if title else f[:-3], "file": f, "summary": summary, "keywords": []})
            warnings.append("%s is not in index.json; using its first paragraph as summary" % f)
    by_id = {a["id"]: a for a in articles}
    article_files = {os.path.join(GUIDE, a["file"]): a["id"] for a in articles}
    order = []
    groups = []
    for name, ids in GROUPS:
        ids = [i for i in ids if i in by_id]
        groups.append((name, ids))
        order += ids
    rest = [a["id"] for a in articles if a["id"] not in order]
    if rest:
        groups.append(("More", rest))
        order += rest
    for name, ids in GROUPS:
        for i in ids:
            if i not in by_id:
                warnings.append("guide group names unknown article: " + i)

    # images
    shutil.copytree(os.path.join(GUIDE, "images"), os.path.join(site.out, "guide", "images"), dirs_exist_ok=True)

    # index page
    data = [{"id": a["id"], "title": a["title"], "summary": a["summary"], "keywords": a.get("keywords", [])} for a in articles]
    parts = []
    for name, ids in groups:
        items = "\n".join(
            '<li data-gid="%s"><a href="%s/"><strong>%s</strong><span>%s</span></a></li>'
            % (i, i, html.escape(by_id[i]["title"]), html.escape(by_id[i]["summary"])) for i in ids)
        parts.append('<section class="g-group" aria-labelledby="g-%s"><h2 id="g-%s">%s</h2><ul class="g-list">\n%s\n</ul></section>'
                     % (gh_slug(name), gh_slug(name), html.escape(name), items))
    blob = json.dumps(data).replace("</", "<\\/")
    body = """<section class="page-head"><div class="wrap">
<p class="kicker">Guide</p>
<h1>How to use Kinoklip</h1>
<p class="lead">Short how-to articles, step by step. Kinoklip is laid out like DaVinci Resolve with the same shortcuts, so most Resolve tutorials for the Edit page also work here. New? Start with <a href="getting-started/">Getting started</a>.</p>
<div class="search" role="search"><label class="sr" for="guide-q" style="position:absolute;left:-9999px">Search the guide</label><input id="guide-q" type="search" data-search placeholder="Search the guide: green screen, export, LUT, voice-over..." autocomplete="off"></div>
<p class="search-count" data-search-count role="status" aria-live="polite"></p>
</div></section>
<section class="section tight" style="padding-top:0"><div class="wrap">
%s
<p class="muted" style="margin-top:56px">Press <kbd>F1</kbd> in Kinoklip to open the article about what you are working on. Every shortcut is in the <a href="%s/blob/main/docs/keymap-resolve-windows.md">Resolve keymap</a>; the other editors' presets are in <a href="%s/blob/main/docs/keymaps/readme.md">keymaps</a>.</p>
</div></section>
<script type="application/json" id="guide-data">%s</script>""" % ("\n".join(parts), GITHUB, GITHUB, blob)
    site.page("guide/index.html", "Guide - Kinoklip", "How to use Kinoklip: short step-by-step articles for the Edit, Compositing, Color, Audio and Deliver pages.", body, nav="guide")

    # sidebar (relative to the article page: ../<id>/)
    def sidebar(current):
        out = ['<aside class="side"><details open><summary>All articles</summary><nav aria-label="Guide articles"><a href="../">Guide home and search</a>']
        for name, ids in groups:
            out.append("<h2>%s</h2>" % html.escape(name))
            for i in ids:
                cur = ' aria-current="page"' if i == current else ""
                out.append('<a href="../%s/"%s>%s</a>' % (i, cur, html.escape(by_id[i]["title"])))
        out.append("</nav></details></aside>")
        return "\n".join(out)

    for n, aid in enumerate(order):
        a = by_id[aid]
        src = read(os.path.join(GUIDE, a["file"]))
        src = re.sub(r"\A\s*# .*\n", "", src, count=1)
        fixer = LinkFixer(GUIDE, "../../", article_files, warnings)
        content = fixer.apply(render_md(src))
        prev = by_id[order[n - 1]] if n > 0 else None
        nxt = by_id[order[n + 1]] if n + 1 < len(order) else None
        pager = '<nav class="pager" aria-label="Next and previous article">%s%s</nav>' % (
            '<a class="btn btn-quiet" href="../%s/">&larr; %s</a>' % (prev["id"], html.escape(prev["title"])) if prev else "<span></span>",
            '<a class="btn btn-quiet" href="../%s/">%s &rarr;</a>' % (nxt["id"], html.escape(nxt["title"])) if nxt else "")
        body = """<div class="wrap"><div class="guide-layout">
%s
<article class="prose">
<p style="margin:0 0 8px"><a href="../">&larr; Guide</a></p>
<h1>%s</h1>
%s
%s
<p class="edit-link">Found a mistake? <a href="%s/blob/main/docs/guide/%s">Edit this article on GitHub</a>.</p>
</article></div></div>""" % (sidebar(aid), html.escape(a["title"]), content, pager, GITHUB, a["file"])
        site.page("guide/%s/index.html" % aid, "%s - Kinoklip guide" % a["title"], a["summary"], body, nav="guide")
    return by_id


# ---------------------------------------------------------------- releases

def semver_key(name):
    m = re.match(r"v(\d+)\.(\d+)\.(\d+)(?:-([a-z]+)\.(\d+))?$", name)
    if not m:
        return None
    a, b, c, tag, num = m.groups()
    pre = 0 if tag else 1  # a pre-release sorts below its final release
    tagrank = {"alpha": 0, "beta": 1}.get(tag or "", 2)
    return (int(a), int(b), int(c), pre, tagrank, int(num or 0))


def release_list():
    items = []
    for f in os.listdir(RELEASES):
        base = os.path.splitext(f)[0]
        key = semver_key(base)
        if key:
            items.append((key, base, os.path.join(RELEASES, f)))
    items.sort(reverse=True)
    return items


def short_text(t):
    t = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", t)
    t = t.replace("**", "").replace("`", "").strip()
    if len(t) > 110:
        if ":" in t:
            t = t.split(":")[0]
        if len(t) > 110:
            t = t[:105].rsplit(" ", 1)[0] + "..."
    return t


def highlights(path):
    """Two short lists from the newest release notes: sections that start with 'New'."""
    secs = []
    cur = None
    for line in read(path).split("\n"):
        if line.startswith("### "):
            title = line[4:].strip()
            cur = {"title": title, "items": []} if title.startswith("New") else None
            if cur:
                secs.append(cur)
        elif line.startswith("## "):
            cur = None
        elif cur is not None and re.match(r"^[-*] ", line):
            cur["items"].append(short_text(line[2:]))
    out = []
    for s in secs[:2]:
        lis = "".join("<li>%s</li>" % html.escape(i) for i in s["items"][:5])
        out.append('<div><h3>%s</h3><ul>%s</ul></div>' % (html.escape(s["title"]), lis))
    return "\n".join(out)


def build_releases(site, article_files, warnings):
    items = release_list()
    parts = []
    for n, (key, base, path) in enumerate(items):
        text = read(path)
        fixer = LinkFixer(RELEASES, "../", article_files, warnings)
        content = fixer.apply(render_md(text))
        ver = base[1:]
        first = re.search(r"^### (.+)$", text, re.M)
        sub = html.escape(re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", first.group(1))) if first else ""
        pill = ' <span class="pill">Latest</span>' if n == 0 else ""
        parts.append('<details class="rel" id="%s"%s><summary><b>%s</b>%s<span>%s</span></summary><div class="prose">%s</div></details>'
                     % (base, " open" if n == 0 else "", html.escape(ver), pill, sub, content))
    body = """<section class="page-head"><div class="wrap">
<p class="kicker">Releases</p><h1>Release notes</h1>
<p class="lead">What changed in each version, newest first. To get a version, see the <a href="../download/">download page</a> or the <a href="%s/releases">releases on GitHub</a>.</p>
</div></section>
<section class="section tight" style="padding-top:8px"><div class="wrap" style="max-width:920px">%s</div></section>""" % (GITHUB, "\n".join(parts))
    site.page("releases/index.html", "Release notes - Kinoklip", "What changed in each Kinoklip release.", body)
    return items


# ---------------------------------------------------------------- build

def build(out, base):
    warnings = []
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    site = Site(out, base)
    for d in ("css", "js", "static"):
        shutil.copytree(os.path.join(SRC, d), os.path.join(out, d))
    shots = os.path.join(out, "static", "shots")
    os.makedirs(shots)
    for f in sorted(os.listdir(SHOTS)):
        if f.endswith(".png"):
            shutil.copy(os.path.join(SHOTS, f), os.path.join(shots, f))
    write(os.path.join(out, ".nojekyll"), "")

    by_id = build_guide(site, warnings)
    article_files = {os.path.join(GUIDE, a["file"]): a["id"] for a in by_id.values()}
    items = build_releases(site, article_files, warnings)
    latest = highlights(items[0][2]) if items else ""

    def tpl(name):
        return read(os.path.join(SRC, "pages", name + ".html"))
    site.page("index.html", "Kinoklip: a free video editor for Linux, laid out like Resolve",
              "Kinoklip is a free, open source Qt and FFmpeg video editor for Linux with DaVinci Resolve's layout and shortcuts.",
              tpl("home"), body_is_template=True, extra={"highlights": latest})
    site.page("features/index.html", "Features - Kinoklip", "Editing, compositing, color, audio, export, performance, Vim mode, command line and keymaps in Kinoklip.",
              tpl("features"), nav="features", body_is_template=True)
    site.page("download/index.html", "Download Kinoklip for Linux", "Download the Kinoklip AppImage, how to run it, and what your computer needs.",
              tpl("download"), nav="download", body_is_template=True)
    site.page("report-a-bug/index.html", "Report a bug - Kinoklip", "How to report a Kinoklip bug: save a log file, then open a prefilled GitHub issue.",
              tpl("report"), nav="report", body_is_template=True)
    notfound = """<section class="page-head"><div class="wrap"><p class="kicker">404</p><h1>This page is not here</h1>
<p class="lead">The page may have moved. Try the <a href="{{root}}">home page</a> or the <a href="{{root}}guide/">guide</a>.</p></div></section>"""
    site.page("404.html", "Page not found - Kinoklip", "Page not found.", notfound, root=base, body_is_template=True)

    for w in sorted(set(warnings)):
        print("warning:", w)
    print("Built %s (Kinoklip %s): %d guide articles, %d releases" % (out, site.version, len(by_id), len(items)))
    return warnings


# ---------------------------------------------------------------- check

class Scan(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.links = []
        self.ids = set()
        self.stack = []
        self.errors = []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if "id" in a:
            self.ids.add(a["id"])
        for k in ("href", "src"):
            if k in a and a[k] is not None:
                self.links.append((k, a[k], self.getpos()[0]))
        if tag not in VOID:
            self.stack.append((tag, self.getpos()[0]))

    def handle_startendtag(self, tag, attrs):
        a = dict(attrs)
        if "id" in a:
            self.ids.add(a["id"])
        for k in ("href", "src"):
            if k in a and a[k] is not None:
                self.links.append((k, a[k], self.getpos()[0]))

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        if not self.stack or self.stack[-1][0] != tag:
            self.errors.append("line %d: unexpected </%s> (open: %s)" % (self.getpos()[0], tag, self.stack[-1] if self.stack else None))
            for i in range(len(self.stack) - 1, -1, -1):
                if self.stack[i][0] == tag:
                    del self.stack[i:]
                    break
        else:
            self.stack.pop()


def check(out, base):
    pages = {}
    for dp, _, files in os.walk(out):
        for f in files:
            if f.endswith(".html"):
                p = os.path.join(dp, f)
                s = Scan()
                s.feed(read(p))
                s.close()
                for tag, line in s.stack:
                    s.errors.append("line %d: <%s> never closed" % (line, tag))
                pages[p] = s
    bad = 0
    for p, s in sorted(pages.items()):
        rel = os.path.relpath(p, out)
        for e in s.errors:
            print("%s: %s" % (rel, e))
            bad += 1
        for kind, link, line in s.links:
            if re.match(r"^([a-z][a-z0-9+.-]*:|//)", link, re.I):
                continue
            path, _, frag = link.partition("#")
            if link.startswith("/"):
                if rel != "404.html" or not link.startswith(base):
                    print("%s:%d: absolute link %s" % (rel, line, link))
                    bad += 1
                    continue
                path = link[len(base):].partition("#")[0]
                target = os.path.join(out, path)
            elif not path:
                target = p
            else:
                target = os.path.normpath(os.path.join(os.path.dirname(p), path.split("?")[0]))
            if os.path.isdir(target):
                target = os.path.join(target, "index.html")
            if not os.path.exists(target):
                print("%s:%d: broken %s %s" % (rel, line, kind, link))
                bad += 1
                continue
            if frag and target in pages and frag not in pages[target].ids:
                print("%s:%d: no anchor #%s in %s" % (rel, line, frag, os.path.relpath(target, out)))
                bad += 1
    print("Checked %d pages: %s" % (len(pages), "all fine" if not bad else "%d problems" % bad))
    return bad


def main():
    ap = argparse.ArgumentParser(description="Build the Kinoklip website")
    ap.add_argument("--out", default=os.path.join(REPO, "_site"))
    ap.add_argument("--base", default="/kinoklip/", help="absolute base path, used only by 404.html")
    ap.add_argument("--check", action="store_true", help="check links, images, anchors and tags after the build")
    args = ap.parse_args()
    build(args.out, args.base)
    if args.check and check(args.out, args.base):
        sys.exit(1)


if __name__ == "__main__":
    main()
