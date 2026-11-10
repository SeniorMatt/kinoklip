#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Fails when docs/menu-reference.md differs from what the application writes itself
# (kinoklip --menu-reference), so a new menu entry or panel without a doc update fails the tests.
# Usage: scripts/check_menu_reference.py <path to the kinoklip binary>
#        scripts/check_menu_reference.py <binary> --update   (rewrite the committed file)
import difflib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COMMITTED = ROOT / "docs" / "menu-reference.md"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    update = "--update" in sys.argv[2:]
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", KK_NO_AUDIO="1")
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "menu-reference.md"
        result = subprocess.run([binary, "--menu-reference", str(out)], env=env,
                                capture_output=True, text=True, timeout=240)
        if result.returncode != 0 or not out.exists():
            print("kinoklip --menu-reference failed (%d):\n%s" % (result.returncode, result.stderr))
            return 1
        written = out.read_text(encoding="utf-8")
    if update:
        COMMITTED.write_text(written, encoding="utf-8")
        print("wrote", COMMITTED)
        return 0
    committed = COMMITTED.read_text(encoding="utf-8") if COMMITTED.exists() else ""
    if committed == written:
        return 0
    diff = difflib.unified_diff(committed.splitlines(), written.splitlines(),
                                "docs/menu-reference.md (committed)", "written by the app", lineterm="", n=1)
    print("docs/menu-reference.md is out of date. Regenerate it with:\n"
          "  scripts/menu-reference.sh <path to kinoklip>\n")
    print("\n".join(list(diff)[:60]))
    return 1


if __name__ == "__main__":
    sys.exit(main())
