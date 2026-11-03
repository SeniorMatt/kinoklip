// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerRuler — tick drawing and the drag-out of new guides.
#include "ui/viewer/ViewerRuler.h"

#include "core/ViewerGuides.h"
#include "ui/common/SchemeColors.h"
#include "ui/viewer/GuideOverlay.h"
#include "ui/viewer/ViewerOverlaySettings.h"

#include <QMouseEvent>
#include <QPainter>

#include <cmath>

namespace kk {

namespace {
constexpr int kMajorTick = 8; ///< length of a labelled tick (px)
constexpr int kMinorTick = 4; ///< length of the ticks between labels (px)
constexpr int kMinorPerMajor = 5; ///< minor divisions of one label step (1 / 2 / 5 steps)
constexpr int kMarkerHalf = 4; ///< half width of a guide's mark on the ruler (px)
constexpr double kPictureBandAlpha = 0.18; ///< tint of the picture's extent on the ruler
} // namespace

ViewerRuler::ViewerRuler(Qt::Orientation orientation, GuideOverlay* overlay, QWidget* parent)
    : QWidget(parent)
    , m_orientation(orientation)
    , m_overlay(overlay)
{
    setObjectName(orientation == Qt::Horizontal ? QStringLiteral("ViewerRulerTop")
                                                : QStringLiteral("ViewerRulerLeft"));
    if (orientation == Qt::Horizontal)
        setFixedHeight(kThickness);
    else
        setFixedWidth(kThickness);
    setToolTip(orientation == Qt::Horizontal ? tr("Drag down to add a horizontal guide")
                                             : tr("Drag right to add a vertical guide"));
    if (overlay)
        connect(overlay, &GuideOverlay::changed, this, qOverload<>(&QWidget::update));
    connect(&ViewerOverlaySettings::instance(), &ViewerOverlaySettings::changed, this,
        qOverload<>(&QWidget::update));
}

QSize ViewerRuler::sizeHint() const
{
    return {kThickness, kThickness};
}

QSize ViewerRuler::minimumSizeHint() const
{
    return {kThickness, kThickness};
}

double ViewerRuler::overlayOffset() const
{
    if (!m_overlay)
        return 0;
    // Ruler and overlay are not ancestors of each other: compare through the window.
    const QPoint a = m_overlay->mapTo(window(), QPoint(0, 0));
    const QPoint b = mapTo(window(), QPoint(0, 0));
    return m_orientation == Qt::Horizontal ? a.x() - b.x() : a.y() - b.y();
}

QPointF ViewerRuler::toOverlay(QPointF pos) const
{
    return m_overlay ? QPointF(m_overlay->mapFromGlobal(mapToGlobal(pos))) : pos;
}

double ViewerRuler::toRuler(double framePx) const
{
    if (!m_overlay)
        return 0;
    const QTransform map = m_overlay->frameToWidget();
    // The top ruler measures x (along the picture width), the left one y.
    const double w = m_orientation == Qt::Horizontal ? map.map(QPointF(framePx, 0)).x()
                                                     : map.map(QPointF(0, framePx)).y();
    return w + overlayOffset();
}

double ViewerRuler::toFrame(double rulerPx) const
{
    if (!m_overlay)
        return 0;
    const QTransform inv = m_overlay->widgetToFrame();
    const double w = rulerPx - overlayOffset();
    return m_orientation == Qt::Horizontal ? inv.map(QPointF(w, 0)).x()
                                           : inv.map(QPointF(0, w)).y();
}

void ViewerRuler::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const SchemeColors& sc = schemeColors();
    p.fillRect(rect(), sc.ruler);
    if (!m_overlay)
        return;
    const QSize frame = m_overlay->frameSize();
    const bool top = m_orientation == Qt::Horizontal;
    const double extent = top ? frame.width() : frame.height();
    const double scale = toRuler(1) - toRuler(0); // ruler px per frame px
    if (extent <= 0 || scale <= 0)
        return;
    const int length = top ? width() : height();
    // The picture's extent, lightly tinted: where 0 and the far edge are.
    const double a = toRuler(0), b = toRuler(extent);
    const QRectF band = top ? QRectF(a, 0, b - a, height()) : QRectF(0, a, width(), b - a);
    p.fillRect(band, mixColors(sc.ruler, sc.text, kPictureBandAlpha));
    p.setPen(sc.line);
    if (top)
        p.drawLine(0, height() - 1, width(), height() - 1);
    else
        p.drawLine(width() - 1, 0, width() - 1, height());

    const double step = guides::rulerStep(scale, kLabelSpacing);
    const double minor = step / kMinorPerMajor;
    // Only the visible range: first and last tick on screen (the picture may be far larger
    // than the ruler when zoomed in).
    const double first = std::floor(toFrame(0) / minor) * minor;
    const double last = toFrame(length);
    QFont f = font();
    f.setPointSizeF(std::max(6.0, f.pointSizeF() * 0.8));
    p.setFont(f);
    const QFontMetrics fm(f);
    for (double v = first; v <= last; v += minor) {
        const double pos = std::floor(toRuler(v)) + 0.5;
        const bool major = std::abs(std::remainder(v, step)) < minor / 2;
        const int tick = major ? kMajorTick : kMinorTick;
        p.setPen(sc.textDim);
        if (top)
            p.drawLine(QPointF(pos, height() - tick), QPointF(pos, height()));
        else
            p.drawLine(QPointF(width() - tick, pos), QPointF(width(), pos));
        if (!major)
            continue;
        const QString label = QString::number(std::lround(v));
        p.setPen(sc.text);
        if (top) {
            p.drawText(QPointF(pos + 2, fm.ascent()), label);
        } else {
            // Rotated so labels fit the narrow left ruler.
            p.save();
            p.translate(fm.ascent(), pos - 2);
            p.rotate(-90);
            p.drawText(QPointF(0, 0), label);
            p.restore();
        }
    }
    // Guides of the axis this ruler measures: a vertical guide sits at an x (top ruler).
    const ViewerOverlayOptions& o = ViewerOverlaySettings::instance().options();
    if (!o.showGuides)
        return;
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(ViewerOverlaySettings::instance().guideColor());
    const Qt::Orientation marked = top ? Qt::Vertical : Qt::Horizontal;
    for (const ViewerGuide& g : m_overlay->guides()) {
        if (g.orientation != marked)
            continue;
        const double pos = toRuler(g.position);
        const QPolygonF tri = top
            ? QPolygonF({QPointF(pos - kMarkerHalf, 0), QPointF(pos + kMarkerHalf, 0),
                  QPointF(pos, kMarkerHalf * 1.5)})
            : QPolygonF({QPointF(0, pos - kMarkerHalf), QPointF(0, pos + kMarkerHalf),
                  QPointF(kMarkerHalf * 1.5, pos)});
        p.drawPolygon(tri);
    }
}

void ViewerRuler::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_overlay || !m_overlay->mouseEditable()) {
        event->ignore();
        return;
    }
    m_dragging = true;
    // The top ruler gives horizontal guides (a line parallel to it), the left one vertical.
    m_overlay->beginNewGuide(m_orientation, toOverlay(event->position()));
    setCursor(m_orientation == Qt::Horizontal ? Qt::SplitVCursor : Qt::SplitHCursor);
}

void ViewerRuler::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging && m_overlay)
        m_overlay->dragTo(toOverlay(event->position()), event->modifiers());
}

void ViewerRuler::mouseReleaseEvent(QMouseEvent* event)
{
    if (!m_dragging || event->button() != Qt::LeftButton)
        return;
    m_dragging = false;
    unsetCursor();
    if (m_overlay)
        m_overlay->finishDrag(toOverlay(event->position()));
}

} // namespace kk
