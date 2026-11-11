// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/timeline/VimTimelineHost.h"

#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "core/TimelineData.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/TimelineController.h"

#include <QAction>

#include <algorithm>

namespace kk {

VimTimelineHost::VimTimelineHost(AppContext& ctx, TimelineController* controller)
    : m_ctx(ctx)
    , m_controller(controller)
{
}

bool VimTimelineHost::trigger(const QString& actionId)
{
    if (!m_ctx.actions || !m_ctx.actions->contains(actionId))
        return false;
    QAction* action = m_ctx.actions->action(actionId);
    if (!action->isEnabled())
        return false;
    action->trigger();
    return true;
}

int64_t VimTimelineHost::playhead() const
{
    return m_controller->timeline()->playhead();
}

void VimTimelineHost::setPlayhead(int64_t frame)
{
    m_controller->timeline()->setPlayhead(std::max<int64_t>(0, frame));
}

bool VimTimelineHost::selectClipsAtPlayhead(int count)
{
    const TimelineData& data = m_controller->timeline()->data();
    const int64_t frame = m_controller->timeline()->playhead();
    // The target video track first, then the top-most video track, then the audio tracks.
    std::vector<std::pair<TrackType, int>> order;
    const int target = m_controller->targetVideoTrack();
    if (target >= 0)
        order.emplace_back(TrackType::Video, target);
    for (int i = int(data.videoTracks.size()) - 1; i >= 0; --i)
        order.emplace_back(TrackType::Video, i);
    for (int i = 0; i < int(data.audioTracks.size()); ++i)
        order.emplace_back(TrackType::Audio, i);
    for (const auto& [type, index] : order) {
        const auto& tracks = data.tracks(type);
        if (index >= int(tracks.size()))
            continue;
        const TrackData& track = tracks[size_t(index)];
        const ClipData* under = track.clipAt(frame);
        if (!under)
            continue;
        const int first = track.indexOfClip(under->id);
        QVector<quint64> ids;
        for (int i = first; i < int(track.clips.size()) && ids.size() < count; ++i)
            ids.append(track.clips[size_t(i)].id);
        m_ctx.selection->setClips(ids);
        return true;
    }
    return false;
}

void VimTimelineHost::selectRange(int64_t a, int64_t b)
{
    const TimelineData& data = m_controller->timeline()->data();
    const int64_t lo = std::min(a, b);
    const int64_t hi = std::max(a, b);
    QVector<quint64> ids;
    for (const TrackType type : {TrackType::Video, TrackType::Audio}) {
        for (const TrackData& track : data.tracks(type)) {
            for (const ClipData& clip : track.clips) {
                if (clip.start <= hi && clip.end() > lo)
                    ids.append(clip.id);
            }
        }
    }
    m_ctx.selection->setClips(m_controller->videoClipCurrent(ids));
}

void VimTimelineHost::clearSelection()
{
    m_ctx.selection->clear();
}

void VimTimelineHost::moveTarget(int delta)
{
    const int count = int(m_controller->timeline()->data().videoTracks.size());
    if (count == 0)
        return;
    const int current = std::max(0, m_controller->targetVideoTrack());
    m_controller->setTargetTrack(TrackType::Video, std::clamp(current + delta, 0, count - 1));
}

void VimTimelineHost::openCommandLine()
{
    if (onCommandLine)
        onCommandLine();
}

void VimTimelineHost::openSearch()
{
    if (onSearch)
        onSearch();
}

void VimTimelineHost::runEx(const QString& line)
{
    if (onEx)
        onEx(line);
}

} // namespace kk
