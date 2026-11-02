// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineWidget, stacked timelines (spec 31.9): planning the drop of clips dragged from the
// other timeline (MIME mime::TimelineClips). The ghost shows the copies where they will land;
// the drop runs TimelineController::copyClipsFrom() (one undo step).
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragMime.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>

#include <algorithm>
#include <cmath>

namespace kk {

void TimelineWidget::planClipCopy(
    const QMimeData* mime, const TimelineHit& hit, QPointF pos, DropPlan& plan)
{
    const QJsonObject o
        = QJsonDocument::fromJson(mime->data(QLatin1String(mime::TimelineClips))).object();
    const quint64 uid = quint64(o.value(QLatin1String("timeline")).toInteger());
    const auto source = m_ctx.project->timelineData(uid);
    if (!source || uid == m_ctx.project->activeTimelineUid())
        return; // within one timeline a drag is a move, handled by the input state machine
    QVector<quint64> ids;
    for (const QJsonValue& v : o.value(QLatin1String("clips")).toArray())
        ids.append(quint64(v.toInteger()));
    const editops::Clipboard board = editops::copyClips(*source, ids);
    if (board.isEmpty())
        return;
    const int64_t grab = o.value(QLatin1String("grabFrames")).toInteger();
    plan.kind = DropPlan::Kind::TimelineClips;
    plan.sourceUid = uid;
    plan.sourceClips = ids;
    plan.copyTrack = hit.track;
    // Timeline rates may differ: the clips keep their frame counts (like paste).
    plan.copyFrame = std::max<int64_t>(0, m_layout.nearestFrameAtX(pos.x()) - grab);
    // Ghosts: each clip on base + offset of its type (the drop row's index for both types).
    const TimelineData& data = m_ctx.project->timeline()->data();
    QVector<editops::PlacedClip> ghosts;
    for (const editops::ClipboardClip& c : board.clips) {
        if (c.type == TrackType::Subtitle)
            continue;
        const int index = std::max(0, hit.track.index) + c.trackOffset;
        if (index >= int(data.tracks(c.type).size()))
            continue; // a track the drop adds: not drawn
        editops::PlacedClip p;
        p.track = TrackRef {c.type, index};
        p.clip = c.clip;
        p.clip.start = plan.copyFrame + c.clip.start;
        ghosts.append(p);
    }
    plan.groups = {ghosts};
}

} // namespace kk
