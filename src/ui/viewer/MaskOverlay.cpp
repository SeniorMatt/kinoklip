// SPDX-License-Identifier: GPL-3.0-or-later
// MaskOverlay implementation: mapping (viewer picture rect x clip transform x mask transform),
// painting of outlines and handles, hit testing and the undoable edit gestures.
#include "ui/viewer/MaskOverlay.h"

#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/Mask.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "render/TimelineEval.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QActionGroup>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk {

namespace {

constexpr double kHandleRadius = 4.0; ///< half size of a square handle, px
constexpr double kHitRadius = 7.0; ///< pick distance of handles and outlines, px
constexpr double kRotateOffset = 26.0; ///< rotate handle distance beyond the top edge, px
constexpr double kFeatherOffset = 16.0; ///< feather markers below the right edge, px
constexpr double kFeatherGap = 6.0; ///< feather markers outside / inside their edge, px
constexpr double kRotateSnapDegrees = 15.0; ///< Shift + rotate
constexpr double kStrokeTolerancePx = 2.0; ///< freehand simplification, screen pixels
constexpr double kMinDrawSpacing = 3.0; ///< drawing: clicks closer to the last point are ignored

const QColor kLine(255, 255, 255, 235);
const QColor kHalo(0, 0, 0, 150);
const QColor kInactive(255, 255, 255, 120);
const QColor kSelected(0xff, 0xa8, 0x3a); ///< timeline selection orange
const QColor kFeather(255, 255, 255, 140);

double len(QPointF d)
{
    return std::sqrt(d.x() * d.x() + d.y() * d.y());
}

double distToSegment(QPointF p, QPointF a, QPointF b)
{
    const QPointF ab = b - a;
    const double l2 = ab.x() * ab.x() + ab.y() * ab.y();
    const double t
        = l2 > 0 ? std::clamp(((p - a).x() * ab.x() + (p - a).y() * ab.y()) / l2, 0.0, 1.0) : 0.0;
    return len(p - (a + ab * t));
}

void setParam(MaskData& m, const QString& key, int64_t local, double value)
{
    m.params.animated(key, maskkeys::defaultValue(key)).setValue(local, value);
}

/// Minimum number of points of a closed point mask.
qsizetype minPoints(const MaskData& m)
{
    return m.shape == MaskShape::Bezier ? 2 : 3;
}

/// Moves the mask origin to the centre of its points' bounding box without moving the outline
/// (rotation and scale then act about the shape's centre).
void recenter(MaskData& m, int64_t local)
{
    const MaskPath path = m.path.at(double(local));
    if (path.isEmpty())
        return;
    // Min / max by hand: QRectF::united() ignores the empty rectangles of single points.
    double x0 = path[0].x, x1 = path[0].x, y0 = path[0].y, y1 = path[0].y;
    for (const MaskPoint& p : path) {
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
    }
    const QPointF c((x0 + x1) / 2, (y0 + y1) / 2);
    // pos' + R S (p - c) = pos + R S p for every p  <=>  pos' = maskToContent(c).
    const QPointF pos = maskToContent(m, local, c);
    m.path.forEachShape([c](MaskPath& shape) {
        for (MaskPoint& p : shape) {
            p.x -= c.x();
            p.y -= c.y();
        }
    });
    setParam(m, maskkeys::PosX, local, pos.x());
    setParam(m, maskkeys::PosY, local, pos.y());
}

/// Catmull-Rom tangent of point @p i of a closed path (smooth point from corner).
QPointF autoTangent(const MaskPath& path, int i)
{
    const qsizetype n = path.size();
    if (n < 2)
        return {};
    return (path[(i + 1) % n].pos() - path[(i + n - 1) % n].pos()) / 6.0;
}

/// Mask-local unit vector of the mask's +x axis in content space and the content centre.
std::pair<QPointF, QPointF> contentAxis(const MaskData& m, int64_t local)
{
    const QPointF c = maskToContent(m, local, QPointF(0, 0));
    QPointF axis = maskToContent(m, local, QPointF(1, 0)) - c;
    const double l = len(axis);
    axis = l > 1e-12 ? axis / l : QPointF(1, 0);
    return {c, axis};
}

} // namespace

MaskOverlay::MaskOverlay(AppContext& ctx, ViewerSurface* surface)
    : QWidget(surface->widget())
    , m_ctx(ctx)
    , m_surface(surface)
{
    setObjectName(QStringLiteral("MaskOverlay"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
    surface->widget()->installEventFilter(this);
    setGeometry(surface->widget()->rect());
    if (ctx.actions && ctx.actions->contains(QStringLiteral("view.onscreen_masks"))) {
        m_masksAction = ctx.actions->action(QStringLiteral("view.onscreen_masks"));
        connect(m_masksAction, &QAction::toggled, this, &MaskOverlay::refresh);
    }
    Timeline* timeline = ctx.project->timeline();
    connect(timeline, &Timeline::changed, this, &MaskOverlay::refresh);
    connect(timeline, &Timeline::playheadChanged, this, &MaskOverlay::refresh);
    if (ctx.selection) {
        connect(ctx.selection, &SelectionModel::changed, this, &MaskOverlay::refresh);
        connect(ctx.selection, &SelectionModel::activeMaskChanged, this, [this](quint64 id) {
            if (id != m_lastMask)
                m_selected.clear();
            m_lastMask = id;
            refresh();
        });
    }
    refresh();
}

MaskOverlay::~MaskOverlay()
{
    if (m_gesture)
        finishGesture();
}

QToolButton* MaskOverlay::createModeButton(QAction* masksAction, QWidget* parent)
{
    // Resolve-like on-screen controls menu at the bottom left of the viewer: one mode at a
    // time; "Mask" is the only overlay implemented so far.
    auto* button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("ViewerOnScreenControls"));
    button->setAutoRaise(true);
    button->setPopupMode(QToolButton::InstantPopup);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setToolTip(tr("On-screen controls"));
    auto* menu = new QMenu(button);
    auto* group = new QActionGroup(menu);
    QAction* off = menu->addAction(tr("Off"));
    off->setCheckable(true);
    group->addAction(off);
    QAction* mask = masksAction ? masksAction : menu->addAction(tr("Mask"));
    if (masksAction)
        menu->addAction(masksAction);
    mask->setCheckable(true);
    auto sync = [button, off, mask] {
        off->setChecked(!mask->isChecked());
        button->setText(mask->isChecked() ? tr("Mask") : tr("Off"));
    };
    connect(off, &QAction::triggered, mask, [mask] { mask->setChecked(false); });
    connect(mask, &QAction::toggled, button, sync);
    sync();
    button->setMenu(menu);
    return button;
}

// ---- Geometry ----

std::optional<MaskOverlay::View> MaskOverlay::view() const
{
    if (!m_ctx.selection)
        return std::nullopt;
    const quint64 clipId = m_ctx.selection->currentClip();
    const TimelineData& data = m_ctx.project->timeline()->data();
    const auto loc = clipId ? editops::findClip(data, clipId) : std::nullopt;
    if (!loc || loc->track.type != TrackType::Video)
        return std::nullopt;
    const ClipData& clip = data.track(loc->track)->clips[size_t(loc->clipIndex)];
    const int64_t playhead = m_ctx.project->timeline()->playhead();
    if (!clip.contains(playhead))
        return std::nullopt;
    const QRect pic = m_surface->pictureRect();
    if (pic.isEmpty() || data.width <= 0 || data.height <= 0)
        return std::nullopt;
    View v;
    v.clip = &clip;
    v.local = clip.localFrame(playhead);
    // Content → timeline frame (the compositor's own clip transform) → the viewer's picture
    // rectangle (Fit or zoom), so handles land exactly on the rendered picture.
    v.toWidget = render::clipTransform(clip, v.local, QSizeF(data.width, data.height))
        * QTransform::fromScale(
            double(pic.width()) / data.width, double(pic.height()) / data.height)
        * QTransform::fromTranslate(pic.x(), pic.y());
    bool invertible = false;
    v.toContent = v.toWidget.inverted(&invertible);
    if (!invertible)
        return std::nullopt;
    const quint64 active = m_ctx.selection->activeMask();
    for (int i = 0; i < clip.masks.size(); ++i) {
        if (clip.masks[i].id == active) {
            v.mask = &clip.masks[i];
            v.maskIndex = i;
        }
    }
    return v;
}

std::optional<QTransform> MaskOverlay::contentToWidget() const
{
    const auto v = view();
    return v ? std::optional<QTransform>(v->toWidget) : std::nullopt;
}

QTransform MaskOverlay::maskToWidget(const MaskData& m, int64_t local, const QTransform& toWidget)
{
    QTransform t;
    t.translate(m.value(maskkeys::PosX, local), m.value(maskkeys::PosY, local));
    t.rotate(m.value(maskkeys::Rotation, local));
    t.scale(m.value(maskkeys::ScaleX, local), m.value(maskkeys::ScaleY, local));
    return t * toWidget;
}

QRectF MaskOverlay::handleBox(const MaskData& m, int64_t local)
{
    if (!m.isPathShape()) {
        const double w = m.value(maskkeys::Width, local);
        const double h = m.value(maskkeys::Height, local);
        return QRectF(-w / 2, -h / 2, w, h);
    }
    const MaskPath path = m.path.at(double(local));
    double hx = 0;
    double hy = 0;
    for (const MaskPoint& p : path) {
        hx = std::max(hx, std::abs(p.x));
        hy = std::max(hy, std::abs(p.y));
    }
    return QRectF(-hx, -hy, 2 * hx, 2 * hy);
}

void MaskOverlay::refresh()
{
    const auto v = view();
    const bool controls = m_masksAction && m_masksAction->isChecked();
    const bool editing = v && (controls || v->mask);
    m_clipId = v ? v->clip->id : 0;
    if (editing != m_editing) {
        m_editing = editing;
        setAttribute(Qt::WA_TransparentForMouseEvents, !editing);
        if (!editing && m_gesture)
            finishGesture();
    }
    setVisible(editing);
    if (editing)
        raise();
    update();
}

// ---- Hit testing ----

MaskOverlay::Hit MaskOverlay::hitTest(const View& v, QPointF pos) const
{
    auto near = [&](QPointF p) { return len(p - pos) <= kHitRadius; };
    if (v.mask) {
        const MaskData& m = *v.mask;
        const QTransform toW = maskToWidget(m, v.local, v.toWidget);
        if (m.isPathShape() && !m.closed) {
            const MaskPath path = m.path.at(double(v.local));
            if (m.shape != MaskShape::Freehand && path.size() >= minPoints(m)
                && near(toW.map(path[0].pos())))
                return {Part::FirstPoint, 0};
            return {Part::Canvas};
        }
        if (m.isPathShape()) {
            const MaskPath path = m.path.at(double(v.local));
            if (m.shape != MaskShape::Polygon) {
                for (int i = 0; i < path.size(); ++i) {
                    const MaskPoint& p = path[i];
                    if (!p.hasTangents())
                        continue;
                    if (near(toW.map(QPointF(p.x + p.inX, p.y + p.inY))))
                        return {Part::InTangent, i};
                    if (near(toW.map(QPointF(p.x + p.outX, p.y + p.outY))))
                        return {Part::OutTangent, i};
                }
            }
            for (int i = 0; i < path.size(); ++i) {
                if (near(toW.map(path[i].pos())))
                    return {Part::Point, i};
            }
        }
        const QRectF box = handleBox(m, v.local);
        const QPointF centre = toW.map(QPointF(0, 0));
        const QPointF top = toW.map(QPointF(0, box.top()));
        QPointF up = top - centre;
        up = len(up) > 1e-6 ? up / len(up) : QPointF(0, -1);
        if (near(top + up * kRotateOffset))
            return {Part::Rotate};
        // Feather markers: along the mask's x axis, shifted perpendicular below the edge.
        const auto [cC, axisC] = contentAxis(m, v.local);
        const double half = std::abs(m.value(maskkeys::ScaleX, v.local)) * box.right();
        const QPointF outC = cC + axisC * (half + m.value(maskkeys::FeatherOut, v.local));
        const QPointF inC = cC + axisC * (half - m.value(maskkeys::FeatherIn, v.local));
        QPointF axisW = v.toWidget.map(cC + axisC) - v.toWidget.map(cC);
        axisW = len(axisW) > 1e-9 ? axisW / len(axisW) : QPointF(1, 0);
        const QPointF perp(-axisW.y(), axisW.x());
        if (near(v.toWidget.map(outC) + perp * kFeatherOffset + axisW * kFeatherGap))
            return {Part::FeatherOut};
        if (near(v.toWidget.map(inC) + perp * kFeatherOffset - axisW * kFeatherGap))
            return {Part::FeatherIn};
        const QPointF corners[4]
            = {box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft()};
        for (int i = 0; i < 4; ++i) {
            if (near(toW.map(corners[i])))
                return {Part::Corner, i};
        }
        const QPointF edges[4] = {QPointF(0, box.top()), QPointF(box.right(), 0),
            QPointF(0, box.bottom()), QPointF(box.left(), 0)};
        for (int i = 0; i < 4; ++i) {
            if (near(toW.map(edges[i])))
                return {Part::Edge, i};
        }
        if (m.isPathShape()) {
            // Outline segments: sample each cubic (the overlay is not a hot path).
            const MaskPath path = m.path.at(double(v.local));
            const qsizetype n = path.size();
            constexpr int kSamples = 24;
            for (qsizetype i = 0; i < n; ++i) {
                const MaskPoint& a = path[i];
                const MaskPoint& b = path[(i + 1) % n];
                QPointF prev = toW.map(a.pos());
                for (int k = 1; k <= kSamples; ++k) {
                    const double t = double(k) / kSamples;
                    const QPointF q
                        = toW.map(maskdetail::cubic(a.pos(), QPointF(a.x + a.outX, a.y + a.outY),
                            QPointF(b.x + b.inX, b.y + b.inY), b.pos(), t));
                    if (distToSegment(pos, prev, q) <= kHitRadius * 0.7)
                        return {Part::Segment, int(i), t - 0.5 / kSamples};
                    prev = q;
                }
            }
        }
        if (v.toWidget.map(maskOutline(m, v.local)).contains(pos))
            return {Part::Body};
    }
    for (int i = int(v.clip->masks.size()) - 1; i >= 0; --i) {
        if (i == v.maskIndex)
            continue;
        if (v.toWidget.map(maskOutline(v.clip->masks[i], v.local)).contains(pos))
            return {Part::OtherMask, i};
    }
    return {};
}

void MaskOverlay::updateCursor(const Hit& hit)
{
    switch (hit.part) {
    case Part::Body:
        setCursor(Qt::SizeAllCursor);
        break;
    case Part::Corner:
        setCursor(hit.index % 2 == 0 ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
        break;
    case Part::Edge:
        setCursor(hit.index % 2 == 0 ? Qt::SizeVerCursor : Qt::SizeHorCursor);
        break;
    case Part::FeatherIn:
    case Part::FeatherOut:
        setCursor(Qt::SizeHorCursor);
        break;
    case Part::Point:
    case Part::InTangent:
    case Part::OutTangent:
    case Part::FirstPoint:
    case Part::OtherMask:
        setCursor(Qt::PointingHandCursor);
        break;
    case Part::Rotate:
    case Part::Segment:
    case Part::Canvas:
        setCursor(Qt::CrossCursor);
        break;
    case Part::None:
        unsetCursor();
        break;
    }
}

// ---- Painting ----

void MaskOverlay::paintMask(QPainter& p, const View& v, const MaskData& m, bool active) const
{
    const QPainterPath outline = v.toWidget.map(maskOutline(m, v.local));
    if (!active) {
        QPen pen(kInactive, 1.0, Qt::DashLine);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(outline);
        return;
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(kHalo, 3.0));
    p.drawPath(outline);
    p.setPen(QPen(m.enabled ? kLine : kInactive, 1.4));
    p.drawPath(outline);
    // Feather extents: the outline offset by the feather distances (screen pixels).
    if (m.closed || !m.isPathShape()) {
        const double scale = std::sqrt(std::abs(v.toWidget.determinant()));
        const double out
            = m.value(maskkeys::FeatherOut, v.local) + m.value(maskkeys::Expand, v.local);
        const double in
            = -m.value(maskkeys::FeatherIn, v.local) + m.value(maskkeys::Expand, v.local);
        p.setPen(QPen(kFeather, 1.0, Qt::DotLine));
        for (const double d : {out, in}) {
            if (std::abs(d) < 0.5)
                continue;
            for (QPolygonF poly : outline.toSubpathPolygons()) {
                if (poly.size() > 2 && poly.front() == poly.back())
                    poly.removeLast();
                QVector<QPointF> pts(poly.cbegin(), poly.cend());
                offsetPolygon(pts, d * scale);
                p.drawPolygon(QPolygonF(pts));
            }
        }
    }
}

void MaskOverlay::paintHandles(QPainter& p, const View& v) const
{
    const MaskData& m = *v.mask;
    const QTransform toW = maskToWidget(m, v.local, v.toWidget);
    auto square = [&](QPointF c, bool filled) {
        const QRectF r(c - QPointF(kHandleRadius, kHandleRadius),
            QSizeF(2 * kHandleRadius, 2 * kHandleRadius));
        p.setPen(QPen(kHalo, 1.0));
        p.setBrush(filled ? QBrush(kSelected) : QBrush(kLine));
        p.drawRect(r);
    };
    auto dot = [&](QPointF c, double r, const QColor& fill) {
        p.setPen(QPen(kHalo, 1.0));
        p.setBrush(fill);
        p.drawEllipse(c, r, r);
    };

    if (m.isPathShape()) {
        const MaskPath path = m.path.at(double(v.local));
        if (!m.closed) {
            // Drawing: the open path so far and a rubber band to the mouse.
            QPainterPath open;
            if (!path.isEmpty()) {
                open.moveTo(toW.map(path[0].pos()));
                for (qsizetype i = 1; i < path.size(); ++i) {
                    const MaskPoint& a = path[i - 1];
                    const MaskPoint& b = path[i];
                    open.cubicTo(toW.map(QPointF(a.x + a.outX, a.y + a.outY)),
                        toW.map(QPointF(b.x + b.inX, b.y + b.inY)), toW.map(b.pos()));
                }
                if (m_hovering && !m_gesture)
                    open.lineTo(m_hoverPos);
            }
            for (const QPointF& s : m_stroke) {
                const QPointF w = toW.map(s);
                if (open.elementCount() == 0)
                    open.moveTo(w);
                else
                    open.lineTo(w);
            }
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(kHalo, 3.0));
            p.drawPath(open);
            p.setPen(QPen(kLine, 1.4));
            p.drawPath(open);
        }
        const bool tangents = m.shape != MaskShape::Polygon;
        for (int i = 0; i < path.size(); ++i) {
            const MaskPoint& pt = path[i];
            const QPointF c = toW.map(pt.pos());
            if (tangents && pt.hasTangents()) {
                const QPointF a = toW.map(QPointF(pt.x + pt.inX, pt.y + pt.inY));
                const QPointF b = toW.map(QPointF(pt.x + pt.outX, pt.y + pt.outY));
                p.setPen(QPen(kLine, 1.0));
                p.drawLine(a, b);
                dot(a, 3.0, kLine);
                dot(b, 3.0, kLine);
            }
            square(c, m_selected.contains(i) || (i == 0 && !m.closed));
        }
    }
    if (m.isPathShape() && !m.closed)
        return; // no transform handles while drawing

    const QRectF box = handleBox(m, v.local);
    if (m.isPathShape()) {
        QPolygonF frame;
        for (const QPointF& c :
            {box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft()})
            frame << toW.map(c);
        p.setPen(QPen(kInactive, 1.0, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(frame);
    }
    for (const QPointF& c :
        {box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft(), QPointF(0, box.top()),
            QPointF(box.right(), 0), QPointF(0, box.bottom()), QPointF(box.left(), 0)})
        square(toW.map(c), false);
    // Rotate handle on a stem above the top edge.
    const QPointF centre = toW.map(QPointF(0, 0));
    const QPointF top = toW.map(QPointF(0, box.top()));
    QPointF up = top - centre;
    up = len(up) > 1e-6 ? up / len(up) : QPointF(0, -1);
    p.setPen(QPen(kLine, 1.0));
    p.drawLine(top, top + up * kRotateOffset);
    dot(top + up * kRotateOffset, 4.0, kLine);
    // Centre cross.
    p.setPen(QPen(kLine, 1.0));
    p.drawLine(centre - QPointF(5, 0), centre + QPointF(5, 0));
    p.drawLine(centre - QPointF(0, 5), centre + QPointF(0, 5));
    // Feather markers: triangles pointing outwards (outside feather) and inwards (inside).
    const auto [cC, axisC] = contentAxis(m, v.local);
    const double half = std::abs(m.value(maskkeys::ScaleX, v.local)) * box.right();
    QPointF axisW = v.toWidget.map(cC + axisC) - v.toWidget.map(cC);
    axisW = len(axisW) > 1e-9 ? axisW / len(axisW) : QPointF(1, 0);
    const QPointF perp(-axisW.y(), axisW.x());
    auto triangle = [&](double distance, double direction) {
        const QPointF c = v.toWidget.map(cC + axisC * distance) + perp * kFeatherOffset
            + axisW * (kFeatherGap * direction);
        QPolygonF t;
        t << c + axisW * (5.0 * direction) << c - axisW * (4.0 * direction) + perp * 4.0
          << c - axisW * (4.0 * direction) - perp * 4.0;
        p.setPen(QPen(kHalo, 1.0));
        p.setBrush(kLine);
        p.drawPolygon(t);
    };
    triangle(half + m.value(maskkeys::FeatherOut, v.local), 1.0);
    triangle(half - m.value(maskkeys::FeatherIn, v.local), -1.0);
}

void MaskOverlay::paintEvent(QPaintEvent*)
{
    const auto v = view();
    if (!v)
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setClipRect(m_surface->pictureRect().adjusted(-40, -40, 40, 40));
    for (int i = 0; i < v->clip->masks.size(); ++i)
        paintMask(p, *v, v->clip->masks[i], i == v->maskIndex);
    if (v->mask)
        paintHandles(p, *v);
}

// ---- Events ----

bool MaskOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
        refresh();
    }
    return QWidget::eventFilter(watched, event);
}

bool MaskOverlay::event(QEvent* event)
{
    if (event->type() == QEvent::ShortcutOverride && m_editing) {
        // Claim the editing keys from the global shortcuts (Delete = ripple delete, ...)
        // while they mean something here.
        auto* key = static_cast<QKeyEvent*>(event);
        const auto v = view();
        const bool drawing = v && v->mask && v->mask->isPathShape() && !v->mask->closed;
        switch (key->key()) {
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            if (drawing || !m_selected.isEmpty())
                event->accept();
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (drawing)
                event->accept();
            break;
        case Qt::Key_Escape:
            if (v && v->mask)
                event->accept();
            break;
        default:
            break;
        }
        if (event->isAccepted())
            return true;
    }
    return QWidget::event(event);
}

void MaskOverlay::mousePressEvent(QMouseEvent* event)
{
    emit pressed();
    const auto v = view();
    if (!v || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPointF pos = event->position();
    const Hit hit = hitTest(*v, pos);
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    switch (hit.part) {
    case Part::OtherMask:
        setActiveMask(v->clip->masks[hit.index].id);
        return;
    case Part::None:
        m_selected.clear();
        update();
        return;
    case Part::FirstPoint:
        closeDrawnPath();
        return;
    case Part::Canvas:
        if (v->mask->shape == MaskShape::Freehand) {
            beginGesture(tr("Draw Mask"), *v, hit, pos);
            m_stroke.clear();
            if (const auto local = contentToMask(*v->mask, v->local, v->toContent.map(pos)))
                m_stroke.append(*local);
        } else {
            addDrawnPoint(*v, pos);
        }
        return;
    case Part::Segment:
        insertPointOnSegment(hit);
        return;
    case Part::Point:
        if (shift) {
            if (!m_selected.remove(hit.index))
                m_selected.insert(hit.index);
        } else if (!m_selected.contains(hit.index)) {
            m_selected = {hit.index};
        }
        beginGesture(tr("Move Mask Points"), *v, hit, pos);
        update();
        return;
    case Part::InTangent:
    case Part::OutTangent:
        beginGesture(tr("Change Mask Tangent"), *v, hit, pos);
        return;
    case Part::Body:
        beginGesture(tr("Move Mask"), *v, hit, pos);
        return;
    case Part::Corner:
    case Part::Edge:
        beginGesture(tr("Resize Mask"), *v, hit, pos);
        return;
    case Part::Rotate:
        beginGesture(tr("Rotate Mask"), *v, hit, pos);
        return;
    case Part::FeatherIn:
    case Part::FeatherOut:
        beginGesture(tr("Change Mask Feather"), *v, hit, pos);
        return;
    }
}

void MaskOverlay::mouseMoveEvent(QMouseEvent* event)
{
    m_hoverPos = event->position();
    m_hovering = true;
    if (m_gesture) {
        dragTo(event->position(), event->modifiers());
        return;
    }
    if (const auto v = view())
        updateCursor(hitTest(*v, event->position()));
    const auto v = view();
    if (v && v->mask && v->mask->isPathShape() && !v->mask->closed)
        update(); // rubber band
}

void MaskOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_gesture && event->button() == Qt::LeftButton)
        finishGesture();
}

void MaskOverlay::mouseDoubleClickEvent(QMouseEvent* event)
{
    const auto v = view();
    if (!v || !v->mask || event->button() != Qt::LeftButton)
        return;
    if (v->mask->isPathShape() && !v->mask->closed) {
        closeDrawnPath();
        return;
    }
    const Hit hit = hitTest(*v, event->position());
    if (hit.part == Part::Point)
        toggleSmooth(hit.index);
}

void MaskOverlay::keyPressEvent(QKeyEvent* event)
{
    const auto v = view();
    const MaskData* m = v ? v->mask : nullptr;
    const bool drawing = m && m->isPathShape() && !m->closed;
    switch (event->key()) {
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (drawing) {
            editMask(tr("Remove Mask Point"), [](MaskData& mask, int64_t) {
                const qsizetype n = mask.path.pointCount();
                if (n > 0)
                    removePathPoint(mask, n - 1);
            });
        } else {
            deleteSelectedPoints();
        }
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (drawing)
            closeDrawnPath();
        return;
    case Qt::Key_Escape:
        if (drawing)
            closeDrawnPath();
        else if (!m_selected.isEmpty())
            m_selected.clear();
        else
            setActiveMask(0);
        update();
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

// ---- Editing ----

void MaskOverlay::setActiveMask(quint64 maskId)
{
    if (m_ctx.selection)
        m_ctx.selection->setActiveMask(maskId);
}

void MaskOverlay::beginGesture(const QString& label, const View& v, const Hit& hit, QPointF pos)
{
    m_ctx.project->beginEdit(label);
    m_gesture = true;
    m_moved = false;
    m_dragHit = hit;
    m_pressPos = pos;
    m_startMask = *v.mask;
    m_gestureLocal = v.local;
    m_gestureToContent = v.toContent;
    m_gestureToWidget = v.toWidget;
}

void MaskOverlay::writeMask(const MaskData& mask)
{
    Timeline* timeline = m_ctx.project->timeline();
    ClipData* clip = editops::clipById(timeline->mutableData(), m_clipId);
    MaskData* target = clip ? keyframeops::findMask(*clip, mask.id) : nullptr;
    if (!target)
        return;
    *target = mask;
    timeline->notifyChanged();
}

void MaskOverlay::editMask(const QString& label, const std::function<void(MaskData&, int64_t)>& fn)
{
    const auto v = view();
    if (!v || !v->mask)
        return;
    MaskData m = *v->mask;
    m_ctx.project->beginEdit(label);
    fn(m, v->local);
    writeMask(m);
    m_ctx.project->endEdit();
}

void MaskOverlay::dragTo(QPointF pos, Qt::KeyboardModifiers mods)
{
    if ((pos - m_pressPos).manhattanLength() >= 1.0)
        m_moved = true;
    const MaskData& s = m_startMask;
    const int64_t local = m_gestureLocal;
    const QPointF pC = m_gestureToContent.map(pos);
    const QPointF p0C = m_gestureToContent.map(m_pressPos);
    const bool shift = mods & Qt::ShiftModifier;
    const bool alt = mods & Qt::AltModifier;
    MaskData m = s;

    switch (m_dragHit.part) {
    case Part::Body: {
        QPointF d = pC - p0C;
        if (shift) // constrain to the dominant axis
            (std::abs(d.x()) >= std::abs(d.y()) ? d.ry() : d.rx()) = 0.0;
        setParam(m, maskkeys::PosX, local, s.value(maskkeys::PosX, local) + d.x());
        setParam(m, maskkeys::PosY, local, s.value(maskkeys::PosY, local) + d.y());
        break;
    }
    case Part::Corner:
    case Part::Edge: {
        const auto lp = contentToMask(s, local, pC);
        if (!lp)
            return;
        const int i = m_dragHit.index;
        // Which sides move: corners both axes, edges one.
        int sgx = 0;
        int sgy = 0;
        if (m_dragHit.part == Part::Corner) {
            sgx = (i == 1 || i == 2) ? 1 : -1;
            sgy = i >= 2 ? 1 : -1;
        } else {
            sgx = i == 1 ? 1 : (i == 3 ? -1 : 0);
            sgy = i == 2 ? 1 : (i == 0 ? -1 : 0);
        }
        const QRectF box = handleBox(s, local);
        const double w0 = std::max(box.width(), 1e-6);
        const double h0 = std::max(box.height(), 1e-6);
        // Symmetric about the centre for Alt and always for point masks (they scale).
        const bool symmetric = alt || s.isPathShape();
        double w = w0;
        double h = h0;
        double cx = 0;
        double cy = 0;
        if (sgx != 0) {
            w = symmetric ? 2 * std::abs(lp->x()) : std::max(1.0, (lp->x() + sgx * w0 / 2) * sgx);
            if (!symmetric)
                cx = -sgx * w0 / 2 + sgx * w / 2;
        }
        if (sgy != 0) {
            h = symmetric ? 2 * std::abs(lp->y()) : std::max(1.0, (lp->y() + sgy * h0 / 2) * sgy);
            if (!symmetric)
                cy = -sgy * h0 / 2 + sgy * h / 2;
        }
        if (shift && sgx != 0 && sgy != 0) {
            const double ratio = std::max(w / w0, h / h0);
            w = w0 * ratio;
            h = h0 * ratio;
            if (!symmetric) {
                cx = -sgx * w0 / 2 + sgx * w / 2;
                cy = -sgy * h0 / 2 + sgy * h / 2;
            }
        }
        w = std::max(w, 1.0);
        h = std::max(h, 1.0);
        if (s.isPathShape()) {
            setParam(m, maskkeys::ScaleX, local, s.value(maskkeys::ScaleX, local) * w / w0);
            setParam(m, maskkeys::ScaleY, local, s.value(maskkeys::ScaleY, local) * h / h0);
        } else {
            const QPointF centre = maskToContent(s, local, QPointF(cx, cy));
            setParam(m, maskkeys::Width, local, w);
            setParam(m, maskkeys::Height, local, h);
            setParam(m, maskkeys::PosX, local, centre.x());
            setParam(m, maskkeys::PosY, local, centre.y());
        }
        break;
    }
    case Part::Rotate: {
        const QPointF c(s.value(maskkeys::PosX, local), s.value(maskkeys::PosY, local));
        const double a0 = std::atan2(p0C.y() - c.y(), p0C.x() - c.x());
        const double a1 = std::atan2(pC.y() - c.y(), pC.x() - c.x());
        double rot = s.value(maskkeys::Rotation, local) + (a1 - a0) * 180.0 / std::numbers::pi;
        if (shift)
            rot = std::round(rot / kRotateSnapDegrees) * kRotateSnapDegrees;
        setParam(m, maskkeys::Rotation, local, rot);
        break;
    }
    case Part::FeatherIn:
    case Part::FeatherOut: {
        // Relative to the press (the marker sits a few pixels off its edge): the feather
        // changes by the mouse movement along the mask's x axis.
        const auto [cC, axisC] = contentAxis(s, local);
        Q_UNUSED(cC);
        const QPointF d = pC - p0C;
        const double along = d.x() * axisC.x() + d.y() * axisC.y();
        if (m_dragHit.part == Part::FeatherOut)
            setParam(m, maskkeys::FeatherOut, local,
                std::max(0.0, s.value(maskkeys::FeatherOut, local) + along));
        else
            setParam(m, maskkeys::FeatherIn, local,
                std::max(0.0, s.value(maskkeys::FeatherIn, local) - along));
        break;
    }
    case Part::Point: {
        const auto a = contentToMask(s, local, p0C);
        const auto b = contentToMask(s, local, pC);
        if (!a || !b)
            return;
        const QPointF d = *b - *a;
        MaskPath path = s.path.at(double(local));
        for (const int i : std::as_const(m_selected)) {
            if (i >= 0 && i < path.size()) {
                path[i].x += d.x();
                path[i].y += d.y();
            }
        }
        m.path.setShape(local, path);
        break;
    }
    case Part::InTangent:
    case Part::OutTangent: {
        const auto tip = contentToMask(s, local, pC);
        MaskPath path = s.path.at(double(local));
        const int i = m_dragHit.index;
        if (!tip || i < 0 || i >= path.size())
            return;
        MaskPoint& pt = path[i];
        const QPointF t = *tip - pt.pos();
        const bool in = m_dragHit.part == Part::InTangent;
        const QPointF other = in ? QPointF(pt.outX, pt.outY) : QPointF(pt.inX, pt.inY);
        if (alt)
            pt.smooth = false; // break the tangent
        QPointF mirrored = other;
        if (pt.smooth) {
            const double lt = len(t);
            const double lo = len(other) > 1e-9 ? len(other) : lt;
            mirrored = lt > 1e-9 ? -t / lt * lo : other;
        }
        if (in) {
            pt.inX = t.x();
            pt.inY = t.y();
            pt.outX = mirrored.x();
            pt.outY = mirrored.y();
        } else {
            pt.outX = t.x();
            pt.outY = t.y();
            pt.inX = mirrored.x();
            pt.inY = mirrored.y();
        }
        m.path.setShape(local, path);
        break;
    }
    case Part::Canvas: {
        const auto lp = contentToMask(s, local, pC);
        if (!lp)
            return;
        if (s.shape == MaskShape::Freehand) {
            if (m_stroke.isEmpty()
                || len(m_gestureToWidget.map(*lp) - m_gestureToWidget.map(m_stroke.back())) >= 1.5)
                m_stroke.append(*lp);
            update();
            return;
        }
        // Bezier: dragging from a new point pulls out its tangents (a smooth point).
        MaskPath path = s.path.at(double(local));
        if (path.isEmpty() || s.shape != MaskShape::Bezier)
            return;
        MaskPoint& last = path.last();
        const QPointF t = *lp - last.pos();
        last.outX = t.x();
        last.outY = t.y();
        last.inX = -t.x();
        last.inY = -t.y();
        last.smooth = true;
        m.path.setShape(local, path);
        break;
    }
    default:
        return;
    }
    writeMask(m);
}

void MaskOverlay::finishGesture()
{
    if (!m_gesture)
        return;
    m_gesture = false;
    if (m_dragHit.part == Part::Canvas && m_startMask.shape == MaskShape::Freehand) {
        MaskData m = m_startMask;
        if (m_stroke.size() >= 3) {
            // Simplify in mask-local units: screen tolerance / screen pixels per unit.
            const double unit
                = len(m_gestureToWidget.map(QPointF(1, 0)) - m_gestureToWidget.map(QPointF(0, 0)))
                * std::abs(m.value(maskkeys::ScaleX, m_gestureLocal));
            m.path.setShape(m_gestureLocal,
                simplifyStroke(m_stroke, kStrokeTolerancePx / std::max(unit, 1e-6)));
            m.closed = true;
            recenter(m, m_gestureLocal);
            writeMask(m);
        }
        m_stroke.clear();
    }
    m_ctx.project->endEdit();
    update();
}

void MaskOverlay::cancelGesture()
{
    if (!m_gesture)
        return;
    m_gesture = false;
    m_stroke.clear();
    m_ctx.project->cancelEdit();
}

void MaskOverlay::addDrawnPoint(const View& v, QPointF pos)
{
    const auto lp = contentToMask(*v.mask, v.local, v.toContent.map(pos));
    if (!lp)
        return;
    const MaskPath path = v.mask->path.at(double(v.local));
    if (!path.isEmpty()) {
        const QTransform toW = maskToWidget(*v.mask, v.local, v.toWidget);
        if (len(toW.map(path.last().pos()) - pos) < kMinDrawSpacing)
            return; // the second click of a double-click
    }
    beginGesture(tr("Add Mask Point"), v, {Part::Canvas}, pos);
    MaskData m = *v.mask;
    MaskPath next = path;
    next.append(MaskPoint {lp->x(), lp->y()});
    m.path.setShape(v.local, next);
    writeMask(m);
    m_startMask = m; // the drag pulls tangents out of the new point
}

void MaskOverlay::closeDrawnPath()
{
    const auto v = view();
    if (!v || !v->mask || !v->mask->isPathShape() || v->mask->closed)
        return;
    if (v->mask->path.pointCount() >= minPoints(*v->mask)) {
        editMask(tr("Close Mask Shape"), [](MaskData& m, int64_t local) {
            m.closed = true;
            recenter(m, local);
        });
        return;
    }
    // Too few points to enclose anything: discard the mask.
    const quint64 maskId = v->mask->id;
    m_ctx.project->beginEdit(tr("Delete Mask"));
    if (ClipData* c = editops::clipById(m_ctx.project->timeline()->mutableData(), m_clipId))
        c->masks.removeIf([maskId](const MaskData& m) { return m.id == maskId; });
    m_ctx.project->timeline()->notifyChanged();
    m_ctx.project->endEdit();
    setActiveMask(0);
}

void MaskOverlay::deleteSelectedPoints()
{
    const auto v = view();
    if (!v || !v->mask || m_selected.isEmpty())
        return;
    const qsizetype keep = v->mask->path.pointCount() - m_selected.size();
    if (keep < minPoints(*v->mask))
        return; // the shape would collapse; delete the mask in the Inspector instead
    QList<int> indices = m_selected.values();
    std::sort(indices.begin(), indices.end(), std::greater<int>());
    editMask(tr("Delete Mask Points"), [&indices](MaskData& m, int64_t) {
        for (const int i : indices)
            removePathPoint(m, i);
    });
    m_selected.clear();
}

void MaskOverlay::toggleSmooth(int index)
{
    const auto v = view();
    if (!v || !v->mask || v->mask->shape == MaskShape::Polygon)
        return;
    editMask(tr("Change Mask Point"), [index](MaskData& m, int64_t local) {
        MaskPath path = m.path.at(double(local));
        if (index < 0 || index >= path.size())
            return;
        MaskPoint& p = path[index];
        if (p.hasTangents()) {
            p.inX = p.inY = p.outX = p.outY = 0.0; // corner
            p.smooth = false;
        } else {
            const QPointF t = autoTangent(path, index);
            p.outX = t.x();
            p.outY = t.y();
            p.inX = -t.x();
            p.inY = -t.y();
            p.smooth = true;
        }
        m.path.setShape(local, path);
    });
}

void MaskOverlay::insertPointOnSegment(const Hit& hit)
{
    qsizetype added = -1;
    editMask(tr("Add Mask Point"), [&](MaskData& m, int64_t) {
        added = insertPathPoint(m, hit.index, std::clamp(hit.t, 0.0, 1.0));
    });
    if (added >= 0)
        m_selected = {int(added)};
    update();
}

} // namespace kk
