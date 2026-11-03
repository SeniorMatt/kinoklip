// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerRuler — the pixel ruler along the top or left edge of a viewer picture (View > Show
// Rulers); new guides are dragged out of it.
#pragma once

#include <QPointer>
#include <QWidget>

namespace kk {

class GuideOverlay;

/// One ruler beside a viewer's picture surface.
///
/// Units are frame px of the picture (the timeline resolution in timeline viewers, the clip's
/// in source viewers); 0 is the picture's top / left edge and the scale follows the viewer's
/// Fit / zoom mapping (GuideOverlay::frameToWidget()), so a tick sits exactly beside the
/// picture pixel it names. Labelled ticks are at least kLabelSpacing px apart (1, 2, 5 x 10^k
/// px steps). Guides are marked on the ruler in their colour.
///
/// Dragging from the top ruler adds a horizontal guide, from the left ruler a vertical one
/// (GuideOverlay::beginNewGuide()); releasing over a ruler again drops it. Disabled while the
/// guides are locked or hidden.
///
/// Thread-safety: GUI thread only.
class ViewerRuler : public QWidget
{
    Q_OBJECT
public:
    /// Thickness of a ruler (px).
    static constexpr int kThickness = 20;
    /// Minimum distance between two labelled ticks (px).
    static constexpr double kLabelSpacing = 56.0;

    /// @p orientation Qt::Horizontal = the top ruler (adds horizontal guides), Qt::Vertical =
    /// the left one.
    ViewerRuler(Qt::Orientation orientation, GuideOverlay* overlay, QWidget* parent = nullptr);

    [[nodiscard]] Qt::Orientation orientation() const { return m_orientation; }
    /// Ruler coordinate (px along the ruler) of frame position @p framePx.
    [[nodiscard]] double toRuler(double framePx) const;
    /// Frame position of ruler coordinate @p rulerPx (inverse of toRuler()).
    [[nodiscard]] double toFrame(double rulerPx) const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    /// Offset of the overlay's origin in this ruler's coordinates along the ruler.
    [[nodiscard]] double overlayOffset() const;
    [[nodiscard]] QPointF toOverlay(QPointF pos) const;

    Qt::Orientation m_orientation;
    QPointer<GuideOverlay> m_overlay;
    bool m_dragging = false;
};

} // namespace kk
