#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Rewrites docs/menu-reference.md with the application's own menu and panel reference.
# Usage: scripts/menu-reference.sh [path to the kinoklip binary]   (default: build/kinoklip)
here=$(cd "$(dirname "$0")/.." && pwd)
binary=${1:-$here/build/kinoklip}
QT_QPA_PLATFORM=offscreen KK_NO_AUDIO=1 "$binary" --menu-reference "$here/docs/menu-reference.md"
