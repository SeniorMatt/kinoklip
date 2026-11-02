// SPDX-License-Identifier: GPL-3.0-or-later
// CompoundOps implementation: collapse / decompose, recursion checks, nested source sync.
#include "core/CompoundOps.h"

#include "core/EditOps.h"
#include "core/EditOpsInternal.h" // moveHead / insertSorted / rangeFree (shared EditOps helpers)

#include <QCoreApplication>
#include <QHash>

#include <algorithm>
#include <climits>
#include <limits>

namespace kk::compound {

using editops::detail::insertSorted;
using editops::detail::kTrackTypes;
/// Track types a compound clip can hold (captions stay in the parent).
constexpr TrackType kMediaTrackTypes[] = {TrackType::Video, TrackType::Audio};
using editops::detail::moveHead;
using editops::detail::rangeFree;

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::compound", text);
}

void setError(QString* error, const QString& text)
{
    if (error)
        *error = text;
}

/// Lowest track index >= @p from of @p type whose range [s, e) is free and unlocked; adds a
/// track at the top when none is.
int freeTrackFrom(TimelineData& data, TrackType type, int from, int64_t s, int64_t e)
{
    auto& list = data.tracks(type);
    for (int i = std::max(0, from); i < int(list.size()); ++i) {
        const TrackData& t = list[size_t(i)];
        if (!t.locked && rangeFree(t, s, e))
            return i;
    }
    return editops::addTrack(data, type);
}

} // namespace

QSet<quint64> directReferences(const TimelineData& data)
{
    QSet<quint64> result;
    for (TrackType type : kTrackTypes) {
        for (const TrackData& t : data.tracks(type)) {
            for (const ClipData& c : t.clips) {
                if (c.kind == ClipKind::Nested && c.nestedUid != 0)
                    result.insert(c.nestedUid);
            }
        }
    }
    return result;
}

QVector<quint64> reachableTimelines(quint64 root, const TimelineLookup& lookup)
{
    // Breadth-first walk; `seen` makes damaged (cyclic) data terminate.
    QVector<quint64> result;
    QSet<quint64> seen {root};
    QVector<quint64> queue {root};
    while (!queue.isEmpty()) {
        const quint64 uid = queue.takeFirst();
        const TimelineData* t = lookup(uid);
        if (!t)
            continue;
        for (const quint64 child : directReferences(*t)) {
            if (seen.contains(child))
                continue;
            seen.insert(child);
            result.append(child);
            queue.append(child);
        }
    }
    return result;
}

bool reaches(quint64 from, quint64 target, const TimelineLookup& lookup)
{
    return from == target || reachableTimelines(from, lookup).contains(target);
}

bool canNest(quint64 inner, quint64 outer, const TimelineLookup& lookup)
{
    return inner != 0 && lookup(inner) && !reaches(inner, outer, lookup);
}

int64_t nestedDurationUs(const TimelineData& nested)
{
    return framesToUs(std::max<int64_t>(1, nested.duration()), nested.fps);
}

ClipData makeNestedClip(const TimelineData& nested, const QString& mediaId)
{
    ClipData c;
    c.kind = ClipKind::Nested;
    c.nestedUid = nested.uid;
    c.mediaId = mediaId;
    c.name = nested.name;
    c.duration = std::max<int64_t>(1, nested.duration());
    c.sourceInUs = 0;
    c.sourceDurationUs = nestedDurationUs(nested);
    return c;
}

media::MediaInfo timelineInfo(const TimelineData& data)
{
    media::MediaInfo info;
    info.valid = true;
    info.width = info.displayWidth = data.width;
    info.height = info.displayHeight = data.height;
    info.frameRate = data.fps;
    info.durationUs = framesToUs(data.duration(), data.fps);
    info.container = QStringLiteral("timeline");
    const auto hasClips = [](const std::vector<TrackData>& tracks) {
        return std::any_of(
            tracks.begin(), tracks.end(), [](const TrackData& t) { return !t.clips.empty(); });
    };
    info.hasVideo = hasClips(data.videoTracks);
    info.hasAudio = hasClips(data.audioTracks);
    if (!info.hasVideo && !info.hasAudio) // an empty timeline still offers both
        info.hasVideo = info.hasAudio = true;
    if (info.hasAudio) {
        info.audioChannels = 2; // the mixer's output
        info.audioSampleRate = 48000;
    }
    return info;
}

// ---- Collapse ----

std::optional<CollapseResult> collapse(TimelineData& parent, const QVector<quint64>& clipIds,
    quint64 nestedUid, const QString& name, QString* error)
{
    // 1. The selection: location, range and the track span per type.
    struct Picked
    {
        TrackRef track;
        quint64 id;
    };
    std::vector<Picked> picked;
    int64_t s = std::numeric_limits<int64_t>::max();
    int64_t e = std::numeric_limits<int64_t>::min();
    int lo[2] = {INT_MAX, INT_MAX};
    int hi[2] = {-1, -1};
    QSet<quint64> unique;
    for (const quint64 id : clipIds) {
        if (unique.contains(id))
            continue;
        const auto loc = editops::findClip(parent, id);
        if (!loc)
            continue;
        // Captions stay on the parent's subtitle tracks (a compound clip holds pictures and
        // sound; subtitles belong to the timeline that is delivered).
        if (loc->track.type == TrackType::Subtitle)
            continue;
        unique.insert(id);
        const TrackData& track = *parent.track(loc->track);
        if (track.locked) {
            setError(error, tr("The selection contains clips on a locked track."));
            return std::nullopt;
        }
        const ClipData& c = track.clips[size_t(loc->clipIndex)];
        s = std::min(s, c.start);
        e = std::max(e, c.end());
        const int ti = loc->track.type == TrackType::Video ? 0 : 1;
        lo[ti] = std::min(lo[ti], loc->track.index);
        hi[ti] = std::max(hi[ti], loc->track.index);
        picked.push_back({loc->track, id});
    }
    if (picked.empty()) {
        setError(error, tr("Select the clips to put into the compound clip."));
        return std::nullopt;
    }

    // 2. The compound timeline: same format as the parent, ids continue the parent's id
    //    space so the copied clips keep their ids, link groups and effect ids.
    CollapseResult result;
    TimelineData& nested = result.nested;
    nested.uid = nestedUid;
    nested.compound = true;
    nested.useProjectSettings = parent.useProjectSettings;
    nested.retimeProcess = parent.retimeProcess;
    nested.name = name;
    nested.fps = parent.fps;
    nested.width = parent.width;
    nested.height = parent.height;
    nested.nextId = parent.nextId;
    for (TrackType type : kMediaTrackTypes) {
        const int ti = type == TrackType::Video ? 0 : 1;
        const int count = hi[ti] < 0 ? 1 : hi[ti] - lo[ti] + 1;
        for (int k = 0; k < count; ++k) {
            TrackData t;
            t.id = nested.allocateId();
            t.type = type;
            if (hi[ti] >= 0) {
                // Keep what decides how the content looks/sounds.
                const TrackData& src = parent.tracks(type)[size_t(lo[ti] + k)];
                t.name = src.name;
                t.enabled = src.enabled;
                t.muted = src.muted;
            }
            nested.tracks(type).push_back(std::move(t));
        }
    }
    for (const Picked& p : picked) {
        ClipData c = *editops::clipById(parent, p.id);
        c.start -= s;
        const int ti = p.track.type == TrackType::Video ? 0 : 1;
        insertSorted(nested.tracks(p.track.type)[size_t(p.track.index - lo[ti])], std::move(c));
    }
    editops::normalizeIds(nested);

    // 3. Out of the parent (leaving the range empty), then the instance(s) in.
    for (const Picked& p : picked) {
        TrackData& t = *parent.track(p.track);
        const int i = t.indexOfClip(p.id);
        if (i >= 0)
            t.clips.erase(t.clips.begin() + i);
    }
    const quint64 group = (hi[0] >= 0 && hi[1] >= 0) ? parent.allocateId() : 0;
    for (TrackType type : kMediaTrackTypes) {
        const int ti = type == TrackType::Video ? 0 : 1;
        if (hi[ti] < 0)
            continue;
        // Lowest selected track first; an unselected clip inside the range pushes the
        // instance up (never over it).
        const int index = freeTrackFrom(parent, type, lo[ti], s, e);
        ClipData inst = makeNestedClip(nested, {});
        inst.id = parent.allocateId();
        inst.start = s;
        inst.duration = e - s;
        inst.linkGroup = group;
        result.instanceIds.append(inst.id);
        insertSorted(parent.tracks(type)[size_t(index)], std::move(inst));
    }
    return result;
}

// ---- Decompose ----

std::optional<QVector<quint64>> decompose(TimelineData& parent, const QVector<quint64>& clipIds,
    const TimelineData& nested, QString* error)
{
    if (!(nested.fps == parent.fps)) {
        setError(error, tr("The compound clip has a different frame rate than this timeline."));
        return std::nullopt;
    }
    // Validate everything before changing anything.
    struct Instance
    {
        TrackRef track;
        ClipData clip;
    };
    std::vector<Instance> instances;
    for (const quint64 id : clipIds) {
        const auto loc = editops::findClip(parent, id);
        if (!loc)
            continue;
        const TrackData& track = *parent.track(loc->track);
        const ClipData& c = track.clips[size_t(loc->clipIndex)];
        if (c.kind != ClipKind::Nested || c.nestedUid != nested.uid) {
            setError(error, tr("The clip is not an instance of this compound clip."));
            return std::nullopt;
        }
        if (track.locked) {
            setError(error, tr("The clip is on a locked track."));
            return std::nullopt;
        }
        if (retime::isRetimed(c)) {
            setError(error, tr("Reset the speed of the compound clip to 100% first."));
            return std::nullopt;
        }
        instances.push_back({loc->track, c});
    }
    if (instances.empty()) {
        setError(error, tr("Select a compound clip to decompose."));
        return std::nullopt;
    }

    TimelineData out = parent;
    // Remove the instances first: their ranges become free for the content.
    for (const Instance& in : instances) {
        TrackData& t = *out.track(in.track);
        const int i = t.indexOfClip(in.clip.id);
        if (i >= 0)
            t.clips.erase(t.clips.begin() + i);
    }
    // Links inside the compound clip survive, across tracks and between the V and A instance:
    // one new group per nested group for the whole operation.
    editops::detail::GroupRemap groups;
    QVector<quint64> restored;
    for (const Instance& in : instances) {
        const ClipData& inst = in.clip;
        const int64_t windowStart = usToFrames(inst.sourceInUs, nested.fps);
        const int64_t windowEnd = windowStart + inst.duration;
        const int64_t shift = inst.start - windowStart; // nested frame -> parent frame
        const auto& srcTracks = nested.tracks(in.track.type);
        int previous = in.track.index - 1;
        for (size_t k = 0; k < srcTracks.size(); ++k) {
            // Nominal position base + k, never below the previous nested track's target.
            const int nominal = std::max(in.track.index + int(k), previous + 1);
            std::vector<ClipData> pieces;
            for (const ClipData& c : srcTracks[k].clips) {
                if (c.end() <= windowStart || c.start >= windowEnd)
                    continue;
                ClipData piece = c;
                if (piece.start < windowStart) {
                    moveHead(piece, windowStart - piece.start, nested.fps);
                    piece.transitionIn.reset();
                    piece.fadeInFrames = 0;
                }
                if (piece.end() > windowEnd) {
                    piece.duration = windowEnd - piece.start;
                    piece.transitionOut.reset();
                    piece.fadeOutFrames = 0;
                }
                piece.start += shift;
                pieces.push_back(std::move(piece));
            }
            if (pieces.empty()) {
                previous = nominal; // keep the relative track layout
                continue;
            }
            const int64_t ps = pieces.front().start;
            const int64_t pe = pieces.back().end();
            const int target = freeTrackFrom(out, in.track.type, nominal, ps, pe);
            previous = target;
            TrackData& dst = out.tracks(in.track.type)[size_t(target)];
            for (ClipData& piece : pieces) {
                piece.id = out.allocateId();
                piece.linkGroup = groups.map(out, piece.linkGroup);
                for (EffectInstance& fx : piece.effects)
                    fx.id = out.allocateId();
                restored.append(piece.id);
                insertSorted(dst, std::move(piece));
            }
        }
    }
    parent = std::move(out);
    return restored;
}

// ---- Derived data ----

bool nestedSourcesOutdated(const TimelineData& data, const TimelineLookup& lookup)
{
    for (TrackType type : kTrackTypes) {
        for (const TrackData& t : data.tracks(type)) {
            for (const ClipData& c : t.clips) {
                if (c.kind != ClipKind::Nested)
                    continue;
                if (const TimelineData* n = lookup(c.nestedUid);
                    n && c.sourceDurationUs != nestedDurationUs(*n))
                    return true;
            }
        }
    }
    return false;
}

bool syncNestedSources(TimelineData& data, const TimelineLookup& lookup)
{
    bool changed = false;
    for (TrackType type : kTrackTypes) {
        for (TrackData& t : data.tracks(type)) {
            for (ClipData& c : t.clips) {
                if (c.kind != ClipKind::Nested)
                    continue;
                if (const TimelineData* n = lookup(c.nestedUid)) {
                    const int64_t us = nestedDurationUs(*n);
                    if (c.sourceDurationUs != us) {
                        c.sourceDurationUs = us;
                        changed = true;
                    }
                }
            }
        }
    }
    return changed;
}

} // namespace kk::compound
