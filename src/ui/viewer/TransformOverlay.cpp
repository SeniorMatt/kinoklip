// SPDX-License-Identifier: GPL-3.0-or-later
// TransformOverlay — drawing of the on-screen Transform / Crop controls and their drags.
#include "ui/viewer/TransformOverlay.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/SchemeColors.h"
#include "ui/viewer/GuideOverlay.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <cmath>

namespace kk {

namespace tg = transformgesture;

namespace {
constexpr double kCrossHalf = 7.0; ///< half size of the anchor cross (px)
constexpr double kAnchorRadius = 4.0; ///< ring around the anchor cross (px)
constexpr double kHandleHalf = 3.5; ///< half side of a square handle (px)
constexpr double kKnobRadius = 4.5; ///< rotation knob (px)
constexpr int kHaloAlpha = 160; ///< dark halo under the lines (readable over any picture)
constexpr int kDimAlpha = 110; ///< the uncropped outline in Crop mode

QString undoName(TransformHandle::Kind kind)
{
    switch (kind) {
    case TransformHandle::Kind::Scale:
        return TransformOverlay::tr("Scale Clip");
    case TransformHandle::Kind::Rotate:
        return TransformOverlay::tr("Rotate Clip");
    case TransformHandle::Kind::Anchor:
        return TransformOverlay::tr("Move Anchor Point");
    case TransformHandle::Kind::Crop:
        return TransformOverlay::tr("Crop Clip");
    case TransformHandle::Kind::Move:
    case TransformHandle::Kind::None:
        break;
    }
    return TransformOverlay::tr("Move Clip");
}

QAction* registryAction(AppContext& ctx, const char* id)
{
    return ctx.actions && ctx.actions->contains(QLatin1String(id))
        ? ctx.actions->action(QLatin1String(id))
        : nullptr;
}
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
    setMouseTracking(true); // hover cursors over the handles
    surface->widget()->installEventFilter(this);
    setGeometry(surface->widget()->rect());
    m_action = registryAction(ctx, "view.onscreen_transform");
    m_cropAction = registryAction(ctx, "view.onscreen_crop");
    for (QAction* a : {m_action, m_cropAction}) {
        if (a)
            connect(a, &QAction::toggled, this, &TransformOverlay::refresh);
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

bool TransformOverlay::isCropMode() const
{
    return m_cropAction && m_cropAction->isChecked();
}

const ClipData* TransformOverlay::currentClip(quint64* id) const
{
    const bool on = (m_action && m_action->isChecked()) || isCropMode();
    return on ? onScreenControlsClip(m_ctx, id) : nullptr;
}

const ClipData* onScreenControlsClip(const AppContext& ctx, quint64* id)
{
    if (!ctx.selection)
        return nullptr;
    quint64 clipId = ctx.selection->currentClip();
    const TimelineData& data = ctx.project->timeline()->data();
    auto loc = clipId ? editops::findClip(data, clipId) : std::nullopt;
    // The audio part of a linked pair was clicked: the controls act on its video part, as
    // the Inspector shows the pair as one item (spec 24.1).
    if (loc && loc->track.type != TrackType::Video) {
        const ClipData& audio = data.track(loc->track)->clips[size_t(loc->clipIndex)];
        loc.reset();
        for (int t = 0; audio.linkGroup && t < int(data.videoTracks.size()) && !loc; ++t) {
            const auto& clips = data.videoTracks[size_t(t)].clips;
            for (int i = 0; i < int(clips.size()); ++i) {
                if (clips[size_t(i)].linkGroup == audio.linkGroup) {
                    clipId = clips[size_t(i)].id;
                    loc = editops::findClip(data, clipId);
                    break;
                }
            }
        }
    }
    if (!loc || loc->track.type != TrackType::Video || data.track(loc->track)->locked)
        return nullptr;
    const ClipData& clip = data.track(loc->track)->clips[size_t(loc->clipIndex)];
    if (!clip.contains(ctx.project->timeline()->playhead()))
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

std::optional<TransformPicture> TransformOverlay::picture() const
{
    const ClipData* clip = currentClip();
    if (!clip)
        return std::nullopt;
    const TimelineData& data = m_ctx.project->timeline()->data();
    return tg::picture(*clip, clip->localFrame(m_ctx.project->timeline()->playhead()),
        QSizeF(data.width, data.height), m_ctx.project->mediaPool());
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
    const auto pic = picture();
    const auto toWidget = frameToWidget();
    if (!pic || !toWidget)
        return;
    const bool crop = isCropMode();
    const QTransform content = pic->contentToFrame * *toWidget;
    const QSizeF size = pic->content;
    const QPolygonF outline = content.map(
        QPolygonF(QRectF(-size.width() / 2, -size.height() / 2, size.width(), size.height())));
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const SchemeColors& c = schemeColors();
    const QPen halo(withAlpha(c.shadow, kHaloAlpha), 3);
    const QPen line(c.selection, 1.5);
    auto stroke = [&p, &halo, &line](auto draw) {
        p.setPen(halo);
        draw();
        p.setPen(line);
        draw();
    };
    p.setBrush(Qt::NoBrush);
    if (crop) {
        p.setPen(QPen(withAlpha(c.selection, kDimAlpha), 1, Qt::DashLine));
        p.drawPolygon(outline);
        const QPolygonF cropped = content.map(QPolygonF(pic->crop));
        stroke([&] { p.drawPolygon(cropped); });
    } else {
        stroke([&] { p.drawPolygon(outline); });
        // Rotation knob on a stem above the top edge.
        const QPointF top = content.map(QPointF(0, -size.height() / 2));
        const QPointF knob = tg::rotateKnob(*pic, *toWidget);
        stroke([&] { p.drawLine(top, knob); });
        p.setPen(halo);
        p.setBrush(c.selection);
        p.drawEllipse(knob, kKnobRadius, kKnobRadius);
        // Anchor point: cross and ring.
        const QPointF a = content.map(pic->anchor);
        p.setBrush(Qt::NoBrush);
        stroke([&] {
            p.drawLine(a - QPointF(kCrossHalf, 0), a + QPointF(kCrossHalf, 0));
            p.drawLine(a - QPointF(0, kCrossHalf), a + QPointF(0, kCrossHalf));
            p.drawEllipse(a, kAnchorRadius, kAnchorRadius);
        });
    }
    // Square handles on the corners and edge midpoints.
    p.setPen(halo);
    p.setBrush(c.selection);
    for (const auto& [pos, handle] : tg::handles(*pic, *toWidget, crop))
        p.drawRect(QRectF(
            pos - QPointF(kHandleHalf, kHandleHalf), QSizeF(2 * kHandleHalf, 2 * kHandleHalf)));
}

void TransformOverlay::updateCursor(QPointF pos)
{
    const auto pic = picture();
    const auto toWidget = frameToWidget();
    if (!pic || !toWidget) {
        unsetCursor();
        return;
    }
    const TransformHandle h = tg::hitTest(*pic, *toWidget, isCropMode(), pos);
    switch (h.kind) {
    case TransformHandle::Kind::None:
        unsetCursor();
        break;
    case TransformHandle::Kind::Move:
        setCursor(Qt::SizeAllCursor);
        break;
    case TransformHandle::Kind::Anchor:
        setCursor(Qt::CrossCursor);
        break;
    case TransformHandle::Kind::Rotate:
        setCursor(Qt::PointingHandCursor);
        break;
    case TransformHandle::Kind::Scale:
    case TransformHandle::Kind::Crop:
        if (h.sx != 0 && h.sy != 0)
            setCursor(h.sx == h.sy ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
        else
            setCursor(h.sx != 0 ? Qt::SizeHorCursor : Qt::SizeVerCursor);
        break;
    }
}

void TransformOverlay::mousePressEvent(QMouseEvent* event)
{
    quint64 id = 0;
    const ClipData* clip = currentClip(&id);
    const auto pic = picture();
    const auto toWidget = frameToWidget();
    if (!clip || !pic || !toWidget || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    const TransformHandle handle = tg::hitTest(*pic, *toWidget, isCropMode(), event->position());
    if (handle.kind == TransformHandle::Kind::None) {
        event->ignore();
        return;
    }
    emit pressed();
    setFocus();
    const int64_t local
        = std::clamp<int64_t>(clip->localFrame(m_ctx.project->timeline()->playhead()), 0,
            std::max<int64_t>(0, clip->duration - 1));
    m_drag.begin(*clip, local, *pic, handle, toWidget->inverted().map(event->position()));
    m_dragging = true;
    m_dragClip = id;
    m_ctx.project->beginEdit(undoName(handle.kind));
}

void TransformOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging) {
        updateCursor(event->position());
        return;
    }
    const auto toWidget = frameToWidget();
    Timeline* timeline = m_ctx.project->timeline();
    ClipData* clip = editops::clipById(timeline->mutableData(), m_dragClip);
    if (!toWidget || !clip)
        return;
    const int64_t local = std::clamp<int64_t>(
        clip->localFrame(timeline->playhead()), 0, std::max<int64_t>(0, clip->duration - 1));
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    QPointF pointer = toWidget->inverted().map(event->position());
    m_drag.update(clip->transform, local, pointer, shift);
    if (m_guides && !(event->modifiers() & Qt::ControlModifier))
        pointer = snapPointer(*clip, local, pointer, shift, *toWidget);
    timeline->notifyChanged(); // live picture and Inspector while dragging
}

void TransformOverlay::setGuideOverlay(GuideOverlay* guides)
{
    m_guides = guides;
}

QPointF TransformOverlay::snapPointer(
    ClipData& clip, int64_t localFrame, QPointF pointer, bool shift, const QTransform& toWidget)
{
    const std::vector<double> xs = m_guides->snapTargets(Qt::Vertical); // vertical guides: x
    const std::vector<double> ys = m_guides->snapTargets(Qt::Horizontal);
    const TransformHandle h = m_drag.handle();
    if ((xs.empty() && ys.empty()) || h.kind == TransformHandle::Kind::Rotate
        || h.kind == TransformHandle::Kind::None || toWidget.m11() <= 0)
        return pointer;
    const double threshold = kSnapPx / toWidget.m11(); // frame px
    const TimelineData& data = m_ctx.project->timeline()->data();
    const QSizeF frame(data.width, data.height);
    // The features that follow the pointer, in frame px, for the parameters written by the
    // last update(): Move = the picture's bounding box edges and centre, Anchor = the anchor
    // point, Scale = the dragged edges of the picture, Crop = the dragged crop edges.
    auto features = [&](std::vector<double>& fx, std::vector<double>& fy) {
        fx.clear();
        fy.clear();
        const TransformPicture pic
            = tg::picture(clip, localFrame, frame, m_ctx.project->mediaPool());
        if (h.kind == TransformHandle::Kind::Anchor) {
            const QPointF a = pic.contentToFrame.map(pic.anchor);
            fx.push_back(a.x());
            fy.push_back(a.y());
            return;
        }
        const QSizeF size = pic.content;
        const QRectF shape = h.kind == TransformHandle::Kind::Crop
            ? pic.crop
            : QRectF(-size.width() / 2, -size.height() / 2, size.width(), size.height());
        const QRectF box = pic.contentToFrame.map(QPolygonF(shape)).boundingRect();
        if (h.kind == TransformHandle::Kind::Move) {
            fx = {box.left(), box.center().x(), box.right()};
            fy = {box.top(), box.center().y(), box.bottom()};
            return;
        }
        // Scale / Crop: only the edges the handle drags.
        if (h.sx < 0)
            fx.push_back(box.left());
        else if (h.sx > 0)
            fx.push_back(box.right());
        if (h.sy < 0)
            fy.push_back(box.top());
        else if (h.sy > 0)
            fy.push_back(box.bottom());
    };
    // Move, Anchor and Crop follow the pointer 1:1, so one correction lands exactly; a scale
    // edge moves by a factor of the pointer (around the anchor), so a few rounds converge.
    constexpr int kRounds = 4;
    std::vector<double> fx, fy;
    for (int round = 0; round < kRounds; ++round) {
        features(fx, fy);
        const auto dx = guides::snapOffset(fx, xs, threshold);
        const auto dy = guides::snapOffset(fy, ys, threshold);
        if ((!dx || std::abs(*dx) < 1e-6) && (!dy || std::abs(*dy) < 1e-6))
            break;
        pointer += QPointF(dx.value_or(0.0), dy.value_or(0.0));
        m_drag.update(clip.transform, localFrame, pointer, shift);
    }
    return pointer;
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
