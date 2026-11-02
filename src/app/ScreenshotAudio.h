// SPDX-License-Identifier: GPL-3.0-or-later
// ScreenshotAudio — the "audio" screenshot target: a demo mix on the Audio page.
#pragma once

#include <QPixmap>
#include <QString>

class QWidget;

namespace kk {

class MainWindow;

/// Prepares the Audio page for a screenshot: imports KK_SCREENSHOT_IMPORT (colon-separated)
/// files, puts the first file with sound on A1 and the second (or the first again) on A2 if
/// the timeline is empty, gives the mix something to show (A1: EQ bell and a compressor, A2
/// panned and on a "Dialogue" bus, fader automation on A1), moves the playhead to the middle
/// and fills the meters with a reading mixed offline over the second before the playhead
/// (the same AudioMixer and loudness meter as playback; a headless run has no sound device).
/// "audio:eq" also opens A1's EQ window, "audio:dynamics" its dynamics window. Returns the
/// opened window (to be composited into the grab), or nullptr; @p error is set on failure
/// (the target then fails).
QWidget* prepareAudioScreenshot(MainWindow& window, const QString& section, QString* error);

/// The main window's grab with the floating editor window @p tool painted at its place.
[[nodiscard]] QPixmap grabWithToolWindow(QWidget& window, QWidget* tool);

} // namespace kk
