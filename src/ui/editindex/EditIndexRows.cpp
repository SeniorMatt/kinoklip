// SPDX-License-Identifier: GPL-3.0-or-later
// EditIndexRows — events and markers of a timeline for the Edit Index.
#include "ui/editindex/EditIndexRows.h"

#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/MediaPool.h"
#include "ui/common/SourceTimecode.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace kk::editindex {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::EditIndex", text);
}

QString kindName(ClipKind kind)
{
    switch (kind) {
    case ClipKind::Media:
        return {};
    case ClipKind::Title:
        return tr("Title");
    case ClipKind::Solid:
        return tr("Solid Color");
    case ClipKind::Nested:
        return tr("Compound");
    case ClipKind::Adjustment:
        return tr("Adjustment");
    case ClipKind::Generator:
        return tr("Generator");
    case ClipKind::Subtitle:
        return tr("Subtitle");
    }
    return {};
}

QString transitionName(const TransitionData& t)
{
    const EffectDescriptor* d = EffectRegistry::instance().find(t.type);
    return d ? d->name : t.type;
}

QString notesOf(const ClipData& clip, const MediaPool* pool)
{
    QStringList notes;
    if (!clip.enabled)
        notes << tr("Disabled");
    if (clip.kind == ClipKind::Media && pool) {
        const MediaItem* item = pool->item(clip.mediaId);
        if (!item || pool->isOffline(clip.mediaId))
            notes << tr("Offline");
    }
    if (clip.transitionIn)
        notes << tr("In: %1 (%2 fr)")
                     .arg(transitionName(*clip.transitionIn))
                     .arg(clip.transitionIn->duration);
    if (clip.transitionOut)
        notes << tr("Out: %1 (%2 fr)")
                     .arg(transitionName(*clip.transitionOut))
                     .arg(clip.transitionOut->duration);
    if (clip.retime.isAnimated())
        notes << tr("Speed ramp");
    else if (std::abs(clip.speed - 1.0) > 1e-9)
        notes << tr("Speed %1%").arg(clip.speed * 100.0, 0, 'f', 1);
    return notes.join(QStringLiteral(", "));
}

} // namespace

std::vector<EventRow> events(const TimelineData& data, const MediaPool* pool)
{
    std::vector<EventRow> rows;
    for (TrackType type : {TrackType::Video, TrackType::Audio, TrackType::Subtitle}) {
        const auto& tracks = data.tracks(type);
        for (int t = 0; t < int(tracks.size()); ++t) {
            for (const ClipData& clip : tracks[size_t(t)].clips) {
                EventRow row;
                row.clipId = clip.id;
                row.track = {type, t};
                row.trackName = trackDisplayName(tracks[size_t(t)], t);
                row.name = clip.name;
                const MediaItem* item
                    = (pool && clip.kind == ClipKind::Media) ? pool->item(clip.mediaId) : nullptr;
                row.reel = item ? QFileInfo(item->path).completeBaseName() : kindName(clip.kind);
                if (row.reel.isEmpty() && item)
                    row.reel = item->name;
                row.recordIn = clip.start;
                row.recordOut = clip.end();
                row.sourceIn = sourceTimecode(
                    clip, clip.sourceTimeUsAt(clip.start, data.fps), pool, data.fps);
                row.sourceOut = sourceTimecode(
                    clip, clip.sourceTimeUsAt(clip.end(), data.fps), pool, data.fps);
                row.notes = notesOf(clip, pool);
                row.enabled = clip.enabled;
                rows.push_back(std::move(row));
            }
        }
    }
    // Stable: equal record ins keep the video / audio / subtitle, low-to-high track order.
    std::stable_sort(rows.begin(), rows.end(),
        [](const EventRow& a, const EventRow& b) { return a.recordIn < b.recordIn; });
    for (size_t i = 0; i < rows.size(); ++i)
        rows[i].number = int(i) + 1;
    return rows;
}

std::vector<MarkerRow> markers(const TimelineData& data)
{
    std::vector<MarkerRow> rows;
    for (const Marker& m : data.markers) {
        rows.push_back({0, 0, m.frame, m.frame, m.name, m.color, m.note, tr("Timeline"), false});
    }
    for (TrackType type : {TrackType::Video, TrackType::Audio, TrackType::Subtitle}) {
        for (const TrackData& track : data.tracks(type)) {
            for (const ClipData& clip : track.clips) {
                for (const Marker& m : clip.markers) {
                    // Markers trimmed out of the clip stay stored but are not shown (spec 18.4).
                    if (m.frame < 0 || m.frame >= clip.duration)
                        continue;
                    rows.push_back({0, clip.id, clip.start + m.frame, m.frame, m.name, m.color,
                        m.note, clip.name, track.locked});
                }
            }
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const MarkerRow& a, const MarkerRow& b) {
        if (a.frame != b.frame)
            return a.frame < b.frame;
        return a.clipId == 0 && b.clipId != 0;
    });
    for (size_t i = 0; i < rows.size(); ++i)
        rows[i].number = int(i) + 1;
    return rows;
}

Marker* findMarker(TimelineData& data, quint64 clipId, int64_t storedFrame)
{
    QVector<Marker>* list = &data.markers;
    if (clipId) {
        ClipData* clip = editops::clipById(data, clipId);
        if (!clip)
            return nullptr;
        list = &clip->markers;
    }
    for (Marker& m : *list) {
        if (m.frame == storedFrame)
            return &m;
    }
    return nullptr;
}

} // namespace kk::editindex
