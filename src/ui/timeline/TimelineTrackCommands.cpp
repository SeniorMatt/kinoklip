// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineController, part: moving clips between tracks by key (spec 21.10), Delete Empty
// Tracks (spec 12.8), Move Track Up / Down (12.9) and the track height presets (12.10).
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineEditUtil.h"
#include "ui/timeline/TimelineLayout.h"

#include <algorithm>

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
    runEdit(project(), up > 0 ? tr("Move Clips Up") : tr("Move Clips Down"), [&](TimelineData& d) {
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

bool TimelineController::moveTrack(TrackRef ref, int up)
{
    // On screen, video (and subtitle) numbers grow upwards, audio numbers downwards.
    const int delta = ref.type == TrackType::Audio ? -up : up;
    const TrackRef other {ref.type, ref.index + delta};
    const bool moved = runEdit(
        project(), up > 0 ? tr("Move Track Up") : tr("Move Track Down"), [&](TimelineData& d) {
            auto& list = d.tracks(ref.type);
            if (ref.index < 0 || other.index < 0 || ref.index >= int(list.size())
                || other.index >= int(list.size()))
                return false;
            std::swap(list[size_t(ref.index)], list[size_t(other.index)]);
            return true;
        });
    if (!moved)
        return false;
    // The destination patch stays on the track it was on.
    int& target = ref.type == TrackType::Video ? m_targetVideo : m_targetAudio;
    if (ref.type != TrackType::Subtitle) {
        if (target == ref.index)
            target = other.index;
        else if (target == other.index)
            target = ref.index;
    }
    emit stateChanged();
    return true;
}

int TimelineController::presetHeight(TrackType type, TrackHeightPreset preset)
{
    using namespace timelinemetrics;
    // Small: name band plus a thin strip of content; Large: big thumbnails / waveforms.
    // Medium is the default height (stored as 0, so it follows a change of the default).
    constexpr int kSmall[] = {38, 30, 30}; // video, audio, subtitle
    constexpr int kLarge[] = {120, 96, 64};
    const int i = type == TrackType::Video ? 0 : (type == TrackType::Audio ? 1 : 2);
    switch (preset) {
    case TrackHeightPreset::Small:
        return std::max(kMinTrackHeight, kSmall[i]);
    case TrackHeightPreset::Medium:
        return 0;
    case TrackHeightPreset::Large:
        return std::min(kMaxTrackHeight, kLarge[i]);
    }
    return 0;
}

std::optional<TimelineController::TrackHeightPreset> TimelineController::heightPresetOf(
    const TrackData& track)
{
    for (const auto p :
        {TrackHeightPreset::Small, TrackHeightPreset::Medium, TrackHeightPreset::Large}) {
        if (track.height == presetHeight(track.type, p))
            return p;
    }
    return std::nullopt;
}

bool TimelineController::setTrackHeightPreset(
    TrackRef ref, TrackHeightPreset preset, bool allTracks)
{
    return runEdit(project(), tr("Track Height"), [&](TimelineData& d) {
        bool any = false;
        auto apply = [&](TrackData& t) {
            const int h = presetHeight(t.type, preset);
            if (t.height != h) {
                t.height = h;
                any = true;
            }
        };
        if (allTracks) {
            for (const TrackType type : {TrackType::Video, TrackType::Audio, TrackType::Subtitle})
                for (TrackData& t : d.tracks(type))
                    apply(t);
        } else if (TrackData* t = d.track(ref)) {
            apply(*t);
        }
        return any;
    });
}

} // namespace kk
