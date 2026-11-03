// SPDX-License-Identifier: GPL-3.0-or-later
// CurveEditor implementation: painting and point dragging.
#include "ui/inspector/CurveEditor.h"

#include "ui/common/SchemeColors.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

constexpr int kMargin = 6; ///< px around the graph (room for the point handles)
constexpr double kGrabRadius = 7.0; ///< px
constexpr int kHandle = 6; ///< point handle size, px
constexpr int kPreferredHeight = 120;
constexpr int kMinimumHeight = 80;
constexpr int kCurveSamples = 96;

} // namespace

CurveEditor::CurveEditor(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setToolTip(tr("Drag a point to bend the curve, click to add a point, double-click or "
                  "right-click a point to remove it"));
}

void CurveEditor::setPoints(const curve::Points& points)
{
    if (points == m_points)
        return;
    m_points = points;
    update();
}

void CurveEditor::setCurveColor(const QColor& color)
{
    m_color = color;
    update();
}

QSize CurveEditor::sizeHint() const
{
    return {kPreferredHeight * 2, kPreferredHeight};
}

QSize CurveEditor::minimumSizeHint() const
{
    return {kMinimumHeight, kPreferredHeight};
}

QRectF CurveEditor::graphRect() const
{
    // A square graph, centred (the curve maps 0..1 to 0..1).
    const double side = std::max(10.0, std::min(width(), height()) - 2.0 * kMargin);
    return {(width() - side) / 2.0, (height() - side) / 2.0, side, side};
}

curve::Point CurveEditor::toCurve(const QPointF& pos) const
{
    const QRectF r = graphRect();
    return {std::clamp((pos.x() - r.left()) / r.width(), 0.0, 1.0),
        std::clamp((r.bottom() - pos.y()) / r.height(), 0.0, 1.0)};
}

QPointF CurveEditor::toWidget(const curve::Point& p) const
{
    const QRectF r = graphRect();
    return {r.left() + p.x * r.width(), r.bottom() - p.y * r.height()};
}

int CurveEditor::pointAt(const QPointF& pos) const
{
    int best = -1;
    double bestDistance = kGrabRadius;
    for (int i = 0; i < m_points.size(); ++i) {
        const double d
            = std::hypot(toWidget(m_points[i]).x() - pos.x(), toWidget(m_points[i]).y() - pos.y());
        if (d <= bestDistance) {
            bestDistance = d;
            best = i;
        }
    }
    return best;
}

void CurveEditor::paintEvent(QPaintEvent*)
{
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = graphRect();
    p.fillRect(r, sc.view);
    p.setPen(QPen(sc.grid, 1));
    for (int i = 1; i < 4; ++i) {
        const double x = r.left() + r.width() * i / 4.0;
        const double y = r.top() + r.height() * i / 4.0;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
    p.setPen(QPen(sc.line, 1, Qt::DashLine));
    p.drawLine(r.bottomLeft(), r.topRight());
    p.setPen(QPen(sc.line, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);

    QPainterPath path;
    for (int i = 0; i <= kCurveSamples; ++i) {
        const double x = double(i) / kCurveSamples;
        const QPointF w = toWidget({x, curve::evaluate(m_points, x)});
        if (i == 0)
            path.moveTo(w);
        else
            path.lineTo(w);
    }
    const QColor line = m_color.isValid() ? m_color : sc.text;
    p.setPen(QPen(line, 1.5));
    p.drawPath(path);
    p.setPen(QPen(sc.text, 1));
    for (int i = 0; i < m_points.size(); ++i) {
        const QPointF w = toWidget(m_points[i]);
        p.setBrush(i == m_dragging ? sc.highlight : sc.view);
        p.drawRect(QRectF(w.x() - kHandle / 2.0, w.y() - kHandle / 2.0, kHandle, kHandle));
    }
}

void CurveEditor::mousePressEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    const int hit = pointAt(pos);
    if (event->button() == Qt::RightButton) {
        if (hit >= 0)
            removePoint(hit);
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    emit editStarted();
    if (hit >= 0) {
        m_dragging = hit;
    } else if (m_points.size() < curve::kMaxPoints) {
        // A new point where the user clicked (between its neighbours by x).
        const curve::Point c = toCurve(pos);
        int index = 0;
        while (index < m_points.size() && m_points[index].x < c.x)
            ++index;
        const bool tooClose = (index > 0 && c.x - m_points[index - 1].x < curve::kMinGap)
            || (index < m_points.size() && m_points[index].x - c.x < curve::kMinGap);
        if (tooClose) {
            m_dragging = -1;
            emit editFinished();
            return;
        }
        m_points.insert(index, c);
        m_dragging = index;
        emit pointsEdited(m_points);
    } else {
        emit editFinished();
        return;
    }
    update();
}

void CurveEditor::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging < 0) {
        setCursor(pointAt(event->position()) >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor);
        return;
    }
    curve::Point c = toCurve(event->position());
    // Keep the order: a point stays between its neighbours (kMinGap apart).
    const double lo = m_dragging > 0 ? m_points[m_dragging - 1].x + curve::kMinGap : 0.0;
    const double hi
        = m_dragging + 1 < m_points.size() ? m_points[m_dragging + 1].x - curve::kMinGap : 1.0;
    c.x = std::clamp(c.x, lo, std::max(lo, hi));
    if (c == m_points[m_dragging])
        return;
    m_points[m_dragging] = c;
    update();
    emit pointsEdited(m_points);
}

void CurveEditor::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_dragging < 0)
        return;
    m_dragging = -1;
    update();
    emit editFinished();
}

void CurveEditor::mouseDoubleClickEvent(QMouseEvent* event)
{
    const int hit = pointAt(event->position());
    if (hit >= 0)
        removePoint(hit);
}

void CurveEditor::removePoint(int index)
{
    if (m_points.size() <= curve::kMinPoints || index < 0 || index >= m_points.size())
        return;
    m_points.removeAt(index);
    m_dragging = -1;
    update();
    emit pointsCommitted(m_points);
}

} // namespace kk
