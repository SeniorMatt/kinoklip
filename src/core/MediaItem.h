// SPDX-License-Identifier: GPL-3.0-or-later
// Value types of the media pool: imported media items and the bins (folders) holding them.
#pragma once

#include "core/Subtitles.h"
#include "media/MediaInfo.h"

#include <QString>
#include <QVector>

namespace kk {

/// What a media pool item stands for.
enum class MediaItemKind {
    File, ///< an imported media file (path, probe result)
    Timeline, ///< a timeline of the project (MediaItem::timelineUid)
    CompoundClip, ///< the timeline behind a compound clip (MediaItem::timelineUid)
    /// An imported subtitle file (.srt / .vtt): MediaItem::cues hold its captions, so the
    /// item keeps working when the file is gone. Dropping it on the timeline creates captions.
    Subtitle
};

/// One imported media file, or a timeline / compound clip of the project (Resolve lists both
/// in the media pool). Timeline items have no path; their `info` describes the timeline
/// (duration, resolution, frame rate, video/audio) and is kept in sync by the Project. The probe
/// result is cached so projects open even when the file is offline (moved/deleted).
struct MediaItem
{
    QString id; ///< unique, stable across save/load (referenced by ClipData::mediaId)
    QString path; ///< absolute file path
    QString name; ///< display name (defaults to the file name)
    QString binId; ///< owning Bin::id; empty = the root ("Master") bin
    media::MediaInfo info; ///< result of media::probe() at import time
    MediaItemKind kind = MediaItemKind::File;
    quint64 timelineUid = 0; ///< TimelineData::uid for Timeline / CompoundClip items
    QVector<SubtitleCue> cues; ///< Subtitle items: the parsed captions (file order)

    /// True for timeline and compound clip items.
    [[nodiscard]] bool isTimeline() const
    {
        return kind == MediaItemKind::Timeline || kind == MediaItemKind::CompoundClip;
    }
    /// True for imported subtitle files.
    [[nodiscard]] bool isSubtitle() const { return kind == MediaItemKind::Subtitle; }

    /// Compares every field, including the cached probe result.
    friend bool operator==(const MediaItem& a, const MediaItem& b);
};

/// A folder in the media pool. Bins form a tree via parentId (empty = child of the root bin).
struct Bin
{
    QString id;
    QString name;
    QString parentId;

    friend bool operator==(const Bin&, const Bin&) = default;
};

/// Complete media pool contents; used for undo snapshots and serialization.
struct MediaPoolState
{
    QVector<Bin> bins;
    QVector<MediaItem> items;

    friend bool operator==(const MediaPoolState&, const MediaPoolState&) = default;
};

/// Field-by-field equality of two probe results (media::MediaInfo has no operator==).
[[nodiscard]] bool sameMediaInfo(const media::MediaInfo& a, const media::MediaInfo& b);

} // namespace kk
