// SPDX-License-Identifier: GPL-3.0-or-later
// PanelSplitter — the splitter between the panels of a page, with handles that show they can
// be dragged (hover / press highlight, resize cursor, a grab zone wider than the drawn line).
#pragma once

#include <QSplitter>

namespace kk {

/// QSplitter for page layouts (Resolve's fixed panels resized by the gaps between them).
///
/// The handle draws a thin separator line in the palette's mid tone (the theme's
/// `QSplitter::handle` colour) and highlights it while the pointer is over it or it is being
/// dragged, so the gap reads as grabbable. The handle is wider than the drawn line
/// (kHandleGrabPx), which makes it easy to hit with a touchpad. Resizing uses QSplitter's
/// own press / move / release handling: an implicit pointer grab, no QWidget::grabMouse(), so
/// it works on Wayland.
/// Thread-safety: GUI thread only.
class PanelSplitter : public QSplitter
{
    Q_OBJECT
public:
    /// Creates a splitter that does not let its panels collapse.
    explicit PanelSplitter(Qt::Orientation orientation, QWidget* parent = nullptr);

    /// Width of the handle's grab zone, px (logical).
    static constexpr int kHandleGrabPx = 5;

    /// Smallest width (px, logical) a panel of a horizontal splitter can be dragged to, whatever
    /// its contents ask for. Without it a panel with many fixed-size controls (the Audio
    /// page's meters and analyzer row) lifts the whole page's minimum width above the window:
    /// the page is clipped, and the handles can no longer be dragged (issue #17).
    static constexpr int kMinPanelPx = 72;

protected:
    void childEvent(QChildEvent* event) override;
    QSplitterHandle* createHandle() override;

private:
    bool m_creatingHandle = false;
};

} // namespace kk
