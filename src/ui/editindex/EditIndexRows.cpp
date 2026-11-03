// SPDX-License-Identifier: GPL-3.0-or-later
// EditIndexRows — events and markers of a timeline for the Edit Index.
#include "ui/editindex/EditIndexRows.h"

#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/MediaPool.h"
#include "ui/common/SourceTimecode.h"
#include "ui/common/TimecodeFormat.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QHash>
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

/// What the rows of every clip of one media item share (looked up once per item: a long
/// timeline has many clips of few files).
struct MediaFacts
{
    QString reel;
    SourceTimecodeBase base;
    bool offline = false;
};

QString notesOf(const ClipData& clip, const MediaFacts* media)
{
    if (clip.enabled && !(media && media->offline) && !clip.transitionIn && !clip.transitionOut
        && !clip.retime.isAnimated() && std::abs(clip.speed - 1.0) <= 1e-9)
        return {}; // the common case: nothing to say, no string building
    QStringList notes;
    if (!clip.enabled)
        notes << tr("Disabled");
    if (media && media->offline)
        notes << tr("Offline");
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
    size_t total = 0;
    for (TrackType type : {TrackType::Video, TrackType::Audio, TrackType::Subtitle}) {
        for (const TrackData& track : data.tracks(type))
            total += track.clips.size();
    }
    rows.reserve(total);
    QHash<QString, MediaFacts> media; // by MediaItem::id
    for (TrackType type : {TrackType::Video, TrackType::Audio, TrackType::Subtitle}) {
        const auto& tracks = data.tracks(type);
        for (int t = 0; t < int(tracks.size()); ++t) {
            const QString trackName = trackDisplayName(tracks[size_t(t)], t);
            for (const ClipData& clip : tracks[size_t(t)].clips) {
                EventRow row;
                row.clipId = clip.id;
                row.track = {type, t};
                row.trackName = trackName;
                row.name = clip.name;
                const MediaFacts* facts = nullptr;
                if (pool && clip.kind == ClipKind::Media) {
                    auto it = media.find(clip.mediaId);
                    if (it == media.end()) {
                        MediaFacts f;
                        const MediaItem* item = pool->item(clip.mediaId);
                        if (item) {
                            f.reel = QFileInfo(item->path).completeBaseName();
                            if (f.reel.isEmpty())
                                f.reel = item->name;
                        }
                        f.base = sourceTimecodeBase(clip, pool, data.fps);
                        f.offline = !item || pool->isOffline(clip.mediaId);
                        it = media.insert(clip.mediaId, f);
                    }
                    facts = &it.value();
                }
                const SourceTimecodeBase base
                    = facts ? facts->base : sourceTimecodeBase(clip, pool, data.fps);
                row.reel = facts ? facts->reel : kindName(clip.kind);
                row.recordIn = clip.start;
                row.recordOut = clip.end();
                auto sourceTc = [&](int64_t frame) {
                    const int64_t us = base.startUs + clip.sourceTimeUsAt(frame, data.fps);
                    return timecode::format(usToFrames(us, base.rate), base.rate);
                };
                row.sourceIn = sourceTc(clip.start);
                row.sourceOut = sourceTc(clip.end());
                row.notes = notesOf(clip, facts);
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
