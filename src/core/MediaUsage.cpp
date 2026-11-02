// SPDX-License-Identifier: GPL-3.0-or-later
// Media usage counting over every timeline of a project.
#include "core/MediaUsage.h"

#include "core/Project.h"

namespace kk::usage {

namespace {

/// Timeline uid -> media pool item id.
QHash<quint64, QString> timelineItems(const MediaPoolState& pool)
{
    QHash<quint64, QString> map;
    for (const MediaItem& m : pool.items) {
        if (m.isTimeline())
            map.insert(m.timelineUid, m.id);
    }
    return map;
}

QString itemFor(const ClipData& clip, const QHash<quint64, QString>& timelines)
{
    if (clip.kind == ClipKind::Media)
        return clip.mediaId;
    if (clip.kind == ClipKind::Nested)
        return timelines.value(clip.nestedUid);
    return {};
}

} // namespace

Counts compute(
    const std::vector<std::shared_ptr<const TimelineData>>& timelines, const MediaPoolState& pool)
{
    const QHash<quint64, QString> items = timelineItems(pool);
    Counts counts;
    // A linked pair (same linkGroup, same item) is one use: remember the groups already counted
    // per timeline. Unlinked clips (group 0) always count.
    QSet<std::pair<quint64, QString>> groups;
    for (const auto& t : timelines) {
        if (!t)
            continue;
        groups.clear();
        for (const auto* tracks : {&t->videoTracks, &t->audioTracks}) {
            for (const TrackData& track : *tracks) {
                for (const ClipData& clip : track.clips) {
                    const QString id = itemFor(clip, items);
                    if (id.isEmpty())
                        continue;
                    if (clip.linkGroup != 0) {
                        const std::pair<quint64, QString> key {clip.linkGroup, id};
                        if (groups.contains(key))
                            continue;
                        groups.insert(key);
                    }
                    ++counts[id];
                }
            }
        }
    }
    return counts;
}

Counts compute(const Project& project)
{
    std::vector<std::shared_ptr<const TimelineData>> timelines;
    for (const quint64 uid : project.timelineUids())
        timelines.push_back(project.timelineData(uid));
    return compute(timelines, project.mediaPool()->state());
}

QVector<quint64> clipsUsing(
    const TimelineData& data, const QSet<QString>& itemIds, const MediaPoolState& pool)
{
    const QHash<quint64, QString> items = timelineItems(pool);
    QVector<quint64> ids;
    for (const auto* tracks : {&data.videoTracks, &data.audioTracks}) {
        for (const TrackData& track : *tracks) {
            for (const ClipData& clip : track.clips) {
                if (itemIds.contains(itemFor(clip, items)))
                    ids.append(clip.id);
            }
        }
    }
    return ids;
}

QString itemIdOf(const ClipData& clip, const MediaPoolState& pool)
{
    if (clip.kind == ClipKind::Media)
        return clip.mediaId;
    if (clip.kind == ClipKind::Nested) {
        for (const MediaItem& m : pool.items) {
            if (m.isTimeline() && m.timelineUid == clip.nestedUid)
                return m.id;
        }
    }
    return {};
}

QStringList unusedFileItems(const MediaPoolState& pool, const Counts& counts)
{
    QStringList ids;
    for (const MediaItem& m : pool.items) {
        if (m.kind == MediaItemKind::File && counts.value(m.id, 0) == 0)
            ids.append(m.id);
    }
    return ids;
}

} // namespace kk::usage
