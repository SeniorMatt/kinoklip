// SPDX-License-Identifier: GPL-3.0-or-later
// UndoSnapshots implementation.
#include "core/UndoSnapshots.h"

#include <QHash>
#include <QSet>

namespace kk::undosnap {

namespace {

constexpr TrackType kTypes[] = {TrackType::Video, TrackType::Audio, TrackType::Subtitle};

/// The reference track with id @p trackId, its index in its list, or -1.
int findTrack(const TimelineData& shell, TrackType type, quint64 trackId, int hint)
{
    const auto& tracks = shell.tracks(type);
    if (hint >= 0 && hint < int(tracks.size()) && tracks[size_t(hint)].id == trackId)
        return hint;
    for (int i = 0; i < int(tracks.size()); ++i) {
        if (tracks[size_t(i)].id == trackId)
            return i;
    }
    return -1;
}

} // namespace

std::shared_ptr<const PackedTimeline> pack(const std::shared_ptr<const TimelineData>& data,
    const std::shared_ptr<const PackedTimeline>& reference)
{
    if (!data)
        return nullptr;
    if (reference && reference->full.lock() == data)
        return reference; // the same state (an unchanged timeline, or the step before)

    auto packed = std::make_shared<PackedTimeline>();
    packed->full = data;

    // The shell: a copy without the clips. Copying the tracks with their clips and clearing
    // them costs one transient clip copy; it keeps this code independent of TrackData's
    // fields (a field added later is stored without anyone remembering to add it here).
    auto shell = std::make_shared<TimelineData>(*data);
    shell->nested = {};
    for (TrackType type : kTypes) {
        for (TrackData& t : shell->tracks(type))
            std::vector<ClipData>().swap(t.clips);
    }
    if (reference && reference->shell && *reference->shell == *shell)
        packed->shell = reference->shell;
    else
        packed->shell = std::move(shell);

    for (TrackType type : kTypes) {
        const int ti = trackTypeIndex(type);
        const auto& tracks = data->tracks(type);
        auto& out = packed->clips[size_t(ti)];
        out.resize(tracks.size());
        for (int i = 0; i < int(tracks.size()); ++i) {
            const TrackData& track = tracks[size_t(i)];
            auto& list = out[size_t(i)];
            list.reserve(track.clips.size());
            const std::vector<std::shared_ptr<const ClipData>>* ref = nullptr;
            if (reference) {
                const int r = findTrack(*reference->shell, type, track.id, i);
                if (r >= 0)
                    ref = &reference->clips[size_t(ti)][size_t(r)];
            }
            // Clips usually keep their order; walk both lists and fall back to an id lookup
            // (built once per track) when they diverge (an insert, a delete, a move).
            QHash<quint64, int> refIndex;
            size_t cursor = 0;
            for (const ClipData& clip : track.clips) {
                std::shared_ptr<const ClipData> shared;
                if (ref) {
                    int at = -1;
                    if (cursor < ref->size() && (*ref)[cursor]->id == clip.id) {
                        at = int(cursor);
                    } else {
                        if (refIndex.isEmpty()) {
                            refIndex.reserve(qsizetype(ref->size()));
                            for (size_t k = 0; k < ref->size(); ++k)
                                refIndex.insert((*ref)[k]->id, int(k));
                        }
                        at = refIndex.value(clip.id, -1);
                    }
                    if (at >= 0) {
                        cursor = size_t(at) + 1;
                        if (*(*ref)[size_t(at)] == clip)
                            shared = (*ref)[size_t(at)];
                    }
                }
                list.push_back(shared ? std::move(shared) : std::make_shared<const ClipData>(clip));
            }
        }
    }
    return packed;
}

std::shared_ptr<const TimelineData> unpack(const PackedTimeline& packed)
{
    if (auto alive = packed.full.lock())
        return alive;
    auto data = std::make_shared<TimelineData>(*packed.shell);
    for (TrackType type : kTypes) {
        const auto& lists = packed.clips[size_t(trackTypeIndex(type))];
        auto& tracks = data->tracks(type);
        for (size_t i = 0; i < tracks.size() && i < lists.size(); ++i) {
            tracks[i].clips.reserve(lists[i].size());
            for (const auto& clip : lists[i])
                tracks[i].clips.push_back(*clip);
        }
    }
    std::shared_ptr<const TimelineData> result = std::move(data);
    packed.full = result;
    return result;
}

size_t exclusiveBytes(const PackedTimeline& packed, const PackedTimeline* other)
{
    QSet<const void*> shared;
    if (other) {
        shared.insert(other->shell.get());
        for (const auto& lists : other->clips) {
            for (const auto& list : lists) {
                for (const auto& clip : list)
                    shared.insert(clip.get());
            }
        }
    }
    size_t bytes = 0;
    if (!shared.contains(packed.shell.get()))
        bytes += sizeof(TimelineData);
    for (const auto& lists : packed.clips) {
        for (const auto& list : lists) {
            bytes += list.size() * sizeof(std::shared_ptr<const ClipData>);
            for (const auto& clip : list) {
                if (!shared.contains(clip.get()))
                    bytes += sizeof(ClipData);
            }
        }
    }
    return bytes;
}

} // namespace kk::undosnap
