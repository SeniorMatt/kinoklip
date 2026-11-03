// SPDX-License-Identifier: GPL-3.0-or-later
// Screenshot mode (--screenshot): render the main window to a PNG and quit.
#pragma once

#include <QString>

namespace kk {

class MainWindow;

/// Sizes @p window to 1920x1080, shows @p target, lets layouts settle, grabs it into
/// @p pngPath and quits the event loop. Works with QT_QPA_PLATFORM=offscreen.
/// Targets: a page id (media, cut, edit, deliver); "cut:tape" / "cut:clip" / "cut:strip" (the Cut
/// page with its viewer in Source Tape / Source Clip mode, or the media pool's strip view; an
/// empty timeline first gets the media pool's files appended); "preferences" / "keyboard" (that
/// dialog over the Edit page; only the dialog is saved); "inspector" (first video clip selected) /
/// "mediapool" (only that Edit-page panel is saved); "menu-color-theme" / "menu-style" (Settings
/// menu with the submenu open, cropped to the menus); "import-dialog" (File > Import Media open,
/// the whole screen grabbed: shows the desktop's portal dialog on X11). User guide targets
/// (scripts/guide_screenshots.py): "effects:<id>" (that library item selected), "fx:<effect
/// id>[:inspector|viewer]", "change-speed", "timeline", "deliver:inout" / "deliver:queue".
/// KK_SCREENSHOT_GEOMETRY=<file.json> also writes where the widgets are in the picture
/// (ScreenshotGeometry.h), KK_SCREENSHOT_WINDOW=1 saves the window for panel targets,
/// KK_SCREENSHOT_DELAY_MS waits longer before the grab. Returns the process exit code
/// (0 = success). Runs the event loop itself.
int runScreenshot(MainWindow& window, const QString& target, const QString& pngPath);

} // namespace kk
