#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ts_fallback.py in.ts out.ts: copy a Qt Linguist file and add one extra context, "KinoklipAny",
that holds every translated text once, whatever its original context.

Why: lupdate files a text under the class that contains the tr() call, but many Kinoklip
widgets have no Q_OBJECT and many helpers forward to another class's tr(), so the context at run
time is a different one (QWidget, kk::PreferencesDialog, ...). src/core/Translations.cpp looks a
text up in its own context first and in "KinoklipAny" second, so such texts still get
translated. Used by the build only; the committed .ts files are not changed.
"""
import sys
import xml.etree.ElementTree as ET

ANY = "KinoklipAny"


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    tree = ET.parse(src)
    root = tree.getroot()
    seen = {}
    for ctx in root.findall("context"):
        for msg in ctx.findall("message"):
            s = msg.find("source")
            t = msg.find("translation")
            if s is None or t is None or t.get("type") in ("unfinished", "vanished", "obsolete"):
                continue
            if msg.find("comment") is not None:
                continue  # an explicit disambiguation: only its own context may use it
            forms = t.findall("numerusform")
            if msg.get("numerus") == "yes":
                if not forms or not all((f.text or "") for f in forms):
                    continue
            elif not (t.text or ""):
                continue
            seen.setdefault(s.text or "", msg)
    extra = ET.SubElement(root, "context")
    ET.SubElement(extra, "name").text = ANY
    for source, msg in seen.items():
        extra.append(msg)
    tree.write(dst, encoding="utf-8", xml_declaration=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
