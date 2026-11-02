// SPDX-License-Identifier: GPL-3.0-or-later
// Value types of the media pool: imported media items and the bins (folders) holding them.
#pragma once

#include "core/ClipAttributes.h"
#include "core/Subtitles.h"
#include "media/MediaInfo.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace kk {

/// What a media pool item stands for.
enum class MediaItemKind {
    File, ///< an imported media file (path, probe result)
    Timeline, ///< a timeline of the project (MediaItem::timelineUid)
    CompoundClip, ///< the timeline behind a compound clip (MediaItem::timelineUid)
    MulticamClip, ///< the timeline behind a multicam clip (TimelineData::multicam)
    /// An imported subtitle file (.srt / .vtt): MediaItem::cues hold its captions, so the
    /// item keeps working when the file is gone. Dropping it on the timeline creates captions.
    Subtitle
};

/// User metadata of a media pool item (Metadata panel; searched, sortable list columns, smart
/// bin fields). Every field is optional; the defaults mean "not set".
struct MediaMetadata
{
    QString description;
    QString comments;
    QStringList keywords; ///< in the user's order, no duplicates (case-insensitive)
    QString scene;
    QString shot;
    QString take;
    QString camera;
    QString angle;
    bool goodTake = false; ///< the "Good Take" flag of the clip
    int rating = 0; ///< 0 = none, 1 - 5 stars

    /// True when nothing is set.
    [[nodiscard]] bool isEmpty() const { return *this == MediaMetadata {}; }

    friend bool operator==(const MediaMetadata&, const MediaMetadata&) = default;
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
    /// Clip colour (core/ClipColors.h palette id; empty = none). Timeline clips without their
    /// own colour (ClipData::label) show it.
    QString clipColor;
    /// Flags set on the item: bit i = flagPalette()[i]. Shared by every timeline clip of it.
    quint32 flags = 0;
    MediaMetadata metadata; ///< user metadata (Metadata panel)
    /// When the item was added to the pool (ms since the epoch, UTC); 0 = unknown.
    qint64 addedMs = 0;
    /// Modification time of the file when it was imported / relinked (ms since the epoch,
    /// UTC); 0 = unknown or no file.
    qint64 fileModifiedMs = 0;
    /// File items: how the file is interpreted (Clip Attributes dialog); mirrored into the
    /// ClipData::attributes of its clips by Project::syncDerived().
    ClipAttributes attributes;

    /// True for timeline and compound clip items.
    [[nodiscard]] bool isTimeline() const
    {
        return kind == MediaItemKind::Timeline || kind == MediaItemKind::CompoundClip
            || kind == MediaItemKind::MulticamClip;
    }
    /// True for imported subtitle files.
    [[nodiscard]] bool isSubtitle() const { return kind == MediaItemKind::Subtitle; }

    /// Compares every field, including the cached probe result.
    friend bool operator==(const MediaItem& a, const MediaItem& b);
};

/// A folder in the media pool. Bins form a tree via parentId (empty = child of the root bin);
/// siblings are ordered by their position in MediaPoolState::bins.
struct Bin
{
    QString id;
    QString name;
    QString parentId;

    friend bool operator==(const Bin&, const Bin&) = default;
};

/// What a smart bin rule looks at (core/SmartBins.h evaluates them).
enum class SmartField {
    Name, ///< display name (text)
    FileName, ///< file name of the media (text)
    Path, ///< full path (text)
    Type, ///< video / audio / still / timeline / compound / multicam / subtitle (choice)
    Codec, ///< video codec, else audio codec (text)
    Resolution, ///< "1920x1080": is / is not, larger / smaller (pixel count)
    FrameRate, ///< frames per second (number)
    Duration, ///< seconds (number)
    DateAdded, ///< MediaItem::addedMs (date)
    DateModified, ///< MediaItem::fileModifiedMs (date)
    ClipColor, ///< palette id or "none" (choice)
    Flags, ///< flag id, or "any" (has / has not)
    Keywords, ///< keyword list (has / has not exact keyword, contains / does not contain)
    Comments, ///< comments (text)
    Description, ///< description (text)
    Usage ///< "used" / "unused" in the project's timelines (choice)
};

/// Comparison of a smart bin rule. Which operators a field offers: smartbins::operatorsFor().
enum class SmartOp {
    Contains,
    NotContains,
    Is,
    IsNot,
    StartsWith,
    EndsWith,
    Greater, ///< numbers, resolutions (pixel count)
    Less,
    Before, ///< dates: strictly before the day of the value
    After, ///< dates: strictly after the day of the value
    InLast, ///< dates: within the last N days (value = N)
    Has, ///< flags, keywords
    HasNot
};

/// One rule: field, operator, value (text; numbers in C locale, dates "yyyy-MM-dd").
struct SmartRule
{
    SmartField field = SmartField::Name;
    SmartOp op = SmartOp::Contains;
    QString value;

    friend bool operator==(const SmartRule&, const SmartRule&) = default;
};

/// A rule-based bin: shows every item of the media pool that matches all (or any) rules.
/// Live: the set is recomputed whenever the pool or the timelines change.
struct SmartBin
{
    QString id;
    QString name;
    bool matchAll = true; ///< all rules must match (false: any rule)
    QVector<SmartRule> rules;

    friend bool operator==(const SmartBin&, const SmartBin&) = default;
};

/// Complete media pool contents; used for undo snapshots and serialization.
struct MediaPoolState
{
    QVector<Bin> bins;
    QVector<MediaItem> items;
    QVector<SmartBin> smartBins; ///< user smart bins, in list order

    friend bool operator==(const MediaPoolState&, const MediaPoolState&) = default;
};

/// Field-by-field equality of two probe results (media::MediaInfo has no operator==).
[[nodiscard]] bool sameMediaInfo(const media::MediaInfo& a, const media::MediaInfo& b);

} // namespace kk
