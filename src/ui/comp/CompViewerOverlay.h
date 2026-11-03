// SPDX-License-Identifier: GPL-3.0-or-later
// CompViewerOverlay — on-viewer controls of the selected Compositing page node: centre, size
// and angle handles (Transform, Merge foreground), mask shape handles (Rectangle, Ellipse) and
// polygon points.
#pragma once

#include "ui/comp/CompAdapter.h"

#include <QPointer>
#include <QWidget>

#include <optional>

namespace kk {

class CompSession;
class ViewerSurface;

/// Transparent child of a CompViewer's picture surface (the TransformOverlay / MaskOverlay
/// concepts for composition nodes). It draws the controls of CompSession::currentNode() in
/// frame coordinates mapped through the surface's pictureRect() (Fit / zoom), so they stay on
/// the picture. Which parameters it edits comes from compadapter::viewerControls().
///
/// Each drag is one undo step (a CompSession gesture); an animated parameter gets a key at
/// the comp frame, like an Inspector edit. Esc during a drag cancels it. Locked nodes show the
/// controls without handles.
///
/// Thread-safety: GUI thread only.
class CompViewerOverlay : public QWidget
{
    Q_OBJECT
public:
    /// Which handle a point hits.
    enum class Handle {
        None,
        Centre, ///< drag moves the centre
        Size, ///< corner: uniform size (Transform) / width + height (masks)
        Width, ///< right edge (masks)
        Height, ///< bottom edge (masks)
        Angle, ///< the knob above the top edge
        Point ///< a polygon point (index in pointIndex)
    };

    /// Creates the overlay over @p surface (its widget becomes the parent).
    /// The overlay of viewer @p viewerIndex (0 = left, 1 = right): it draws handles only while
    /// that viewer shows a node (spec 42b.29).
    CompViewerOverlay(CompSession* session, ViewerSurface* surface, int viewerIndex = 0);

    /// True if the selected node has controls (the overlay then takes the mouse).
    [[nodiscard]] bool hasControls() const;
    /// Widget position of handle @p handle (polygon: point @p pointIndex), or nullopt.
    [[nodiscard]] std::optional<QPointF> handlePos(Handle handle, int pointIndex = 0) const;
    /// Handle under @p pos.
    [[nodiscard]] Handle handleAt(QPointF pos, int* pointIndex = nullptr) const;
    /// Re-evaluates visibility and repaints.
    void refresh();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// Values of the current node's controls at the comp frame.
    struct State
    {
        compadapter::ViewerControlKeys keys;
        QPointF centre {0.5, 0.5}; ///< normalised
        double size = 1.0;
        double width = 0.5, height = 0.5; ///< normalised
        double angleDeg = 0.0;
        QVector<QPointF>
            points; ///< Polygon: CompNode::path points (timeline px around the centre, y down)
        bool locked = false;
    };
    [[nodiscard]] std::optional<State> state() const;
    [[nodiscard]] QPointF toWidget(QPointF normalised) const;
    [[nodiscard]] QPointF toNormalised(QPointF widget) const;
    /// Half extents of the drawn box in widget pixels (before rotation).
    [[nodiscard]] QSizeF halfExtents(const State& s) const;
    /// Widget position of polygon point @p p (timeline px around the centre).
    [[nodiscard]] QPointF pointToWidget(const State& s, QPointF p) const;
    /// Widget pixels per timeline pixel.
    [[nodiscard]] double pixelScale() const;
    void applyDrag(QPointF pos);

    QPointer<CompSession> m_session;
    ViewerSurface* m_surface;
    int m_viewerIndex = 0;
    Handle m_drag = Handle::None;
    int m_dragPoint = -1;
    QPointF m_pressPos;
    std::optional<State> m_pressState;
};

} // namespace kk
