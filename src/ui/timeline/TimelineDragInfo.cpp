// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineWidget, part: the offset label next to the pointer while moving / trimming
// (spec 21.9) and the trim preview frames for the viewer's two-up view (spec 8.15).
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/timeline/TimelineWidget.h"

#include <algorithm>
#include <limits>

namespace kk {

namespace ops = editops;

namespace {

/// Short track name for the label ("V2", "A1").
QString trackLabel(const TrackRef& ref)
{
    const QChar letter = ref.type == TrackType::Video ? QLatin1Char('V')
        : ref.type == TrackType::Audio                ? QLatin1Char('A')
                                                      : QLatin1Char('S');
    return letter + QString::number(ref.index + 1);
}

/// The side of the two-up view showing @p clip's frame at timeline frame @p frame.
TrimPreviewSide sideOf(const ClipData* clip, int64_t frame, Rational fps)
{
    TrimPreviewSide side;
    if (!clip)
        return side;
    frame = std::clamp(frame, clip->start, clip->end() - 1);
    side.clipId = clip->id;
    side.mediaId = clip->kind == ClipKind::Media ? clip->mediaId : QString();
    side.sourceUs = std::max<int64_t>(0, clip->sourceTimeUsAt(frame, fps));
    side.timelineFrame = frame;
    side.name = clip->name;
    return side;
}

/// The clip before (@p after = false) or after @p clipId on its track, if any.
const ClipData* neighbour(const TimelineData& data, quint64 clipId, bool after)
{
    const auto loc = ops::findClip(data, clipId);
    if (!loc)
        return nullptr;
    const auto& clips = data.track(loc->track)->clips;
    const int i = loc->clipIndex + (after ? 1 : -1);
    return i >= 0 && i < int(clips.size()) ? &clips[size_t(i)] : nullptr;
}

} // namespace

void TimelineWidget::refreshDragInfo()
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const Rational fps = data.fps;
    DragInfoLabel label;
    TrimPreview preview;
    const ClipData* now = ops::clipById(data, m_drag.primary);
    const ClipData* was = ops::clipById(m_drag.origin, m_drag.primary);

    switch (m_drag.mode) {
    case DragMode::Move: {
        int64_t offset = 0;
        QString track;
        if (m_drag.duplicate) {
            // The copies have new ids: measure the earliest one against the originals.
            int64_t minStart = std::numeric_limits<int64_t>::max();
            for (quint64 id : m_drag.resultIds)
                if (const ClipData* c = ops::clipById(data, id))
                    minStart = std::min(minStart, c->start);
            if (minStart != std::numeric_limits<int64_t>::max())
                offset = minStart - m_drag.minStart;
        } else if (now) {
            offset = now->start - m_drag.primaryStart;
            if (const auto loc = ops::findClip(data, m_drag.primary);
                loc && !(loc->track == m_drag.primaryTrack))
                track = trackLabel(m_drag.primaryTrack) + QStringLiteral(" > ")
                    + trackLabel(loc->track);
        }
        label.lines << draginfo::formatOffset(offset, fps);
        QString detail
            = tr("Start %1").arg(timecode::formatRecord(m_drag.primaryStart + offset, data));
        if (!track.isEmpty())
            detail += QStringLiteral("  ") + track;
        label.lines << detail;
        break;
    }
    case DragMode::TrimHead:
    case DragMode::TrimTail: {
        if (!now)
            break;
        const bool head = m_drag.mode == DragMode::TrimHead;
        const int64_t offset
            = head ? now->start - m_drag.primaryStart : now->end() - m_drag.primaryEnd;
        label.lines << draginfo::formatOffset(offset, fps)
                    << tr("Duration %1").arg(timecode::format(now->duration, fps));
        // Head: the clip before the new start and the clip's new first frame; tail: the
        // clip's new last frame and what follows it.
        if (head) {
            preview.left = sideOf(neighbour(data, now->id, false), now->start - 1, fps);
            preview.right = sideOf(now, now->start, fps);
        } else {
            preview.left = sideOf(now, now->end() - 1, fps);
            preview.right = sideOf(neighbour(data, now->id, true), now->end(), fps);
        }
        break;
    }
    case DragMode::Roll: {
        const ClipData* left
            = m_drag.rolls.isEmpty() ? nullptr : ops::clipById(data, m_drag.rolls[0].first);
        const ClipData* right
            = m_drag.rolls.isEmpty() ? nullptr : ops::clipById(data, m_drag.rolls[0].second);
        if (!left)
            break;
        label.lines << draginfo::formatOffset(left->end() - m_drag.primaryEnd, fps)
                    << tr("Edit %1").arg(timecode::formatRecord(left->end(), data));
        preview.left = sideOf(left, left->end() - 1, fps);
        preview.right = sideOf(right, left->end(), fps);
        break;
    }
    case DragMode::Slip: {
        if (!now || !was)
            break;
        // Slip moves the material under a fixed window: report the source offset in frames.
        const int64_t offset = usToFrames(now->sourceInUs - was->sourceInUs, fps);
        label.lines << draginfo::formatOffset(offset,
            fps) << tr("Source In %1").arg(timecode::format(usToFrames(now->sourceInUs, fps), fps));
        // Decision (spec 8.15): the clip's new first and last frame.
        preview.left = sideOf(now, now->start, fps);
        preview.right = sideOf(now, now->end() - 1, fps);
        break;
    }
    case DragMode::Slide: {
        if (!now)
            break;
        label.lines << draginfo::formatOffset(now->start - m_drag.primaryStart, fps)
                    << tr("Start %1").arg(timecode::formatRecord(now->start, data));
        // Decision (spec 8.15): the neighbours' frames that change: the previous clip's new
        // last frame and the next clip's new first frame.
        preview.left = sideOf(neighbour(data, now->id, false), now->start - 1, fps);
        preview.right = sideOf(neighbour(data, now->id, true), now->end(), fps);
        break;
    }
    case DragMode::MarkerMove:
        label.lines << draginfo::formatOffset(m_drag.markerNow - m_drag.markerFrame, fps)
                    << tr("Marker %1").arg(timecode::formatRecord(m_drag.markerNow, data));
        break;
    case DragMode::MarkerDuration:
        for (const Marker& m : data.markers) {
            if (m.frame == m_drag.markerFrame) {
                label.lines << tr("Duration %1").arg(timecode::format(m.duration, fps))
                            << tr("End %1").arg(timecode::formatRecord(m.end(), data));
                break;
            }
        }
        break;
    default:
        break;
    }

    if (!label.isEmpty())
        label.anchor = m_drag.lastPos;
    m_overlay.dragInfo = label;
    if (!(preview == m_trimPreview)) {
        m_trimPreview = preview;
        Q_EMIT trimPreviewChanged(m_trimPreview);
    }
}

void TimelineWidget::clearDragInfo()
{
    m_overlay.dragInfo = DragInfoLabel {};
    if (m_trimPreview.isActive()) {
        m_trimPreview = TrimPreview {};
        Q_EMIT trimPreviewChanged(m_trimPreview);
    }
}

} // namespace kk
