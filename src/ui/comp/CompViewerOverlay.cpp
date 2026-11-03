// SPDX-License-Identifier: GPL-3.0-or-later
// CompViewerOverlay implementation: frame <-> widget mapping, handle hit tests, drags as
// CompSession gestures (one undo step each).
#include "ui/comp/CompViewerOverlay.h"

#include "ui/common/SchemeColors.h"
#include "ui/comp/CompSession.h"
#include "ui/viewer/ViewerSurface.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>

#include <cmath>
#include <numbers>

namespace kk {

namespace {

constexpr double kHandleHitPx = 8.0; ///< pick radius of a handle
constexpr double kHandleSizePx = 7.0; ///< drawn handle square
constexpr double kKnobDistancePx = 24.0; ///< rotation knob above the top edge

/// Screen rotation of a parameter angle (degrees counter-clockwise; screen y points down).
QTransform rotation(double angleDeg)
{
    QTransform t;
    t.rotate(-angleDeg);
    return t;
}

QVector<QPointF> parsePoints(const QString& text)
{
    QVector<QPointF> out;
    for (const QString& pair : text.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const QStringList xy = pair.split(QLatin1Char(','));
        if (xy.size() == 2)
            out.append(QPointF(xy[0].toDouble(), xy[1].toDouble()));
    }
    return out;
}

QString formatPoints(const QVector<QPointF>& points)
{
    QStringList parts;
    for (const QPointF& p : points)
        parts.append(QString::number(p.x(), 'g', 6) + QLatin1Char(',')
            + QString::number(p.y(), 'g', 6));
    return parts.join(QLatin1Char(' '));
}

double length(QPointF p)
{
    return std::hypot(p.x(), p.y());
}

} // namespace

CompViewerOverlay::CompViewerOverlay(CompSession* session, ViewerSurface* surface)
    : QWidget(surface->widget())
    , m_session(session)
    , m_surface(surface)
{
    setObjectName(QStringLiteral("CompViewerOverlay"));
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::ClickFocus);
    setGeometry(surface->widget()->rect());
    surface->widget()->installEventFilter(this);
    connect(session, &CompSession::selectionChanged, this, &CompViewerOverlay::refresh);
    connect(session, &CompSession::compositionChanged, this, &CompViewerOverlay::refresh);
    refresh();
    show();
}

bool CompViewerOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_surface->widget() && event->type() == QEvent::Resize)
        setGeometry(m_surface->widget()->rect());
    return QWidget::eventFilter(watched, event);
}

std::optional<CompViewerOverlay::State> CompViewerOverlay::state() const
{
    if (!m_session)
        return std::nullopt;
    const CompNode* n = m_session->composition().node(m_session->currentNode());
    if (!n)
        return std::nullopt;
    State s;
    s.keys = compadapter::viewerControls(n->type);
    if (s.keys.kind == compadapter::ViewerControl::None)
        return std::nullopt;
    const int64_t f = m_session->compFrame();
    auto value = [&](const QString& key, double fallback) {
        return key.isEmpty() ? fallback : n->params.value(key, f, fallback);
    };
    s.centre = QPointF(value(s.keys.centerX, 0.5), value(s.keys.centerY, 0.5));
    s.size = value(s.keys.size, 1.0);
    s.width = value(s.keys.width, 0.5);
    s.height = value(s.keys.height, 0.5);
    s.angleDeg = value(s.keys.angle, 0.0);
    if (!s.keys.points.isEmpty())
        s.points = parsePoints(n->params.staticValue(s.keys.points).toString());
    s.locked = n->locked;
    return s;
}

bool CompViewerOverlay::hasControls() const
{
    return state().has_value();
}

void CompViewerOverlay::refresh()
{
    const bool on = hasControls();
    setAttribute(Qt::WA_TransparentForMouseEvents, !on);
    update();
}

QPointF CompViewerOverlay::toWidget(QPointF n) const
{
    const QRectF p = m_surface->pictureRect();
    return p.topLeft() + QPointF(n.x() * p.width(), n.y() * p.height());
}

QPointF CompViewerOverlay::toNormalised(QPointF w) const
{
    const QRectF p = m_surface->pictureRect();
    if (p.isEmpty())
        return {};
    return QPointF((w.x() - p.left()) / p.width(), (w.y() - p.top()) / p.height());
}

QSizeF CompViewerOverlay::halfExtents(const State& s) const
{
    const QRectF p = m_surface->pictureRect();
    if (s.keys.kind == compadapter::ViewerControl::Transform)
        return QSizeF(p.width() * s.size / 2, p.height() * s.size / 2);
    return QSizeF(p.width() * s.width / 2, p.height() * s.height / 2);
}

std::optional<QPointF> CompViewerOverlay::handlePos(Handle handle, int pointIndex) const
{
    const std::optional<State> s = state();
    if (!s)
        return std::nullopt;
    if (s->keys.kind == compadapter::ViewerControl::Polygon) {
        if (handle != Handle::Point || pointIndex < 0 || pointIndex >= s->points.size())
            return std::nullopt;
        return toWidget(s->points[pointIndex]);
    }
    const QPointF c = toWidget(s->centre);
    const QSizeF h = halfExtents(*s);
    const QTransform r = rotation(s->angleDeg);
    switch (handle) {
    case Handle::Centre:
        return c;
    case Handle::Size:
        return c + r.map(QPointF(h.width(), h.height()));
    case Handle::Width:
        if (s->keys.kind == compadapter::ViewerControl::Transform)
            return std::nullopt;
        return c + r.map(QPointF(h.width(), 0));
    case Handle::Height:
        if (s->keys.kind == compadapter::ViewerControl::Transform)
            return std::nullopt;
        return c + r.map(QPointF(0, h.height()));
    case Handle::Angle:
        return c + r.map(QPointF(0, -h.height() - kKnobDistancePx));
    default:
        return std::nullopt;
    }
}

CompViewerOverlay::Handle CompViewerOverlay::handleAt(QPointF pos, int* pointIndex) const
{
    const std::optional<State> s = state();
    if (!s || s->locked)
        return Handle::None;
    if (s->keys.kind == compadapter::ViewerControl::Polygon) {
        for (int i = 0; i < s->points.size(); ++i) {
            if (length(toWidget(s->points[i]) - pos) <= kHandleHitPx) {
                if (pointIndex)
                    *pointIndex = i;
                return Handle::Point;
            }
        }
        return Handle::None;
    }
    for (Handle h : {Handle::Angle, Handle::Size, Handle::Width, Handle::Height, Handle::Centre}) {
        const std::optional<QPointF> p = handlePos(h);
        if (p && length(*p - pos) <= kHandleHitPx)
            return h;
    }
    // Anywhere inside the box moves the centre (like dragging a clip in the Edit viewer).
    const QPointF local = rotation(s->angleDeg).inverted().map(pos - toWidget(s->centre));
    const QSizeF h = halfExtents(*s);
    if (std::abs(local.x()) <= h.width() && std::abs(local.y()) <= h.height())
        return Handle::Centre;
    return Handle::None;
}

void CompViewerOverlay::paintEvent(QPaintEvent*)
{
    const std::optional<State> s = state();
    if (!s)
        return;
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // White over a dark halo: readable over any picture (the mask outline convention).
    auto stroke = [&p](const QPainterPath& path) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 160), 3.0));
        p.drawPath(path);
        p.setPen(QPen(QColor(255, 255, 255), 1.2));
        p.drawPath(path);
    };
    auto handle = [&](QPointF c) {
        if (s->locked)
            return;
        const QRectF r(c - QPointF(kHandleSizePx / 2, kHandleSizePx / 2),
            QSizeF(kHandleSizePx, kHandleSizePx));
        p.setPen(QPen(QColor(0, 0, 0, 160), 1.0));
        p.setBrush(sc.selection);
        p.drawRect(r);
    };
    if (s->keys.kind == compadapter::ViewerControl::Polygon) {
        QPainterPath path;
        for (int i = 0; i < s->points.size(); ++i) {
            const QPointF w = toWidget(s->points[i]);
            if (i == 0)
                path.moveTo(w);
            else
                path.lineTo(w);
        }
        if (s->points.size() > 2)
            path.closeSubpath();
        stroke(path);
        for (const QPointF& pt : s->points)
            handle(toWidget(pt));
        return;
    }
    const QPointF c = toWidget(s->centre);
    const QSizeF h = halfExtents(*s);
    QTransform t = rotation(s->angleDeg);
    t = t * QTransform::fromTranslate(c.x(), c.y());
    QPainterPath shape;
    const QRectF local(-h.width(), -h.height(), h.width() * 2, h.height() * 2);
    if (s->keys.kind == compadapter::ViewerControl::Ellipse)
        shape.addEllipse(local);
    else
        shape.addRect(local);
    stroke(t.map(shape));
    // Centre cross and the rotation knob on its stem.
    QPainterPath cross;
    cross.moveTo(c - QPointF(6, 0));
    cross.lineTo(c + QPointF(6, 0));
    cross.moveTo(c - QPointF(0, 6));
    cross.lineTo(c + QPointF(0, 6));
    stroke(cross);
    if (const auto knob = handlePos(Handle::Angle)) {
        QPainterPath stem;
        stem.moveTo(*handlePos(Handle::Angle));
        stem.lineTo(c + rotation(s->angleDeg).map(QPointF(0, -h.height())));
        stroke(stem);
        if (!s->locked) {
            p.setPen(QPen(QColor(0, 0, 0, 160), 1.0));
            p.setBrush(sc.selection);
            p.drawEllipse(*knob, kHandleSizePx / 2 + 1, kHandleSizePx / 2 + 1);
        }
    }
    for (Handle hd : {Handle::Size, Handle::Width, Handle::Height}) {
        if (const auto hp = handlePos(hd))
            handle(*hp);
    }
}

void CompViewerOverlay::mousePressEvent(QMouseEvent* event)
{
    int point = -1;
    const Handle h = event->button() == Qt::LeftButton ? handleAt(event->position(), &point)
                                                       : Handle::None;
    if (h == Handle::None || !m_session) {
        event->ignore();
        return;
    }
    static const char* const labels[] = {"", QT_TR_NOOP("Move"), QT_TR_NOOP("Resize"),
        QT_TR_NOOP("Resize"), QT_TR_NOOP("Resize"), QT_TR_NOOP("Rotate"), QT_TR_NOOP("Move Point")};
    m_drag = h;
    m_dragPoint = point;
    m_pressPos = event->position();
    m_pressState = state();
    const CompNode* n = m_session->composition().node(m_session->currentNode());
    m_session->beginGesture(tr("%1 %2").arg(tr(labels[int(h)]), n ? n->name : QString()));
    setFocus(Qt::MouseFocusReason);
}

void CompViewerOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (m_drag != Handle::None)
        applyDrag(event->position());
}

void CompViewerOverlay::applyDrag(QPointF pos)
{
    if (!m_pressState || !m_session)
        return;
    const State& s0 = *m_pressState;
    const QRectF pic = m_surface->pictureRect();
    if (pic.isEmpty())
        return;
    const QPointF c = toWidget(s0.centre);
    QMap<QString, double> scalars;
    QString pointsText;
    switch (m_drag) {
    case Handle::Centre: {
        const QPointF d = pos - m_pressPos;
        scalars.insert(s0.keys.centerX, s0.centre.x() + d.x() / pic.width());
        scalars.insert(s0.keys.centerY, s0.centre.y() + d.y() / pic.height());
        break;
    }
    case Handle::Size:
    case Handle::Width:
    case Handle::Height: {
        if (s0.keys.kind == compadapter::ViewerControl::Transform) {
            const double from = length(m_pressPos - c);
            if (from > 1)
                scalars.insert(s0.keys.size, std::max(0.0, s0.size * length(pos - c) / from));
            break;
        }
        const QPointF local = rotation(s0.angleDeg).inverted().map(pos - c);
        if (m_drag != Handle::Height)
            scalars.insert(s0.keys.width, 2 * std::abs(local.x()) / pic.width());
        if (m_drag != Handle::Width)
            scalars.insert(s0.keys.height, 2 * std::abs(local.y()) / pic.height());
        break;
    }
    case Handle::Angle: {
        // Screen angles grow clockwise (y down); parameter angles counter-clockwise.
        const QPointF a = m_pressPos - c, b = pos - c;
        const double delta = std::atan2(b.y(), b.x()) - std::atan2(a.y(), a.x());
        scalars.insert(s0.keys.angle, s0.angleDeg - delta * 180.0 / std::numbers::pi);
        break;
    }
    case Handle::Point: {
        QVector<QPointF> pts = s0.points;
        if (m_dragPoint >= 0 && m_dragPoint < pts.size())
            pts[m_dragPoint] = toNormalised(pos);
        pointsText = formatPoints(pts);
        break;
    }
    case Handle::None:
        return;
    }
    const quint64 id = m_session->currentNode();
    const int64_t frame = m_session->compFrame();
    const QString pointsKey = s0.keys.points;
    m_session->modifyLive([&](Composition& comp) {
        CompNode* n = comp.node(id);
        if (!n)
            return;
        for (auto it = scalars.cbegin(); it != scalars.cend(); ++it) {
            if (!it.key().isEmpty())
                n->params.animated(it.key(), it.value()).setValue(frame, it.value());
        }
        if (!pointsKey.isEmpty() && !pointsText.isNull())
            n->params.setStatic(pointsKey, pointsText);
    });
}

void CompViewerOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_drag == Handle::None) {
        event->ignore();
        return;
    }
    applyDrag(event->position());
    m_drag = Handle::None;
    m_pressState.reset();
    if (m_session)
        m_session->endGesture();
}

void CompViewerOverlay::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && m_drag != Handle::None && m_pressState) {
        // Cancel: put the values from the press back, then close the (now empty) gesture.
        const QPointF press = m_pressPos;
        applyDrag(press);
        m_drag = Handle::None;
        m_pressState.reset();
        if (m_session)
            m_session->endGesture();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace kk
