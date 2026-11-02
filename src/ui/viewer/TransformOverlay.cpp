// SPDX-License-Identifier: GPL-3.0-or-later
// TransformOverlay — outline of the current clip and the move gesture.
#include "ui/viewer/TransformOverlay.h"

#include "core/EditOps.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "render/TimelineEval.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/SchemeColors.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <cmath>

namespace kk {

namespace {
/// Half size of the centre cross (px).
constexpr double kCrossHalf = 7.0;
} // namespace

TransformOverlay::TransformOverlay(AppContext& ctx, ViewerSurface* surface)
    : QWidget(surface->widget())
    , m_ctx(ctx)
    , m_surface(surface)
{
    setObjectName(QStringLiteral("TransformOverlay"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::ClickFocus);
    surface->widget()->installEventFilter(this);
    setGeometry(surface->widget()->rect());
    if (ctx.actions && ctx.actions->contains(QStringLiteral("view.onscreen_transform"))) {
        m_action = ctx.actions->action(QStringLiteral("view.onscreen_transform"));
        connect(m_action, &QAction::toggled, this, &TransformOverlay::refresh);
    }
    Timeline* timeline = ctx.project->timeline();
    connect(timeline, &Timeline::changed, this, &TransformOverlay::refresh);
    connect(timeline, &Timeline::playheadChanged, this, &TransformOverlay::refresh);
    if (ctx.selection)
        connect(ctx.selection, &SelectionModel::changed, this, &TransformOverlay::refresh);
    refresh();
}

TransformOverlay::~TransformOverlay()
{
    if (m_dragging)
        finishDrag(true);
}

const ClipData* TransformOverlay::currentClip(quint64* id) const
{
    if (!m_ctx.selection || !m_action || !m_action->isChecked())
        return nullptr;
    const quint64 clipId = m_ctx.selection->currentClip();
    const TimelineData& data = m_ctx.project->timeline()->data();
    const auto loc = clipId ? editops::findClip(data, clipId) : std::nullopt;
    if (!loc || loc->track.type != TrackType::Video || data.track(loc->track)->locked)
        return nullptr;
    const ClipData& clip = data.track(loc->track)->clips[size_t(loc->clipIndex)];
    if (!clip.contains(m_ctx.project->timeline()->playhead()))
        return nullptr;
    if (id)
        *id = clipId;
    return &clip;
}

std::optional<QTransform> TransformOverlay::frameToWidget() const
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const QRect pic = m_surface->pictureRect();
    if (!currentClip() || pic.isEmpty() || data.width <= 0 || data.height <= 0)
        return std::nullopt;
    return QTransform::fromScale(
               double(pic.width()) / data.width, double(pic.height()) / data.height)
        * QTransform::fromTranslate(pic.x(), pic.y());
}

void TransformOverlay::refresh()
{
    if (parentWidget() && geometry() != parentWidget()->rect())
        setGeometry(parentWidget()->rect());
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

void TransformOverlay::paintEvent(QPaintEvent*)
{
    const ClipData* clip = currentClip();
    const auto toWidget = frameToWidget();
    if (!clip || !toWidget)
        return;
    const TimelineData& data = m_ctx.project->timeline()->data();
    const int64_t local = clip->localFrame(m_ctx.project->timeline()->playhead());
    // The clip's frame-sized content through its own transform (content origin = centre).
    const QTransform content
        = render::clipTransform(*clip, local, QSizeF(data.width, data.height)) * *toWidget;
    const QPolygonF outline = content.map(
        QPolygonF(QRectF(-data.width / 2.0, -data.height / 2.0, data.width, data.height)));
    const QPointF centre = content.map(QPointF(0, 0));
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const SchemeColors& c = schemeColors();
    p.setPen(QPen(withAlpha(c.shadow, 160), 3));
    p.drawPolygon(outline);
    p.setPen(QPen(c.selection, 1.5));
    p.drawPolygon(outline);
    p.drawLine(centre - QPointF(kCrossHalf, 0), centre + QPointF(kCrossHalf, 0));
    p.drawLine(centre - QPointF(0, kCrossHalf), centre + QPointF(0, kCrossHalf));
}

void TransformOverlay::mousePressEvent(QMouseEvent* event)
{
    quint64 id = 0;
    const ClipData* clip = currentClip(&id);
    if (!clip || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    emit pressed();
    setFocus();
    const int64_t local = clip->localFrame(m_ctx.project->timeline()->playhead());
    m_dragging = true;
    m_dragClip = id;
    m_pressPos = event->position();
    m_startPos = QPointF(
        clip->transform.value(keys::PosX, local), clip->transform.value(keys::PosY, local));
    m_ctx.project->beginEdit(tr("Move Clip"));
}

void TransformOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging)
        return;
    const QRect pic = m_surface->pictureRect();
    const TimelineData& data = m_ctx.project->timeline()->data();
    if (pic.isEmpty())
        return;
    // Widget px -> timeline px (the viewer shows the frame scaled into pictureRect()).
    QPointF d = event->position() - m_pressPos;
    d.rx() *= double(data.width) / pic.width();
    d.ry() *= double(data.height) / pic.height();
    if (event->modifiers() & Qt::ShiftModifier) {
        if (std::abs(d.x()) >= std::abs(d.y()))
            d.setY(0);
        else
            d.setX(0);
    }
    Timeline* timeline = m_ctx.project->timeline();
    ClipData* clip = editops::clipById(timeline->mutableData(), m_dragClip);
    if (!clip)
        return;
    const int64_t local = std::clamp<int64_t>(
        clip->localFrame(timeline->playhead()), 0, std::max<int64_t>(0, clip->duration - 1));
    // Whole pixels, like the Inspector's Position fields.
    clip->transform.animated(keys::PosX, 0).setValue(local, std::round(m_startPos.x() + d.x()));
    clip->transform.animated(keys::PosY, 0).setValue(local, std::round(m_startPos.y() + d.y()));
    timeline->notifyChanged(); // live picture while dragging
}

void TransformOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragging && event->button() == Qt::LeftButton)
        finishDrag(true);
}

void TransformOverlay::keyPressEvent(QKeyEvent* event)
{
    if (m_dragging && event->key() == Qt::Key_Escape) {
        finishDrag(false);
        return;
    }
    QWidget::keyPressEvent(event);
}

void TransformOverlay::finishDrag(bool commit)
{
    m_dragging = false;
    if (!commit)
        m_ctx.project->cancelEdit();
    m_ctx.project->endEdit();
    refresh();
}

bool TransformOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize)
        refresh();
    return QWidget::eventFilter(watched, event);
}

} // namespace kk
