// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineController, part: moving clips between tracks by key (spec 21.10) and Delete Empty
// Tracks (spec 12.8).
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineEditUtil.h"

namespace kk {

namespace ops = editops;
using timelineedit::runEdit;

void TimelineController::moveSelectionVertically(int up)
{
    // Only the clips themselves, not their linked partners (they keep their tracks).
    const QVector<quint64> selected = m_ctx.selection->clips();
    const TimelineData& data = timeline()->data();
    QVector<quint64> video;
    QVector<quint64> audio;
    for (quint64 id : selected)
        if (const auto loc = ops::findClip(data, id)) // subtitles stack upwards like video
            (loc->track.type == TrackType::Audio ? audio : video).append(id);
    // On screen, video track numbers grow upwards and audio track numbers downwards.
    const bool useVideo = !video.isEmpty();
    const QVector<quint64>& ids = useVideo ? video : audio;
    if (ids.isEmpty())
        return;
    const int delta = useVideo ? up : -up;
    runEdit(project(), up > 0 ? tr("Move Clips Up") : tr("Move Clips Down"),
        [&](TimelineData& d) {
            // moveClips() clamps at the outermost track: a clamp to no movement is a no-op.
            const auto before = ops::findClip(d, ids.front());
            if (!ops::moveClips(d, ids, 0, delta))
                return false;
            const auto after = ops::findClip(d, ids.front());
            return before && after && !(before->track == after->track);
        });
}

void TimelineController::deleteEmptyTracks()
{
    const bool removed = runEdit(project(), tr("Delete Empty Tracks"), [](TimelineData& d) {
        bool any = false;
        for (const TrackType type : {TrackType::Video, TrackType::Audio, TrackType::Subtitle}) {
            // From the top index down, so the remaining indices stay valid.
            for (int i = int(d.tracks(type).size()) - 1; i >= 0; --i) {
                const TrackData& t = d.tracks(type)[size_t(i)];
                const bool keepOne = type != TrackType::Subtitle && d.tracks(type).size() <= 1;
                if (!t.clips.empty() || t.locked || keepOne)
                    continue;
                any = ops::removeTrack(d, TrackRef {type, i}) || any;
            }
        }
        return any;
    });
    if (removed) {
        const TimelineData& d = timeline()->data();
        m_targetVideo = std::min(m_targetVideo, int(d.videoTracks.size()) - 1);
        m_targetAudio = std::min(m_targetAudio, int(d.audioTracks.size()) - 1);
        emit stateChanged();
    }
}

} // namespace kk
