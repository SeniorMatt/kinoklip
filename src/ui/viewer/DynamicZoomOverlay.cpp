// SPDX-License-Identifier: GPL-3.0-or-later
// DynamicZoomOverlay — drawing of the on-screen Dynamic Zoom rectangles and their drags.
#include "ui/viewer/DynamicZoomOverlay.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "render/TimelineEval.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/SchemeColors.h"
#include "ui/viewer/TransformOverlay.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace kk {

namespace dz = dynamiczoom;

namespace {
constexpr double kHandleHalf = 3.5; ///< half side of a corner handle (px)
constexpr int kHaloAlpha = 160; ///< dark halo under the lines
// Resolve's colours: green start, red end.
const QColor kStartColor(0x3c, 0xc8, 0x4b);
const QColor kEndColor(0xe0, 0x3c, 0x3c);
} // namespace

DynamicZoomOverlay::DynamicZoomOverlay(AppContext& ctx, ViewerSurface* surface)
    : QWidget(surface->widget())
    , m_ctx(ctx)
    , m_surface(surface)
{
    setObjectName(QStringLiteral("DynamicZoomOverlay"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
    surface->widget()->installEventFilter(this);
    setGeometry(surface->widget()->rect());
    if (ctx.actions && ctx.actions->contains(QStringLiteral("view.onscreen_dynamic_zoom"))) {
        m_action = ctx.actions->action(QStringLiteral("view.onscreen_dynamic_zoom"));
        connect(m_action, &QAction::toggled, this, &DynamicZoomOverlay::refresh);
    }
    Timeline* timeline = ctx.project->timeline();
    connect(timeline, &Timeline::changed, this, &DynamicZoomOverlay::refresh);
    connect(timeline, &Timeline::playheadChanged, this, &DynamicZoomOverlay::refresh);
    if (ctx.selection)
        connect(ctx.selection, &SelectionModel::changed, this, &DynamicZoomOverlay::refresh);
    refresh();
}

DynamicZoomOverlay::~DynamicZoomOverlay()
{
    if (m_dragging)
        finishDrag(true);
}

bool DynamicZoomOverlay::isOn() const
{
    return m_action && m_action->isChecked();
}

const ClipData* DynamicZoomOverlay::currentClip(quint64* id) const
{
    return isOn() ? onScreenControlsClip(m_ctx, id) : nullptr;
}

std::optional<QTransform> DynamicZoomOverlay::pictureToWidget() const
{
    const ClipData* clip = currentClip();
    const TimelineData& data = m_ctx.project->timeline()->data();
    const QRect pic = m_surface->pictureRect();
    if (!clip || pic.isEmpty() || data.width <= 0 || data.height <= 0)
        return std::nullopt;
    const QTransform frameToWidget = QTransform::fromScale(double(pic.width()) / data.width,
                                         double(pic.height()) / data.height)
        * QTransform::fromTranslate(pic.x(), pic.y());
    const int64_t local = clip->localFrame(m_ctx.project->timeline()->playhead());
    // The clip's own transform without its Dynamic Zoom: the rectangles sit on the picture.
    return render::clipTransform(*clip, local, QSizeF(data.width, data.height), false)
        * frameToWidget;
}

void DynamicZoomOverlay::refresh()
{
    if (parentWidget() && geometry() != parentWidget()->rect())
        setGeometry(parentWidget()->rect());
    // The whole picture while the controls are chosen (Resolve shows the unzoomed source).
    if (m_ctx.playback)
        m_ctx.playback->setDynamicZoomApplied(!isOn());
    const bool editing = currentClip() != nullptr || m_dragging;
    if (editing != m_editing) {
        m_editing = editing;
        setAttribute(Qt::WA_TransparentForMouseEvents, !editing);
        setVisible(editing);
        if (editing)
            raise();
    }
    update();
}

void DynamicZoomOverlay::paintEvent(QPaintEvent*)
{
    const ClipData* clip = currentClip();
    const auto toWidget = pictureToWidget();
    if (!clip || !toWidget)
        return;
    const TimelineData& data = m_ctx.project->timeline()->data();
    const QSizeF frame(data.width, data.height);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPen halo(withAlpha(schemeColors().shadow, kHaloAlpha), 3);
    for (const bool end : {false, true}) {
        const dz::Rect r = end ? dz::end(clip->transform) : dz::start(clip->transform);
        const QPolygonF shape = toWidget->map(QPolygonF(dz::toPicture(r, frame)));
        const QColor colour = end ? kEndColor : kStartColor;
        p.setBrush(Qt::NoBrush);
        p.setPen(halo);
        p.drawPolygon(shape);
        p.setPen(QPen(colour, 1.5));
        p.drawPolygon(shape);
        p.setPen(halo);
        p.setBrush(colour);
        for (int i = 0; i < 4; ++i)
            p.drawRect(QRectF(shape[i] - QPointF(kHandleHalf, kHandleHalf),
                QSizeF(2 * kHandleHalf, 2 * kHandleHalf)));
    }
}

DynamicZoomOverlay::Handle DynamicZoomOverlay::hitTest(QPointF pos) const
{
    const ClipData* clip = currentClip();
    const auto toWidget = pictureToWidget();
    if (!clip || !toWidget)
        return {};
    const TimelineData& data = m_ctx.project->timeline()->data();
    const QSizeF frame(data.width, data.height);
    // The end rectangle is drawn on top, so it is tested first; corners before the insides.
    for (const bool end : {true, false}) {
        const dz::Rect r = end ? dz::end(clip->transform) : dz::start(clip->transform);
        const QRectF rect = dz::toPicture(r, frame);
        const std::pair<QPointF, std::pair<int, int>> corners[] = {
            {rect.topLeft(), {-1, -1}},
            {rect.topRight(), {1, -1}},
            {rect.bottomRight(), {1, 1}},
            {rect.bottomLeft(), {-1, 1}},
        };
        for (const auto& [corner, signs] : corners) {
            if (QLineF(toWidget->map(corner), pos).length() <= kGrabRadius)
                return {Handle::Part::Corner, end, signs.first, signs.second};
        }
    }
    bool invertible = false;
    const QPointF q = toWidget->inverted(&invertible).map(pos);
    if (!invertible)
        return {};
    for (const bool end : {true, false}) {
        const dz::Rect r = end ? dz::end(clip->transform) : dz::start(clip->transform);
        if (dz::toPicture(r, frame).contains(q))
            return {Handle::Part::Move, end, 0, 0};
    }
    return {};
}

void DynamicZoomOverlay::mousePressEvent(QMouseEvent* event)
{
    quint64 id = 0;
    const ClipData* clip = currentClip(&id);
    const auto toWidget = pictureToWidget();
    const Handle h = event->button() == Qt::LeftButton ? hitTest(event->position()) : Handle {};
    if (!clip || !toWidget || h.part == Handle::Part::None) {
        event->ignore();
        return;
    }
    emit pressed();
    setFocus();
    m_handle = h;
    m_pressPicture = toWidget->inverted().map(event->position());
    m_pressRect = h.end ? dz::end(clip->transform) : dz::start(clip->transform);
    m_dragging = true;
    m_dragClip = id;
    m_ctx.project->beginEdit(
        h.part == Handle::Part::Move ? tr("Move Dynamic Zoom") : tr("Resize Dynamic Zoom"));
}

void DynamicZoomOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging) {
        const Handle h = hitTest(event->position());
        if (h.part == Handle::Part::Corner)
            setCursor(h.sx == h.sy ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
        else if (h.part == Handle::Part::Move)
            setCursor(Qt::SizeAllCursor);
        else
            unsetCursor();
        return;
    }
    const auto toWidget = pictureToWidget();
    Timeline* timeline = m_ctx.project->timeline();
    ClipData* clip = editops::clipById(timeline->mutableData(), m_dragClip);
    if (!toWidget || !clip)
        return;
    const TimelineData& data = timeline->data();
    const QSizeF frame(data.width, data.height);
    const QPointF q = toWidget->inverted().map(event->position());
    dz::Rect r = m_pressRect;
    if (m_handle.part == Handle::Part::Move) {
        const QPointF d = q - m_pressPicture;
        r.cx += d.x() / frame.width();
        r.cy += d.y() / frame.height();
    } else {
        // The opposite corner stays; the size follows the pointer along the larger axis.
        const QRectF start = dz::toPicture(m_pressRect, frame);
        const QPointF fixed(m_handle.sx < 0 ? start.right() : start.left(),
            m_handle.sy < 0 ? start.bottom() : start.top());
        const double size = std::max({dz::kMinSize, std::abs(q.x() - fixed.x()) / frame.width(),
            std::abs(q.y() - fixed.y()) / frame.height()});
        const double w = size * frame.width();
        const double h = size * frame.height();
        const QPointF centre(fixed.x() + m_handle.sx * w / 2, fixed.y() + m_handle.sy * h / 2);
        r = dz::fromPicture(QRectF(centre.x() - w / 2, centre.y() - h / 2, w, h), frame);
    }
    if (m_handle.end)
        dz::setEnd(clip->transform, r);
    else
        dz::setStart(clip->transform, r);
    timeline->notifyChanged(); // live picture and Inspector while dragging
}

void DynamicZoomOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragging && event->button() == Qt::LeftButton)
        finishDrag(true);
}

void DynamicZoomOverlay::keyPressEvent(QKeyEvent* event)
{
    if (m_dragging && event->key() == Qt::Key_Escape) {
        finishDrag(false);
        return;
    }
    QWidget::keyPressEvent(event);
}

void DynamicZoomOverlay::finishDrag(bool commit)
{
    m_dragging = false;
    if (!commit)
        m_ctx.project->cancelEdit();
    m_ctx.project->endEdit();
    refresh();
}

bool DynamicZoomOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize)
        refresh();
    return QWidget::eventFilter(watched, event);
}

} // namespace kk
