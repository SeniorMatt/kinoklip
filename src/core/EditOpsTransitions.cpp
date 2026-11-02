// SPDX-License-Identifier: GPL-3.0-or-later
// EditOps: transition windows, length / alignment changes and media-handle checks
// (declared in EditOps.h, spec 13.5 / 13.7 / 13.9).
#include "core/EditOps.h"
#include "core/EditOpsInternal.h"

#include <algorithm>

namespace kk::editops {

using namespace detail;

namespace {

/// The clips around one transition edge: the clip itself, the edge, the neighbour at the cut.
struct EdgeClips
{
    const ClipData* clip = nullptr;
    const ClipData* outgoing = nullptr; ///< left side of the cut (nullptr: fade-in)
    const ClipData* incoming = nullptr; ///< right side of the cut (nullptr: fade-out)
    const TrackData* track = nullptr;
};

EdgeClips edgeClips(const TimelineData& data, quint64 clipId, bool head)
{
    EdgeClips e;
    const auto loc = findClip(data, clipId);
    if (!loc)
        return e;
    e.track = data.track(loc->track);
    const auto& clips = e.track->clips;
    const size_t i = size_t(loc->clipIndex);
    e.clip = &clips[i];
    if (head) {
        e.incoming = e.clip;
        if (i > 0 && clips[i - 1].end() == e.clip->start)
            e.outgoing = &clips[i - 1];
    } else {
        e.outgoing = e.clip;
        if (i + 1 < clips.size() && clips[i + 1].start == e.clip->end())
            e.incoming = &clips[i + 1];
    }
    return e;
}

/// Frames before the cut for a transition of @p d frames (as TransitionData::framesBeforeCut).
int64_t beforeCut(int64_t d, TransitionAlignment alignment)
{
    TransitionData t;
    t.duration = d;
    t.alignment = alignment;
    return t.framesBeforeCut();
}

} // namespace

quint64 cutNeighbour(const TimelineData& data, quint64 clipId, bool head)
{
    const EdgeClips e = edgeClips(data, clipId, head);
    if (!e.clip)
        return 0;
    const ClipData* other = head ? e.outgoing : e.incoming;
    return other ? other->id : 0;
}

std::optional<std::pair<int64_t, int64_t>> transitionWindow(
    const TimelineData& data, quint64 clipId, bool head)
{
    const EdgeClips e = edgeClips(data, clipId, head);
    if (!e.clip)
        return std::nullopt;
    const std::optional<TransitionData>& slot = head ? e.clip->transitionIn : e.clip->transitionOut;
    if (!slot)
        return std::nullopt;
    const int64_t d = std::max<int64_t>(1, slot->duration);
    if (e.outgoing && e.incoming) {
        const int64_t start
            = e.incoming->start - std::clamp<int64_t>(slot->framesBeforeCut(), 0, d);
        return std::make_pair(start, start + d);
    }
    const int64_t len = std::min(d, e.clip->duration);
    return head ? std::make_pair(e.clip->start, e.clip->start + len)
                : std::make_pair(e.clip->end() - len, e.clip->end());
}

int64_t maxTransitionDuration(const TimelineData& data, quint64 clipId, bool head,
    TransitionAlignment alignment, bool limitByHandles)
{
    const EdgeClips e = edgeClips(data, clipId, head);
    if (!e.clip)
        return 0;
    if (!e.outgoing || !e.incoming)
        return e.clip->duration; // a fade lies inside its clip
    const int64_t a = e.outgoing->duration;
    const int64_t b = e.incoming->duration;
    // Before the cut the window lies in the outgoing clip (and needs incoming handles), after
    // it in the incoming clip (and needs outgoing handles).
    const int64_t tailHandle
        = limitByHandles ? maxTailExtension(*e.outgoing, data.fps) : kUnbounded;
    const int64_t headHandle
        = limitByHandles ? maxHeadExtension(*e.incoming, data.fps) : kUnbounded;
    switch (alignment) {
    case TransitionAlignment::StartOnCut:
        return std::max<int64_t>(0, std::min(b, tailHandle));
    case TransitionAlignment::EndOnCut:
        return std::max<int64_t>(0, std::min(a, headHandle));
    case TransitionAlignment::Centre:
        break;
    }
    // d/2 frames before the cut, d - d/2 after: d <= 2a + 1 (before) and d <= 2b (after).
    const int64_t before = std::min(a, headHandle);
    const int64_t after = std::min(b, tailHandle);
    return std::max<int64_t>(0, std::min(2 * before + 1, 2 * after));
}

HandleShortfall transitionShortfall(const TimelineData& data, quint64 clipId, bool head,
    int64_t duration, TransitionAlignment alignment)
{
    HandleShortfall s;
    const EdgeClips e = edgeClips(data, clipId, head);
    if (!e.outgoing || !e.incoming || duration < 1)
        return s;
    const int64_t before = beforeCut(duration, alignment);
    const int64_t after = duration - before;
    s.outgoing = std::max<int64_t>(0, after - maxTailExtension(*e.outgoing, data.fps));
    s.incoming = std::max<int64_t>(0, before - maxHeadExtension(*e.incoming, data.fps));
    return s;
}

int64_t setTransitionDuration(TimelineData& data, quint64 clipId, bool head, int64_t frames)
{
    const auto loc = findClip(data, clipId);
    if (!loc || data.track(loc->track)->locked)
        return 0;
    ClipData& clip = data.track(loc->track)->clips[size_t(loc->clipIndex)];
    std::optional<TransitionData>& slot = head ? clip.transitionIn : clip.transitionOut;
    if (!slot)
        return 0;
    const int64_t limit
        = std::max<int64_t>(1, maxTransitionDuration(data, clipId, head, slot->alignment, false));
    slot->duration = std::clamp<int64_t>(frames, 1, limit);
    return slot->duration;
}

bool setTransitionAlignment(
    TimelineData& data, quint64 clipId, bool head, TransitionAlignment alignment)
{
    const auto loc = findClip(data, clipId);
    if (!loc || data.track(loc->track)->locked)
        return false;
    const int64_t limit
        = std::max<int64_t>(1, maxTransitionDuration(data, clipId, head, alignment, false));
    ClipData& clip = data.track(loc->track)->clips[size_t(loc->clipIndex)];
    std::optional<TransitionData>& slot = head ? clip.transitionIn : clip.transitionOut;
    if (!slot || slot->alignment == alignment)
        return false;
    slot->alignment = alignment;
    slot->duration = std::min(slot->duration, limit);
    return true;
}

} // namespace kk::editops
