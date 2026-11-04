#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Sanity check of the GitHub issue forms in .github/ISSUE_TEMPLATE: valid YAML with the keys
# GitHub needs, unique field ids, and the template files / field ids the app's
# Help > Report a Bug link uses (src/app/BugReport.cpp).
# Usage: scripts/check_issue_forms.py   (from anywhere; exit status 0 = fine)
# Without PyYAML the files are checked with a simple line parser (ids and top-level keys only).
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FORMS = ROOT / ".github" / "ISSUE_TEMPLATE"

# template file -> field ids the app prefills (keep in sync with src/app/BugReport.cpp)
APP_FIELDS = {
    "bug_report.yml": ["version", "environment"],
    "resolve_difference.yml": ["version", "environment"],
    "feature_request.yml": ["version"],
}
TYPES = {"markdown", "input", "textarea", "dropdown", "checkboxes"}
ID_RE = re.compile(r"^[A-Za-z0-9_-]+$")

errors = []


def check_with_yaml(path, yaml):
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    for key in ("name", "description", "body"):
        if key not in doc:
            errors.append(f"{path.name}: no '{key}'")
    ids = []
    for i, el in enumerate(doc.get("body") or []):
        t = el.get("type")
        if t not in TYPES:
            errors.append(f"{path.name}: body[{i}] has unknown type {t!r}")
        attrs = el.get("attributes") or {}
        if t != "markdown" and not attrs.get("label"):
            errors.append(f"{path.name}: body[{i}] ({t}) has no label")
        if t == "markdown" and not attrs.get("value"):
            errors.append(f"{path.name}: body[{i}] markdown has no value")
        if t == "dropdown" and not attrs.get("options"):
            errors.append(f"{path.name}: body[{i}] dropdown has no options")
        if "id" in el:
            ids.append(str(el["id"]))
    return ids


def check_simple(path):
    text = path.read_text(encoding="utf-8")
    for key in ("name", "description", "body"):
        if not re.search(rf"^{key}:", text, re.M):
            errors.append(f"{path.name}: no '{key}'")
    return re.findall(r"^\s+id:\s*(\S+)\s*$", text, re.M)


def main():
    try:
        import yaml  # noqa: PLC0415
    except ImportError:
        yaml = None
    forms = sorted(p for p in FORMS.glob("*.yml") if p.name != "config.yml")
    if not forms:
        errors.append(f"no issue forms in {FORMS}")
    for path in forms:
        ids = check_with_yaml(path, yaml) if yaml else check_simple(path)
        for i in ids:
            if not ID_RE.match(i):
                errors.append(f"{path.name}: bad id {i!r}")
        dupes = {i for i in ids if ids.count(i) > 1}
        if dupes:
            errors.append(f"{path.name}: duplicate ids {sorted(dupes)}")
        for field in APP_FIELDS.get(path.name, []):
            if field not in ids:
                errors.append(f"{path.name}: the app fills '{field}' but the form has no such id")
    for name in APP_FIELDS:
        if not (FORMS / name).exists():
            errors.append(f"the app links to {name}, which does not exist")
    config = FORMS / "config.yml"
    if yaml and config.exists():
        cfg = yaml.safe_load(config.read_text(encoding="utf-8"))
        for link in cfg.get("contact_links") or []:
            for key in ("name", "url", "about"):
                if not link.get(key):
                    errors.append(f"config.yml: contact link without '{key}'")
    for e in errors:
        print("error:", e, file=sys.stderr)
    print(f"{len(forms)} issue forms checked ({'PyYAML' if yaml else 'simple parser'}), "
          f"{len(errors)} problems")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
