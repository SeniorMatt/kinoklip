// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineWidget, level lines and fade handles (spec section 31): presses on a clip's fade
// handles, on the audio level line (drag = volume, Alt+click = add a key) and on its keys
// (drag in time and value, right-click menu). Every gesture is one undo step; the edits
// themselves are core/LevelOps.h.
#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/LevelOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/LevelLines.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineEditUtil.h"
#include "ui/timeline/TimelineWidget.h"

#include <QGuiApplication>
#include <QMenu>
#include <QToolTip>

#include <cmath>

namespace kk {

namespace ll = levellines;

namespace {

/// Clips that get fade handles: everything with a picture or sound, not captions.
bool hasFadeHandles(const ClipData& clip)
{
    return clip.kind != ClipKind::Subtitle;
}

/// Tooltip text of a fade curve value while it is dragged.
QString fadeCurveText(double curve)
{
    if (curve == 0.0)
        return QObject::tr("Fade curve: linear");
    return QObject::tr("Fade curve: %1 (%2)")
        .arg(curve, 0, 'f', 2)
        .arg(curve > 0 ? QObject::tr("slow start") : QObject::tr("fast start"));
}

/// Audio clips with a level line (media or nested sound).
bool hasLevelLine(const ClipData& clip, TrackType type)
{
    return type == TrackType::Audio
        && (clip.kind == ClipKind::Media || clip.kind == ClipKind::Nested);
}

} // namespace

std::optional<Qt::CursorShape> TimelineWidget::levelCursor(
    QPointF pos, const TimelineHit& hit) const
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const ClipData* clip = editops::clipById(data, hit.clipId);
    if (!clip || hit.row < 0)
        return std::nullopt;
    const QRectF r = m_layout.clipRect(hit.row, *clip);
    if (hasFadeHandles(*clip) && ll::hitFadeCurveHandle(*clip, r, m_layout, pos) != 0)
        return Qt::SplitVCursor; // the fade curve dot (spec 59.1)
    if (hasFadeHandles(*clip) && ll::hitFadeHandle(*clip, r, m_layout, pos) != 0)
        return Qt::SizeHorCursor;
    if (hasLevelLine(*clip, hit.track.type)) {
        const ll::LevelHit h = ll::hitLevel(*clip, r, m_layout, pos);
        if (h.kind == ll::LevelHit::Kind::Key)
            return Qt::SizeAllCursor;
        if (h.kind == ll::LevelHit::Kind::Line)
            return Qt::SizeVerCursor;
    }
    return std::nullopt;
}

bool TimelineWidget::pressLevels(QPointF pos, const TimelineHit& hit)
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const ClipData* clip = editops::clipById(data, hit.clipId);
    const TrackData* track = data.track(hit.track);
    if (!clip || !track || track->locked || hit.row < 0)
        return false;
    const QRectF r = m_layout.clipRect(hit.row, *clip);

    // The curve dot in the middle of a fade line (before the level line it may lie on).
    if (hasFadeHandles(*clip)) {
        if (const int dot = ll::hitFadeCurveHandle(*clip, r, m_layout, pos)) {
            m_drag.levelClip = clip->id;
            m_drag.fadeHead = dot == 1;
            m_drag.curveGrabDy = pos.y()
                - ll::fadeCurveHandleRect(*clip, r, m_layout, m_drag.fadeHead).center().y();
            m_drag.pending = DragMode::FadeCurve;
            m_drag.mode = DragMode::Pending;
            return true;
        }
    }
    // Fade handles at the top corners.
    if (hasFadeHandles(*clip)) {
        if (const int handle = ll::hitFadeHandle(*clip, r, m_layout, pos)) {
            m_drag.levelClip = clip->id;
            m_drag.fadeHead = handle == 1;
            m_drag.pending = DragMode::Fade;
            m_drag.mode = DragMode::Pending;
            return true;
        }
    }
    if (!hasLevelLine(*clip, hit.track.type))
        return false;
    const ll::LevelHit h = ll::hitLevel(*clip, r, m_layout, pos);
    if (h.kind == ll::LevelHit::Kind::None)
        return false;
    m_drag.levelClip = clip->id;
    m_drag.levelArea = ll::levelArea(r);
    m_drag.levelLastY = pos.y();
    if (h.kind == ll::LevelHit::Kind::Key) {
        // Select the key (only it), ready to drag it in time and value.
        const KeyframeRef ref {clip->id, levelops::volumePath(), h.localFrame};
        m_ctx.selection->setClips({});
        m_ctx.selection->setKeyframes({ref});
        m_drag.levelKey = ref;
        m_drag.levelKeyDb = levelops::volumeDbAt(*clip, double(h.localFrame));
        m_drag.pending = DragMode::LevelKey;
        m_drag.mode = DragMode::Pending;
        return true;
    }
    if (m_drag.modifiers & Qt::AltModifier) {
        // Alt+click on the line: a key at that frame with the line's value (one undo step).
        std::optional<KeyframeRef> added;
        timelineedit::runEdit(m_ctx.project, tr("Add Keyframe"), [&](TimelineData& d) {
            added = levelops::addVolumeKey(d, clip->id, h.localFrame);
            return added.has_value();
        });
        if (added) {
            m_ctx.selection->setClips({});
            m_ctx.selection->setKeyframes({*added});
        }
        m_drag = Drag {};
        viewport()->update();
        return true;
    }
    m_drag.levelFrame = h.localFrame;
    m_drag.pending = DragMode::LevelLine;
    m_drag.mode = DragMode::Pending;
    return true;
}

void TimelineWidget::updateLevelDrag(QPointF pos)
{
    restoreOrigin();
    TimelineData& data = m_ctx.project->timeline()->mutableData();
    const ClipData* clip = editops::clipById(data, m_drag.levelClip);
    if (!clip) {
        m_ctx.project->timeline()->notifyChanged();
        return;
    }
    if (m_drag.mode == DragMode::Fade) {
        // The fade reaches from the clip's corner to the pointer (whole frames).
        const double ppf = m_layout.pixelsPerFrame();
        const double edgeX = m_layout.frameToX(double(m_drag.fadeHead ? clip->start : clip->end()));
        const double px = m_drag.fadeHead ? pos.x() - edgeX : edgeX - pos.x();
        const int64_t frames = ppf > 0 ? std::max<int64_t>(0, std::llround(px / ppf)) : 0;
        levelops::setFade(data, clip->id, m_drag.fadeHead, frames);
        m_ctx.project->timeline()->notifyChanged();
        return;
    }
    if (m_drag.mode == DragMode::FadeCurve) {
        // The middle of the fade follows the pointer's height (kept relative to where the dot
        // was grabbed, so it does not jump); near linear it snaps to exactly linear.
        if (m_drag.hit.row >= 0) {
            const QRectF r = m_layout.clipRect(m_drag.hit.row, *clip);
            const double curve = ll::fadeCurveForY(r, pos.y() - m_drag.curveGrabDy);
            levelops::setFadeCurve(data, clip->id, m_drag.fadeHead, curve);
            QToolTip::showText(viewport()->mapToGlobal(pos.toPoint()) + QPoint(14, 14),
                fadeCurveText(curve), viewport());
        }
        m_ctx.project->timeline()->notifyChanged();
        return;
    }
    // Vertical motion in dB, accumulated move by move so Shift (fine, a tenth) can be pressed
    // or released during the drag without the line jumping.
    const bool fine = QGuiApplication::keyboardModifiers() & Qt::ShiftModifier;
    m_drag.levelDb += -(pos.y() - m_drag.levelLastY) * ll::dbPerPixel(m_drag.levelArea)
        * (fine ? ll::kFineFactor : 1.0);
    m_drag.levelLastY = pos.y();
    if (m_drag.mode == DragMode::LevelLine) {
        levelops::offsetVolume(data, clip->id, m_drag.levelFrame, m_drag.levelDb);
    } else {
        const double ppf = m_layout.pixelsPerFrame();
        const int64_t dt = ppf > 0 ? std::llround((pos.x() - m_drag.pressPos.x()) / ppf) : 0;
        if (const auto moved = levelops::setVolumeKey(data, m_drag.levelKey,
                m_drag.levelKey.frame + dt, m_drag.levelKeyDb + m_drag.levelDb))
            m_ctx.selection->setKeyframes({*moved});
    }
    m_ctx.project->timeline()->notifyChanged();
}

bool TimelineWidget::resetFadeCurveAt(QPointF pos)
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const TimelineHit hit = m_layout.hitTest(data, pos);
    if (hit.area != TimelineHit::Area::Clip || hit.row < 0
        || m_controller->tool() != TimelineTool::Select)
        return false;
    const ClipData* clip = editops::clipById(data, hit.clipId);
    const TrackData* track = data.track(hit.track);
    if (!clip || !track || track->locked || !hasFadeHandles(*clip))
        return false;
    const int dot = ll::hitFadeCurveHandle(*clip, m_layout.clipRect(hit.row, *clip), m_layout, pos);
    if (dot == 0)
        return false;
    const quint64 id = clip->id;
    timelineedit::runEdit(m_ctx.project, tr("Reset Fade Curve"),
        [&](TimelineData& d) { return levelops::setFadeCurve(d, id, dot == 1, 0.0); });
    viewport()->update();
    return true;
}

bool TimelineWidget::levelKeyMenu(QPointF pos, QPoint globalPos)
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const TimelineHit hit = m_layout.hitTest(data, pos);
    if (hit.area != TimelineHit::Area::Clip || hit.row < 0)
        return false;
    const ClipData* clip = editops::clipById(data, hit.clipId);
    if (!clip || !hasLevelLine(*clip, hit.track.type))
        return false;
    const ll::LevelHit h = ll::hitLevel(*clip, m_layout.clipRect(hit.row, *clip), m_layout, pos);
    if (h.kind != ll::LevelHit::Kind::Key)
        return false;
    const KeyframeRef ref {clip->id, levelops::volumePath(), h.localFrame};
    m_ctx.selection->setClips({});
    m_ctx.selection->setKeyframes({ref});
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("levelKeyMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(tr("Delete Keyframe"), this, [this, ref] {
        timelineedit::runEdit(m_ctx.project, tr("Delete Keyframe"),
            [&](TimelineData& d) { return keyframeops::deleteKeys(d, {ref}) > 0; });
        m_ctx.selection->setKeyframes({});
    });
    menu->popup(globalPos);
    return true;
}

} // namespace kk
