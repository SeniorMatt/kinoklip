// SPDX-License-Identifier: GPL-3.0-or-later
// ColorViewerOverlay — window handles, eyedropper, highlight and wipe over the Color viewer.
#include "ui/color/ColorViewerOverlay.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "render/TimelineEval.h"
#include "core/GradeMath.h"
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
    const bool interactive = m_pick || isWiping() || m_chartOn
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

void ColorViewerOverlay::setChart(bool on, const colormatch::ChartQuad& quad)
{
    m_chartOn = on;
    m_chartQuad = quad;
    refresh();
}

QPointF ColorViewerOverlay::chartCornerPos(int index) const
{
    const QRectF pic = pictureRect();
    const QPointF c = m_chartQuad.corner[size_t(index)];
    return QPointF(pic.left() + c.x() * pic.width(), pic.top() + c.y() * pic.height());
}

int ColorViewerOverlay::chartHit(QPointF pos) const
{
    if (!m_chartOn || pictureRect().isEmpty())
        return -1;
    int best = -1;
    double bestDist = kChartHandleRadius;
    for (int i = 0; i < 4; ++i) {
        const QPointF c = chartCornerPos(i);
        const double d = std::hypot(c.x() - pos.x(), c.y() - pos.y());
        if (d <= bestDist) {
            bestDist = d;
            best = i;
        }
    }
    if (best >= 0)
        return best;
    QPolygonF poly;
    for (int i = 0; i < 4; ++i)
        poly << chartCornerPos(i);
    return poly.containsPoint(pos, Qt::OddEvenFill) ? 4 : -1;
}

void ColorViewerOverlay::paintChart(QPainter& p) const
{
    // The patch grid, dark under light so it shows on any picture, and the central half of
    // every patch (where the colour is measured) dotted.
    auto at = [&](double u, double v) {
        const QPointF f = colormatch::chartPoint(m_chartQuad, u, v);
        const QRectF pic = pictureRect();
        return QPointF(pic.left() + f.x() * pic.width(), pic.top() + f.y() * pic.height());
    };
    for (int pass = 0; pass < 2; ++pass) {
        p.setPen(pass == 0 ? QPen(QColor(0, 0, 0, 200), 3) : QPen(QColor(255, 255, 255, 235), 1));
        for (int i = 0; i <= colormatch::kChartColumns; ++i) {
            const double u = double(i) / colormatch::kChartColumns;
            p.drawLine(at(u, 0), at(u, 1));
        }
        for (int j = 0; j <= colormatch::kChartRows; ++j) {
            const double v = double(j) / colormatch::kChartRows;
            p.drawLine(at(0, v), at(1, v));
        }
    }
    p.setPen(QPen(QColor(255, 255, 255, 160), 1, Qt::DotLine));
    p.setBrush(Qt::NoBrush);
    for (int row = 0; row < colormatch::kChartRows; ++row) {
        for (int col = 0; col < colormatch::kChartColumns; ++col) {
            const double u0 = (col + 0.25) / colormatch::kChartColumns;
            const double u1 = (col + 0.75) / colormatch::kChartColumns;
            const double v0 = (row + 0.25) / colormatch::kChartRows;
            const double v1 = (row + 0.75) / colormatch::kChartRows;
            QPolygonF cell;
            cell << at(u0, v0) << at(u1, v0) << at(u1, v1) << at(u0, v1);
            p.drawPolygon(cell);
        }
    }
    for (int i = 0; i < 4; ++i) {
        p.setPen(QPen(QColor(0, 0, 0, 220), 1.5));
        p.setBrush(QColor(255, 200, 60, 235));
        p.drawEllipse(chartCornerPos(i), kChartHandleRadius * 0.7, kChartHandleRadius * 0.7);
    }
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
        * render::clipTransform(*clip, local, frame, true, m_project->timeline()->data().fps)
        * frameToWidget;
}

GradeWindow ColorViewerOverlay::shown(const GradeWindow& window) const
{
    GradeWindow w = window;
    if (m_session && !w.shapeKeys.isEmpty()) {
        // The shape at the playhead (the handle code reads points only).
        w.points = w.shapeAt(m_session->gradeFrame());
        w.shapeKeys.clear();
    }
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    QSizeF size;
    if (!n || !n->track.hasData() || !pictureToWidget(&size) || size.height() <= 0)
        return w;
    return grademath::trackedWindow(
        *n, w, m_session->gradeFrame(), size.width() / size.height());
}

bool ColorViewerOverlay::setShownParams(
    const QVector<std::pair<QString, double>>& values, const QString& label)
{
    const GradeNode* n = m_session ? m_session->currentNode() : nullptr;
    QSizeF size;
    if (!n || !n->track.hasData() || !pictureToWidget(&size) || size.height() <= 0)
        return m_session && m_session->setWindowParams(m_drag.window, values, label);
    const int64_t f = m_session->gradeFrame();
    const GradeWindow& w = m_pressWindow; // already shown
    grademath::WindowGeometry g;
    g.cx = w.value(wk::CenterX, f);
    g.cy = w.value(wk::CenterY, f);
    g.width = w.value(wk::Width, f);
    g.height = w.value(wk::Height, f);
    g.rotation = w.value(wk::Rotation, f);
    bool centre = false;
    for (const auto& [key, v] : values) {
        if (key == wk::CenterX) {
            g.cx = v;
            centre = true;
        } else if (key == wk::CenterY) {
            g.cy = v;
            centre = true;
        } else if (key == wk::Width) {
            g.width = v;
        } else if (key == wk::Height) {
            g.height = v;
        } else if (key == wk::Rotation) {
            g.rotation = v;
        }
    }
    const grademath::WindowGeometry r
        = grademath::untrackedWindowGeometry(n->track, g, f, size.width() / size.height());
    QVector<std::pair<QString, double>> out;
    for (const auto& [key, v] : values) {
        if (key == wk::CenterX || key == wk::CenterY)
            continue;
        double stored = v;
        if (key == wk::Width)
            stored = r.width;
        else if (key == wk::Height)
            stored = r.height;
        else if (key == wk::Rotation)
            stored = r.rotation;
        out.append(std::pair<QString, double>(key, stored));
    }
    if (centre) {
        out.append(std::pair<QString, double>(wk::CenterX, r.cx));
        out.append(std::pair<QString, double>(wk::CenterY, r.cy));
    }
    return m_session->setWindowParams(m_drag.window, out, label);
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
    case GradeWindowShape::Polygon:
    case GradeWindowShape::Curve: {
        const double pw = w.value(wk::Width, f) * size.width();
        const double ph = w.value(wk::Height, f) * size.height();
        const bool curve = w.shape == GradeWindowShape::Curve;
        const int n = int(w.points.size());
        QVector<QPointF> pos(n);
        QPointF centre;
        for (int i = 0; i < n; ++i) {
            pos[i] = QPointF(w.points[i].pos.x() * pw, w.points[i].pos.y() * ph);
            centre += pos[i];
        }
        if (n > 0)
            centre /= n;
        if (grow != 0.0 && n >= 3) {
            // Grow about the centroid (an approximation of the soft edge, for display).
            for (QPointF& pt : pos) {
                const QPointF d = pt - centre;
                const double len = length(d);
                if (len > 1e-6)
                    pt += d / len * grow;
            }
        }
        if (n == 0)
            break;
        path.moveTo(pos[0]);
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            if (curve && (w.points[i].hasHandles() || w.points[j].hasHandles())) {
                const QPointF c0 = pos[i]
                    + QPointF(w.points[i].outHandle.x() * pw, w.points[i].outHandle.y() * ph);
                const QPointF c1 = pos[j]
                    + QPointF(w.points[j].inHandle.x() * pw, w.points[j].inHandle.y() * ph);
                path.cubicTo(c0, c1, pos[j]);
            } else {
                path.lineTo(pos[j]);
            }
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
        return gradient || w.shape == GradeWindowShape::Polygon || w.shape == GradeWindowShape::Curve
            ? QPointF()
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
    case Handle::TangentIn:
    case Handle::TangentOut: {
        const bool curve = w.shape == GradeWindowShape::Curve;
        if (!(w.shape == GradeWindowShape::Polygon || curve) || point < 0
            || point >= w.points.size() || (handle != Handle::Point && !curve))
            return {};
        const GradeWindowPoint& gp = w.points[point];
        QPointF pt = gp.pos;
        if (handle == Handle::TangentIn)
            pt += gp.inHandle;
        else if (handle == Handle::TangentOut)
            pt += gp.outHandle;
        return toW->map(QPointF(pt.x() * hw * 2, pt.y() * hh * 2));
    }
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
    if (const GradeWindow* raw = m_session->window()) {
        const GradeWindow shownWindow = shown(*raw);
        const GradeWindow* w = &shownWindow;
        if (w->shape == GradeWindowShape::Curve && m_selPoint >= 0 && m_selPoint < w->points.size()) {
            // The selected point's tangent handles (only those with a length: a corner is
            // switched to smooth by a double-click first).
            const GradeWindowPoint& gp = w->points[m_selPoint];
            if (!gp.outHandle.isNull() && near(handlePos(*w, Handle::TangentOut, m_selPoint)))
                return {Handle::TangentOut, w->id, m_selPoint};
            if (!gp.inHandle.isNull() && near(handlePos(*w, Handle::TangentIn, m_selPoint)))
                return {Handle::TangentIn, w->id, m_selPoint};
        }
        if (w->shape == GradeWindowShape::Polygon || w->shape == GradeWindowShape::Curve) {
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
    QVector<GradeWindow> order;
    if (const GradeWindow* sel = m_session->window())
        order.append(shown(*sel));
    for (const GradeWindow& raw : n->windows) {
        if (raw.id != m_session->selectedWindow())
            order.append(shown(raw));
    }
    for (const GradeWindow& shownWindow : std::as_const(order)) {
        const GradeWindow* w = &shownWindow;
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
    if (w.shape == GradeWindowShape::Polygon || w.shape == GradeWindowShape::Curve) {
        if (w.shape == GradeWindowShape::Curve && m_selPoint >= 0 && m_selPoint < w.points.size()) {
            const QPointF at = handlePos(w, Handle::Point, m_selPoint);
            for (const Handle t : {Handle::TangentIn, Handle::TangentOut}) {
                const QPointF tip = handlePos(w, t, m_selPoint);
                if (length(tip - at) < 0.5)
                    continue;
                p.setPen(QPen(QColor(255, 210, 90, 200), 1));
                p.drawLine(at, tip);
                p.setPen(QPen(QColor(0, 0, 0, 160), 1));
                p.setBrush(QColor(255, 210, 90));
                p.drawEllipse(tip, 3.5, 3.5);
            }
        }
        for (int i = 0; i < w.points.size(); ++i) {
            const bool sel = w.shape == GradeWindowShape::Curve && i == m_selPoint;
            // Smooth points are drawn round, corners square; the selected one is brighter.
            if (w.shape == GradeWindowShape::Curve && w.points[i].smooth) {
                const QPointF at = handlePos(w, Handle::Point, i);
                p.setPen(QPen(QColor(0, 0, 0, 160), 1));
                p.setBrush(sel ? QColor(255, 240, 160) : QColor(255, 210, 90));
                p.drawEllipse(at, kHandleSize / 2 + 0.5, kHandleSize / 2 + 0.5);
            } else {
                square(handlePos(w, Handle::Point, i),
                    sel ? QColor(255, 240, 160) : QColor(255, 210, 90));
            }
        }
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
    if (m_chartOn && !pic.isEmpty())
        paintChart(p);
    if (m_windowControls && m_session) {
        if (const GradeNode* n = m_session->currentNode()) {
            for (const GradeWindow& w : n->windows)
                paintWindow(p, shown(w), w.id == m_session->selectedWindow());
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
    if (m_chartOn && event->button() == Qt::LeftButton) {
        m_chartDrag = chartHit(pos);
        m_chartPressQuad = m_chartQuad;
        return;
    }
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
            m_session->removeWindowPoint(hit.window, hit.pointIndex, tr("Delete Window Point"));
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
        if (hit.handle == Handle::Point || hit.handle == Handle::TangentIn
            || hit.handle == Handle::TangentOut) {
            m_selPoint = hit.pointIndex;
            update();
        }
        m_drag = hit;
        m_pressWindow = shown(*w);
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
        setShownParams(
            {{wk::CenterX, cx + d.x() / size.width()}, {wk::CenterY, cy + d.y() / size.height()}},
            tr("Move Window"));
        break;
    }
    case Handle::Width:
        setShownParams({{wk::Width, 2 * std::abs(lp.x()) / size.width()}}, tr("Window Size"));
        break;
    case Handle::Height:
        setShownParams({{wk::Height, 2 * std::abs(lp.y()) / size.height()}}, tr("Window Size"));
        break;
    case Handle::Corner: {
        const QPointF lp0 = toLocal.map(picPress);
        const double k = length(lp) / std::max(1e-6, length(lp0));
        setShownParams({{wk::Width, w0 * k}, {wk::Height, h0 * k}}, tr("Window Size"));
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
        setShownParams({{wk::Rotation, deg}}, tr("Rotate Window"));
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
        GradeWindowShapePoints pts = w.points;
        if (m_drag.pointIndex < 0 || m_drag.pointIndex >= pts.size())
            return;
        const double pw = std::max(1e-6, w0 * size.width());
        const double ph = std::max(1e-6, h0 * size.height());
        // The handles are offsets: they travel with the point.
        pts[m_drag.pointIndex].pos = QPointF(lp.x() / pw, lp.y() / ph);
        m_session->setWindowShape(m_drag.window, pts, tr("Move Window Point"));
        break;
    }
    case Handle::TangentIn:
    case Handle::TangentOut: {
        GradeWindowShapePoints pts = w.points;
        const int i = m_drag.pointIndex;
        if (i < 0 || i >= pts.size())
            return;
        const double pw = std::max(1e-6, w0 * size.width());
        const double ph = std::max(1e-6, h0 * size.height());
        GradeWindowPoint& pt = pts[i];
        const QPointF t = QPointF(lp.x() / pw, lp.y() / ph) - pt.pos;
        const bool in = m_drag.handle == Handle::TangentIn;
        const QPointF other = in ? pt.outHandle : pt.inHandle;
        if (QGuiApplication::keyboardModifiers() & Qt::AltModifier)
            pt.smooth = false; // break the tangent
        QPointF mirrored = other;
        if (pt.smooth) {
            // The other side stays collinear and keeps its length (as the mask editor does).
            const double lt = std::hypot(t.x() * pw, t.y() * ph);
            const double lo = std::hypot(other.x() * pw, other.y() * ph);
            const double keep = lo > 1e-9 ? lo : lt;
            mirrored = lt > 1e-9 ? -t * (keep / lt) : other;
        }
        (in ? pt.inHandle : pt.outHandle) = t;
        (in ? pt.outHandle : pt.inHandle) = mirrored;
        m_session->setWindowShape(m_drag.window, pts, tr("Move Window Tangent"));
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
    if (m_chartDrag >= 0) {
        const QPointF d = toFrameFraction(pos) - toFrameFraction(m_pressPos);
        colormatch::ChartQuad q = m_chartPressQuad;
        if (m_chartDrag < 4) {
            QPointF c = q.corner[size_t(m_chartDrag)] + d;
            q.corner[size_t(m_chartDrag)] = QPointF(std::clamp(c.x(), 0.0, 1.0), std::clamp(c.y(), 0.0, 1.0));
        } else {
            // Keep the whole outline on the picture.
            QPointF shift = d;
            for (const QPointF& c : q.corner) {
                shift.setX(std::clamp(shift.x(), -c.x(), 1.0 - c.x()));
                shift.setY(std::clamp(shift.y(), -c.y(), 1.0 - c.y()));
            }
            for (QPointF& c : q.corner)
                c += shift;
        }
        if (!(q == m_chartQuad)) {
            m_chartQuad = q;
            update();
            emit chartQuadChanged(q);
        }
        return;
    }
    if (m_chartOn) {
        const int hit = chartHit(pos);
        setCursor(hit == 4 ? Qt::SizeAllCursor : hit >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        return;
    }
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
    case Handle::TangentIn:
    case Handle::TangentOut:
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
    if (m_chartDrag >= 0) {
        m_chartDrag = -1;
        return;
    }
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
    // Double-click on a curve point switches it between smooth and corner; on a polygon or
    // curve edge it inserts a point there.
    if (!m_windowControls || !m_session)
        return;
    const GradeWindow* raw = m_session->window();
    if (!raw
        || (raw->shape != GradeWindowShape::Polygon && raw->shape != GradeWindowShape::Curve))
        return;
    const GradeWindow shownWindow = shown(*raw);
    const GradeWindow* w = &shownWindow;
    const bool curve = w->shape == GradeWindowShape::Curve;
    const QPointF pos = event->position();
    const int count = int(w->points.size());
    if (curve) {
        const HandleHit hit = hitTest(pos);
        if (hit.handle == Handle::Point && hit.window == w->id) {
            // Cusp: no handles. Smooth: automatic handles along the neighbours.
            GradeWindowShapePoints pts = w->points;
            GradeWindowPoint& p = pts[hit.pointIndex];
            if (p.smooth || p.hasHandles()) {
                p.inHandle = p.outHandle = QPointF();
                p.smooth = false;
            } else {
                p = smoothedPoint(pts, hit.pointIndex);
            }
            m_selPoint = hit.pointIndex;
            m_session->setWindowShape(w->id, pts, tr("Smooth or Corner Window Point"));
            return;
        }
    }
    if (count >= kMaxWindowPoints)
        return;
    const auto toW = windowToWidget(*w);
    QSizeF size;
    if (!toW || !pictureToWidget(&size))
        return;
    const double hw2 = w->value(wk::Width, m_session->gradeFrame()) * size.width();
    const double hh2 = w->value(wk::Height, m_session->gradeFrame()) * size.height();
    int best = -1;
    double bestT = 0;
    double bestDist = 8.0;
    constexpr int kSteps = 24;
    for (int i = 0; i < count; ++i) {
        // Walk the span in small steps (a straight edge is one step) and keep the nearest.
        const int steps = curve ? kSteps : 1;
        QPointF prev;
        for (int k = 0; k <= steps; ++k) {
            const double t = double(k) / steps;
            QPointF local = curve ? windowSpanPoint(w->points, i, t)
                                  : w->points[i].pos
                    + (w->points[(i + 1) % count].pos - w->points[i].pos) * t;
            const QPointF at = toW->map(QPointF(local.x() * hw2, local.y() * hh2));
            if (k > 0) {
                const QPointF ab = at - prev;
                const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
                const double u = len2 < 1e-9 ? 0.0
                                             : std::clamp(((pos.x() - prev.x()) * ab.x()
                                                              + (pos.y() - prev.y()) * ab.y())
                                                       / len2,
                                                   0.0, 1.0);
                const double d = length(prev + ab * u - pos);
                if (d < bestDist) {
                    bestDist = d;
                    best = i;
                    bestT = (double(k - 1) + u) / steps;
                }
            }
            prev = at;
        }
    }
    if (best < 0)
        return;
    m_session->addWindowPoint(w->id, best, bestT, tr("Add Window Point"));
}

} // namespace kk
