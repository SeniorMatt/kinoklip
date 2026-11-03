// SPDX-License-Identifier: GPL-3.0-or-later
// CurveEditor — the Inspector's small tone-curve graph for Curve parameters (Curves effect):
// drag points, click to add one, double-click or right-click a point to remove it.
#pragma once

#include "core/CurveParam.h"

#include <QColor>
#include <QWidget>

namespace kk {

/// Interactive curve graph. Input 0..1 left to right, output 0..1 bottom to top. Emits
/// editStarted() / pointsEdited() / editFinished() around a drag (one undo step) and
/// pointsCommitted() for single-step changes (add, remove). GUI thread only; owns no model
/// state beyond the points shown.
class CurveEditor final : public QWidget
{
    Q_OBJECT
public:
    explicit CurveEditor(QWidget* parent = nullptr);

    /// Shows @p points (no signal).
    void setPoints(const curve::Points& points);
    [[nodiscard]] const curve::Points& points() const { return m_points; }
    /// Colour of the curve line (the channel's colour).
    void setCurveColor(const QColor& color);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

    /// Graph position (0..1 each) of widget point @p pos, and back (for tests).
    [[nodiscard]] curve::Point toCurve(const QPointF& pos) const;
    [[nodiscard]] QPointF toWidget(const curve::Point& p) const;

signals:
    void editStarted();
    void pointsEdited(const kk::curve::Points& points);
    void editFinished();
    void pointsCommitted(const kk::curve::Points& points);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    /// Index of the point within the grab radius of @p pos, or -1.
    [[nodiscard]] int pointAt(const QPointF& pos) const;
    [[nodiscard]] QRectF graphRect() const;
    void removePoint(int index);

    curve::Points m_points = curve::identity();
    QColor m_color;
    int m_dragging = -1; ///< index of the dragged point
};

} // namespace kk
