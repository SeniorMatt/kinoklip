// SPDX-License-Identifier: GPL-3.0-or-later
// Screenshot mode (--screenshot): render the main window to a PNG and quit.
#pragma once

#include <QString>

namespace kk {

class MainWindow;

/// Sizes @p window to 1920x1080, shows page @p pageId, lets layouts settle, grabs the window
/// into @p pngPath and quits the event loop. Works with QT_QPA_PLATFORM=offscreen.
/// Returns the process exit code (0 = success). Runs the event loop itself.
int runScreenshot(MainWindow& window, const QString& pageId, const QString& pngPath);

} // namespace kk
