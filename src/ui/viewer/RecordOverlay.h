// SPDX-License-Identifier: GPL-3.0-or-later
// RecordOverlay — the voice-over count-in and the "recording" mark over the viewer's picture.
#pragma once

#include <QWidget>

namespace kk {

class ViewerSurface;
class VoiceOverController;

/// Transparent child of a viewer surface: while the voice-over counts in it shows the seconds
/// left in large numbers in the middle (3, 2, 1); while it records, a red dot with "REC" in the
/// top left corner. Visible only then. Passes mouse events through.
///
/// Thread-safety: GUI thread only.
class RecordOverlay : public QWidget
{
    Q_OBJECT
public:
    RecordOverlay(VoiceOverController* controller, ViewerSurface* surface);

    /// The number shown in the middle (0 = none); tests.
    [[nodiscard]] int shownCount() const { return m_count; }
    [[nodiscard]] bool showsRecording() const { return m_recording; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void sync();

    VoiceOverController* m_controller;
    ViewerSurface* m_surface;
    int m_count = 0;
    bool m_recording = false;
};

} // namespace kk
