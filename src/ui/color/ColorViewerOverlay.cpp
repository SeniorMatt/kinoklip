// SPDX-License-Identifier: GPL-3.0-or-later
// ColorViewerOverlay — window handles, eyedropper, highlight and wipe over the Color viewer.
#include "ui/color/ColorViewerOverlay.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "render/TimelineEval.h"
#include "ui/color/ColorSession.h"
#include "ui/common/ClipGeometry.h"
#include "ui/viewer/ViewerSurface.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk {

namespace {

namespace wk = gradewindowkeys;

constexpr double kHandlePick = 8.0;
constexpr double kHandleSize = 7.0;
constexpr double kKnobGap = 22.0; ///< rotation knob above the top edge (widget px)
constexpr double kMinSoftHandle = 10.0; ///< soft handle at least this far out (widget px)

double length(QPointF p)
{
    return std::hypot(p.x(), p.y());
}

} // namespace

ColorViewerOverlay::ColorViewerOverlay(
    ColorSession* session, Project* project, ViewerSurface* surface)
    : QWidget(surface->widget())
    , m_session(session)
    , m_project(project)
    , m_surface(surface)
{
    setObjectName(QStringLiteral("ColorViewerOverlay"));
    setAttribute(Qt::WA_NoSystemBackground);
    setMouseTracking(true);
    setGeometry(surface->widget()->rect());
    surface->widget()->installEventFilter(this);
    if (m_session) {
        connect(m_session, &ColorSession::gradeChanged, this, &ColorViewerOverlay::refresh);
        connect(m_session, &ColorSession::selectedNodeChanged, this, &ColorViewerOverlay::refresh);
        connect(
            m_session, &ColorSession::selectedWindowChanged, this, &ColorViewerOverlay::refresh);
        connect(m_session, &ColorSession::clipChanged, this, &ColorViewerOverlay::refresh);
    }
    connect(project->timeline(), &Timeline::playheadChanged, this, &ColorViewerOverlay::refresh);
    refresh();
    show();
}

bool ColorViewerOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_surface->widget() && event->type() == QEvent::Resize)
        setGeometry(m_surface->widget()->rect());
    return QWidget::eventFilter(watched, event);
}

void ColorViewerOverlay::refresh()
{
    const bool interactive = m_pick || isWiping()
        || (m_windowControls && m_session && m_session->currentNode()
            && !m_session->currentNode()->windows.isEmpty());
    setAttribute(Qt::WA_TransparentForMouseEvents, !interactive);
    setCursor(m_pick ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

void ColorViewerOverlay::setWindowControls(bool on)
{
    m_windowControls = on;
    refresh();
}

void ColorViewerOverlay::setPickMode(bool on)
{
    m_pick = on;
    refresh();
}

void ColorViewerOverlay::setHighlightImage(const QImage& image)
{
    m_highlight = image;
    update();
}

void ColorViewerOverlay::setWipeImage(const QImage& image)
{
    m_wipe = image;
    refresh();
}

void ColorViewerOverlay::setWipePosition(double fraction)
{
    m_wipePos = std::clamp(fraction, 0.0, 1.0);
    update();
}

QRectF ColorViewerOverlay::pictureRect() const
{
    return QRectF(m_surface->pictureRect());
}

std::optional<QTransform> ColorViewerOverlay::pictureToWidget(QSizeF* pictureSize) const
{
    if (!m_session || !m_session->hasGrade())
        return std::nullopt;
    const TimelineData& data = m_project->timeline()->data();
    const QRectF pic = pictureRect();
    if (pic.isEmpty() || data.width <= 0 || data.height <= 0)
        return std::nullopt;
    const QSizeF frame(data.width, data.height);
    const QTransform frameToWidget
        = QTransform::fromScale(pic.width() / frame.width(), pic.height() / frame.height())
        * QTransform::fromTranslate(pic.x(), pic.y());
    if (m_session->target() == ColorSession::Target::Timeline) {
        if (pictureSize)
            *pictureSize = frame;
        return frameToWidget;
    }
    const ClipData* clip = editops::clipById(data, m_session->clipId());
    if (!clip)
        return std::nullopt;
    const QSizeF content = clipContentSize(m_project, *clip, frame);
    if (pictureSize)
        *pictureSize = content;
    // Content pixels (origin top-left) -> content centred -> frame (the compositor's clip
    // transform) -> widget.
    const int64_t local = clip->localFrame(m_project->timeline()->playhead());
    return QTransform::fromTranslate(-content.width() / 2, -content.height() / 2)
        * render::clipTransform(*clip, local, frame) * frameToWidget;
}

std::optional<QTransform> ColorViewerOverlay::windowToWidget(const GradeWindow& w) const
{
    QSizeF size;
    const auto toWidget = pictureToWidget(&size);
    if (!toWidget || !m_session)
        return std::nullopt;
    const int64_t f = m_session->gradeFrame();
    QTransform local;
    local.translate(
        w.value(wk::CenterX, f) * size.width(), w.value(wk::CenterY, f) * size.height());
    local.rotate(w.value(wk::Rotation, f));
    return local * *toWidget;
}

QPainterPath ColorViewerOverlay::windowPath(const GradeWindow& w, double grow) const
{
    QSizeF size;
    QPainterPath path;
    if (!pictureToWidget(&size) || !m_session)
        return path;
    const int64_t f = m_session->gradeFrame();
    const double hw = w.value(wk::Width, f) * size.width() / 2 + grow;
    const double hh = w.value(wk::Height, f) * size.height() / 2 + grow;
    switch (w.shape) {
    case GradeWindowShape::Circle:
        path.addEllipse(QPointF(0, 0), std::max(0.0, hw), std::max(0.0, hh));
        break;
    case GradeWindowShape::Linear:
        path.addRect(QRectF(-hw, -hh, 2 * hw, 2 * hh));
        break;
    case GradeWindowShape::Polygon: {
        const double pw = w.value(wk::Width, f) * size.width();
        const double ph = w.value(wk::Height, f) * size.height();
        QPolygonF poly;
        for (const QPointF& pt : w.points)
            poly << QPointF(pt.x() * pw, pt.y() * ph);
        if (grow != 0.0 && poly.size() >= 3) {
            // Grow about the centroid (an approximation of the soft edge, for display).
            QPointF c;
            for (const QPointF& pt : std::as_const(poly))
                c += pt;
            c /= poly.size();
            for (QPointF& pt : poly) {
                const QPointF d = pt - c;
                const double len = length(d);
                if (len > 1e-6)
                    pt += d / len * grow;
            }
        }
        if (!poly.isEmpty()) {
            poly << poly.first();
            path.addPolygon(poly);
        }
        break;
    }
    case GradeWindowShape::Gradient: {
        // The line through the centre; the selected side is above it.
        const double reach = std::hypot(size.width(), size.height());
        path.moveTo(-reach, -grow);
        path.lineTo(reach, -grow);
        break;
    }
    }
    return path;
}

QPointF ColorViewerOverlay::handlePos(const GradeWindow& w, Handle handle, int point) const
{
    QSizeF size;
    const auto toW = windowToWidget(w);
    if (!toW || !pictureToWidget(&size) || !m_session)
        return {};
    const int64_t f = m_session->gradeFrame();
    const double hw = w.value(wk::Width, f) * size.width() / 2;
    const double hh = w.value(wk::Height, f) * size.height() / 2;
    const double softOut = w.value(wk::SoftOut, f) * size.height();
    const bool gradient = w.shape == GradeWindowShape::Gradient;
    switch (handle) {
    case Handle::Move:
        return toW->map(QPointF(0, 0));
    case Handle::Width:
        return gradient ? QPointF() : toW->map(QPointF(hw, 0));
    case Handle::Height:
        return gradient ? QPointF() : toW->map(QPointF(0, hh));
    case Handle::Corner:
        return gradient || w.shape == GradeWindowShape::Polygon ? QPointF()
                                                                : toW->map(QPointF(hw, hh));
    case Handle::Rotate: {
        const QPointF top = toW->map(QPointF(0, gradient ? 0.0 : -hh));
        const QPointF centre = toW->map(QPointF(0, 0));
        QPointF up = toW->map(QPointF(0, -1)) - centre;
        const double len = length(up);
        if (len < 1e-9)
            return {};
        up /= len;
        return top + up * (gradient ? kKnobGap * 2 : kKnobGap);
    }
    case Handle::Soft: {
        // Left of the shape at the outer soft edge; a gradient's above its line, 30 px to the
        // side of the centre.
        const QPointF along = gradient ? QPointF(0, -1) : QPointF(-1, 0);
        const QPointF baseLocal = gradient ? QPointF(0, 0) : QPointF(-hw, 0);
        const QPointF base = toW->map(baseLocal);
        QPointF unit = toW->map(baseLocal + along) - base;
        const double ul = length(unit);
        if (ul < 1e-9)
            return {};
        unit /= ul;
        QPointF at = base + unit * std::max(softOut * ul, kMinSoftHandle);
        if (gradient) {
            QPointF side = toW->map(QPointF(1, 0)) - toW->map(QPointF(0, 0));
            const double sl = length(side);
            if (sl > 1e-9)
                at += side / sl * 30.0;
        }
        return at;
    }
    case Handle::Point:
        if (w.shape == GradeWindowShape::Polygon && point >= 0 && point < w.points.size()) {
            const QPointF pt = w.points[point];
            return toW->map(QPointF(pt.x() * hw * 2, pt.y() * hh * 2));
        }
        return {};
    case Handle::None:
        break;
    }
    return {};
}

ColorViewerOverlay::HandleHit ColorViewerOverlay::hitTest(QPointF pos) const
{
    HandleHit hit;
    if (!m_windowControls || !m_session)
        return hit;
    const GradeNode* n = m_session->currentNode();
    if (!n)
        return hit;
    auto near = [&](QPointF p) { return !p.isNull() && length(p - pos) <= kHandlePick; };
    // The selected window's handles first.
    if (const GradeWindow* w = m_session->window()) {
        if (w->shape == GradeWindowShape::Polygon) {
            for (int i = 0; i < w->points.size(); ++i) {
                if (near(handlePos(*w, Handle::Point, i)))
                    return {Handle::Point, w->id, i};
            }
        }
        for (const Handle h :
            {Handle::Rotate, Handle::Soft, Handle::Corner, Handle::Width, Handle::Height}) {
            if (near(handlePos(*w, h)))
                return {h, w->id, -1};
        }
    }
    // Inside a window (the selected one first) moves it; a gradient is grabbed at its line.
    QVector<const GradeWindow*> order;
    if (const GradeWindow* sel = m_session->window())
        order.append(sel);
    for (const GradeWindow& w : n->windows) {
        if (!order.contains(&w))
            order.append(&w);
    }
    for (const GradeWindow* w : std::as_const(order)) {
        const auto toW = windowToWidget(*w);
        if (!toW)
            continue;
        if (w->shape == GradeWindowShape::Gradient) {
            bool ok = false;
            const QTransform inv = toW->inverted(&ok);
            if (!ok)
                continue;
            const QPointF local = inv.map(pos);
            const double scale = std::sqrt(std::abs(toW->determinant()));
            if (std::abs(local.y()) * scale <= kHandlePick || near(handlePos(*w, Handle::Move)))
                return {Handle::Move, w->id, -1};
        } else if (toW->map(windowPath(*w, 0)).contains(pos)) {
            return {Handle::Move, w->id, -1};
        }
    }
    return hit;
}

QPointF ColorViewerOverlay::toFrameFraction(QPointF widget) const
{
    const QRectF pic = pictureRect();
    if (pic.isEmpty())
        return {};
    return QPointF(
        (widget.x() - pic.left()) / pic.width(), (widget.y() - pic.top()) / pic.height());
}

// ---- Painting ----

void ColorViewerOverlay::paintWindow(QPainter& p, const GradeWindow& w, bool selected) const
{
    const auto toW = windowToWidget(w);
    if (!toW || !m_session)
        return;
    QSizeF size;
    pictureToWidget(&size);
    const int64_t f = m_session->gradeFrame();
    const QColor main = !w.enabled ? QColor(160, 160, 160, 160)
        : selected                 ? QColor(240, 240, 240)
                                   : QColor(200, 200, 200, 170);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 120), 3));
    p.drawPath(toW->map(windowPath(w, 0)));
    p.setPen(QPen(main, selected ? 1.5 : 1.0));
    p.drawPath(toW->map(windowPath(w, 0)));
    // Soft edges, dashed.
    const double softOut = w.value(wk::SoftOut, f) * size.height();
    const double softIn = w.value(wk::SoftIn, f) * size.height();
    QPen dash(main, 1.0, Qt::DashLine);
    p.setPen(dash);
    if (softOut > 0.5)
        p.drawPath(toW->map(windowPath(w, softOut)));
    if (softIn > 0.5)
        p.drawPath(toW->map(windowPath(w, -softIn)));
    if (w.shape == GradeWindowShape::Gradient) {
        // Arrow towards the selected side.
        const QPointF c = toW->map(QPointF(0, 0));
        QPointF up = toW->map(QPointF(0, -1)) - c;
        const double len = length(up);
        if (len > 1e-9) {
            up /= len;
            p.setPen(QPen(main, 1.5));
            p.drawLine(c, c + up * 30);
            const QPointF side(-up.y(), up.x());
            p.drawLine(c + up * 30, c + up * 22 + side * 5);
            p.drawLine(c + up * 30, c + up * 22 - side * 5);
        }
    }
    if (!selected)
        return;
    auto square = [&](QPointF at, const QColor& fill) {
        if (at.isNull())
            return;
        p.setPen(QPen(QColor(0, 0, 0, 160), 1));
        p.setBrush(fill);
        p.drawRect(QRectF(
            at - QPointF(kHandleSize / 2, kHandleSize / 2), QSizeF(kHandleSize, kHandleSize)));
    };
    const QColor handle(245, 245, 245);
    square(handlePos(w, Handle::Move), handle);
    for (const Handle h : {Handle::Width, Handle::Height, Handle::Corner})
        square(handlePos(w, h), handle);
    if (w.shape == GradeWindowShape::Polygon) {
        for (int i = 0; i < w.points.size(); ++i)
            square(handlePos(w, Handle::Point, i), QColor(255, 210, 90));
    }
    const QPointF knob = handlePos(w, Handle::Rotate);
    if (!knob.isNull()) {
        p.setPen(QPen(main, 1));
        p.drawLine(handlePos(w, Handle::Move), knob);
        p.setBrush(handle);
        p.drawEllipse(knob, 4, 4);
    }
    const QPointF soft = handlePos(w, Handle::Soft);
    if (!soft.isNull()) {
        p.setPen(QPen(QColor(0, 0, 0, 160), 1));
        p.setBrush(QColor(120, 190, 255));
        QPolygonF diamond;
        diamond << soft + QPointF(0, -5) << soft + QPointF(5, 0) << soft + QPointF(0, 5)
                << soft + QPointF(-5, 0);
        p.drawPolygon(diamond);
    }
}

void ColorViewerOverlay::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF pic = pictureRect();
    if (!m_highlight.isNull() && !pic.isEmpty()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(pic, m_highlight);
    }
    if (!m_wipe.isNull() && !pic.isEmpty()) {
        const double x = pic.left() + pic.width() * m_wipePos;
        p.save();
        p.setClipRect(QRectF(pic.left(), pic.top(), x - pic.left(), pic.height()));
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(pic, m_wipe);
        p.restore();
        p.setPen(QPen(QColor(255, 255, 255, 200), 1.5));
        p.drawLine(QPointF(x, pic.top()), QPointF(x, pic.bottom()));
        p.setBrush(QColor(255, 255, 255, 220));
        p.drawEllipse(QPointF(x, pic.center().y()), 5, 5);
    }
    if (m_windowControls && m_session) {
        if (const GradeNode* n = m_session->currentNode()) {
            for (const GradeWindow& w : n->windows)
                paintWindow(p, w, w.id == m_session->selectedWindow());
        }
    }
    if (m_picking) {
        p.setPen(QPen(QColor(255, 255, 255, 220), 1, Qt::DashLine));
        p.setBrush(QColor(255, 255, 255, 30));
        p.drawRect(QRectF(m_pressPos, m_mousePos).normalized());
    }
}

// ---- Gestures ----

void ColorViewerOverlay::mousePressEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    m_pressPos = m_mousePos = pos;
    if (!m_session)
        return;
    if (m_pick && event->button() == Qt::LeftButton) {
        m_picking = true;
        update();
        return;
    }
    const HandleHit hit = hitTest(pos);
    if (event->button() == Qt::RightButton) {
        // Right-click on a polygon point deletes it (three stay).
        if (hit.handle == Handle::Point) {
            if (const GradeWindow* w = m_session->window(hit.window); w && w->points.size() > 3) {
                QVector<QPointF> pts = w->points;
                pts.remove(hit.pointIndex);
                m_session->setWindowPoints(hit.window, pts, tr("Delete Window Point"));
            }
        }
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    if (hit.handle != Handle::None) {
        if (hit.window != m_session->selectedWindow())
            m_session->selectWindow(hit.window);
        const GradeWindow* w = m_session->window(hit.window);
        const auto toW = pictureToWidget(&m_pressPicture);
        if (!w || !toW)
            return;
        m_drag = hit;
        m_pressWindow = *w;
        m_pressToWidget = *toW;
        m_session->beginGesture(tr("Window"));
        return;
    }
    if (isWiping()) {
        const QRectF pic = pictureRect();
        const double x = pic.left() + pic.width() * m_wipePos;
        if (std::abs(pos.x() - x) <= 10)
            m_dragWipe = true;
    }
}

void ColorViewerOverlay::applyDrag(QPointF pos)
{
    if (!m_session)
        return;
    const GradeWindow& w = m_pressWindow;
    const int64_t f = m_session->gradeFrame();
    const QSizeF size = m_pressPicture;
    bool ok = false;
    const QTransform toPicture = m_pressToWidget.inverted(&ok);
    if (!ok || size.isEmpty())
        return;
    const QPointF picPos = toPicture.map(pos);
    const QPointF picPress = toPicture.map(m_pressPos);
    const double cx = w.value(wk::CenterX, f), cy = w.value(wk::CenterY, f);
    const QPointF centrePx(cx * size.width(), cy * size.height());
    const double rot = w.value(wk::Rotation, f);
    QTransform local;
    local.translate(centrePx.x(), centrePx.y());
    local.rotate(rot);
    const QTransform toLocal = local.inverted();
    const QPointF lp = toLocal.map(picPos);
    const double w0 = w.value(wk::Width, f), h0 = w.value(wk::Height, f);
    switch (m_drag.handle) {
    case Handle::Move: {
        const QPointF d = picPos - picPress;
        m_session->setWindowParams(m_drag.window,
            {{wk::CenterX, cx + d.x() / size.width()}, {wk::CenterY, cy + d.y() / size.height()}},
            tr("Move Window"));
        break;
    }
    case Handle::Width:
        m_session->setWindowParams(
            m_drag.window, {{wk::Width, 2 * std::abs(lp.x()) / size.width()}}, tr("Window Size"));
        break;
    case Handle::Height:
        m_session->setWindowParams(
            m_drag.window, {{wk::Height, 2 * std::abs(lp.y()) / size.height()}}, tr("Window Size"));
        break;
    case Handle::Corner: {
        const QPointF lp0 = toLocal.map(picPress);
        const double k = length(lp) / std::max(1e-6, length(lp0));
        m_session->setWindowParams(
            m_drag.window, {{wk::Width, w0 * k}, {wk::Height, h0 * k}}, tr("Window Size"));
        break;
    }
    case Handle::Rotate: {
        const QPointF v = picPos - centrePx;
        if (length(v) < 1e-6)
            return;
        // Clockwise from straight up (y down).
        double deg = std::atan2(v.x(), -v.y()) * 180.0 / std::numbers::pi;
        if (QGuiApplication::keyboardModifiers() & Qt::ShiftModifier)
            deg = std::round(deg / 15.0) * 15.0;
        m_session->setWindowParams(m_drag.window, {{wk::Rotation, deg}}, tr("Rotate Window"));
        break;
    }
    case Handle::Soft: {
        double out = 0;
        if (w.shape == GradeWindowShape::Gradient)
            out = std::max(0.0, -lp.y());
        else
            out = std::max(0.0, std::abs(lp.x()) - w0 * size.width() / 2);
        m_session->setWindowParams(
            m_drag.window, {{wk::SoftOut, out / size.height()}}, tr("Window Softness"));
        break;
    }
    case Handle::Point: {
        QVector<QPointF> pts = w.points;
        if (m_drag.pointIndex < 0 || m_drag.pointIndex >= pts.size())
            return;
        const double pw = std::max(1e-6, w0 * size.width());
        const double ph = std::max(1e-6, h0 * size.height());
        pts[m_drag.pointIndex] = QPointF(lp.x() / pw, lp.y() / ph);
        m_session->setWindowPoints(m_drag.window, pts, tr("Move Window Point"));
        break;
    }
    case Handle::None:
        break;
    }
}

void ColorViewerOverlay::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    m_mousePos = pos;
    if (m_picking) {
        update();
        return;
    }
    if (m_dragWipe) {
        const QRectF pic = pictureRect();
        if (!pic.isEmpty())
            setWipePosition((pos.x() - pic.left()) / pic.width());
        return;
    }
    if (m_drag.handle != Handle::None) {
        applyDrag(pos);
        return;
    }
    // Hover feedback.
    const HandleHit hit = hitTest(pos);
    Qt::CursorShape shape = m_pick ? Qt::CrossCursor : Qt::ArrowCursor;
    switch (hit.handle) {
    case Handle::Move:
        shape = Qt::SizeAllCursor;
        break;
    case Handle::Width:
    case Handle::Height:
    case Handle::Corner:
    case Handle::Soft:
    case Handle::Point:
        shape = Qt::PointingHandCursor;
        break;
    case Handle::Rotate:
        shape = Qt::OpenHandCursor;
        break;
    case Handle::None:
        if (isWiping()) {
            const QRectF pic = pictureRect();
            if (std::abs(pos.x() - (pic.left() + pic.width() * m_wipePos)) <= 10)
                shape = Qt::SplitHCursor;
        }
        break;
    }
    setCursor(shape);
}

void ColorViewerOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (m_picking) {
        m_picking = false;
        QRectF r(toFrameFraction(m_pressPos), toFrameFraction(event->position()));
        r = r.normalized();
        if (std::hypot(
                event->position().x() - m_pressPos.x(), event->position().y() - m_pressPos.y())
            < 3.0)
            r = QRectF(toFrameFraction(m_pressPos), QSizeF(0, 0));
        update();
        emit pickRequested(r);
        return;
    }
    m_dragWipe = false;
    if (m_drag.handle != Handle::None) {
        m_drag = HandleHit {};
        if (m_session)
            m_session->endGesture();
    }
}

void ColorViewerOverlay::mouseDoubleClickEvent(QMouseEvent* event)
{
    // Double-click on a polygon edge inserts a point there.
    if (!m_windowControls || !m_session)
        return;
    const GradeWindow* w = m_session->window();
    if (!w || w->shape != GradeWindowShape::Polygon || w->points.size() >= kMaxWindowPoints)
        return;
    const QPointF pos = event->position();
    int best = -1;
    double bestDist = 8.0;
    QPointF bestPoint;
    for (int i = 0; i < w->points.size(); ++i) {
        const QPointF a = handlePos(*w, Handle::Point, i);
        const QPointF b = handlePos(*w, Handle::Point, (i + 1) % int(w->points.size()));
        const QPointF ab = b - a;
        const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
        if (len2 < 1e-9)
            continue;
        const double t = std::clamp(
            ((pos.x() - a.x()) * ab.x() + (pos.y() - a.y()) * ab.y()) / len2, 0.0, 1.0);
        const double d = length(a + ab * t - pos);
        if (d < bestDist) {
            bestDist = d;
            best = i;
            bestPoint
                = w->points[i] + (w->points[(i + 1) % int(w->points.size())] - w->points[i]) * t;
        }
    }
    if (best < 0)
        return;
    QVector<QPointF> pts = w->points;
    pts.insert(best + 1, bestPoint);
    m_session->setWindowPoints(w->id, pts, tr("Add Window Point"));
}

} // namespace kk
