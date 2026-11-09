// SPDX-License-Identifier: GPL-3.0-or-later
// Otio — the timeline as an OpenTimelineIO (.otio) file and back (File > Export / Import, spec
// 51.1 - 51.4). A .otio file is plain JSON with a public schema, so this is a reader and a
// writer on QJsonDocument; no OpenTimelineIO library is used.
#pragma once

#include "core/TimelineData.h"

#include <QByteArray>
#include <QColor>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>

namespace kk {
class MediaPool;
}

namespace kk::otio {

/// Options of timelineToOtio().
struct ExportOptions
{
    /// Source time (µs) that shows as the media's first frame in OTIO time, e.g. the media's
    /// start timecode (ui/common/SourceTimecode.h), like EdlOptions::sourceStartUs. Empty = 0.
    std::function<int64_t(const ClipData&)> sourceStartUs;
    /// Write Clip.1 (one "media_reference") instead of Clip.2 ("media_references" +
    /// "active_media_reference_key"), for readers older than OpenTimelineIO 0.15.
    bool legacyClipSchema = false;
};

/// @p data as a .otio file (UTF-8 JSON, indented):
///
/// * Timeline.1 with global_start_time = the starting timecode, one Stack.1 ("tracks") of
///   Track.1 (kind "Video" / "Audio", V1 / A1 first; subtitle tracks are not written);
/// * every clip a Clip.2 with source_range (source time, then length on the track), an
///   ExternalReference.1 (target_url "file:///...", available_range = the whole media) or a
///   MissingReference.1 (titles, solids, generators, compound clips, media gone from the pool),
///   enabled, and its clip markers as Marker.2;
/// * a gap between clips a Gap.1; a transition between two touching clips a Transition.1
///   (SMPTE_Dissolve, in_offset / out_offset = the frames before / after the cut; other
///   transition types are written as Custom_Transition with the Kinoklip type in metadata);
///   fades from / to nothing are not written;
/// * a clip at a constant speed other than 100 % a LinearTimeWarp.1 (time_scalar = speed, 0 =
///   FreezeFrame.1) in its effects; source_range.duration stays the length on the track.
///   Ramped clips are written at their average speed;
/// * timeline markers as Marker.2 on the Stack ("tracks"), record timecode in marked_range.
///
/// Every time is a RationalTime.1 at the timeline's rate; values may be fractions of a frame
/// (source times are microseconds), so a round trip keeps the source times exactly. @p pool
/// (may be null) supplies the media paths.
[[nodiscard]] QByteArray timelineToOtio(
    const TimelineData& data, const MediaPool* pool, const ExportOptions& options = {});

/// Media a clip of an imported file refers to; resolveMedia() turns it into a media pool item.
struct MediaRef
{
    /// Absolute local path (target_url decoded, a relative one resolved against
    /// ImportOptions::baseDir). Empty for a clip without a file (MissingReference, no reference).
    QString path;
    QString name; ///< the clip's name, for an item without a path
    bool hasVideo = true; ///< false when only audio tracks use it
    bool hasAudio = false; ///< true when an audio track uses it
    /// Length of the media in µs: the reference's available_range, else the furthest source
    /// time any clip uses. 0 = unknown.
    int64_t durationUs = 0;
    Rational frameRate; ///< rate of the available_range / source_range (informational)
};

/// What resolveMedia() answers.
struct ResolvedMedia
{
    QString mediaId; ///< MediaItem::id of the pool item; empty = leave the clip out
    int64_t durationUs = 0; ///< ClipData::sourceDurationUs of its clips (0 = unbounded)
};

/// Options of importOtio().
struct ImportOptions
{
    /// Folder relative target_urls resolve against (the .otio file's folder).
    QString baseDir;
    /// Name of the timeline when the file gives none.
    QString fallbackName;
    /// Frame rate when the file gives no usable rate.
    Rational defaultFps {24, 1};
    int width = 1920; ///< picture size of the new timeline (a .otio file carries none)
    int height = 1080;
    /// Finds or creates the media pool item of a referenced file (called once per distinct
    /// path). Without it every clip stays without media and the import fails.
    std::function<ResolvedMedia(const MediaRef&)> resolveMedia;
};

/// Outcome of importOtio().
struct TimelineImport
{
    bool ok = false; ///< false: not a usable .otio timeline (see error)
    QString error;
    TimelineData timeline; ///< tracks and clips placed with editops; uid 0, no media of its own
    /// What the file contained that Kinoklip left out, one line per kind with a count:
    /// "Effect.1 (3 times)", "metadata of Clip.2", "transition type SMPTE_Wipe imported as ...".
    QStringList skipped;
    int clipCount = 0; ///< clips placed
};

/// Reads a .otio file: Timeline.1 (the first one of a SerializableCollection.1) with Stack.1,
/// Track.1 (kind Video / Audio), Clip (Clip.1 media_reference, Clip.2 media_references with
/// active_media_reference_key), Gap.1, Transition.1, ExternalReference.1, MissingReference.1,
/// RationalTime.1, TimeRange.1, Marker.1 / Marker.2 (on clips and on the Stack) and
/// LinearTimeWarp.1 / FreezeFrame.1 (clip speed). Any version number of a known schema is
/// accepted. Everything else (other effects, nested stacks, unknown schemas, metadata,
/// image sequence and generator references) is skipped and listed in TimelineImport::skipped.
/// The timeline rate comes from global_start_time, else the gaps, else the clips.
[[nodiscard]] TimelineImport importOtio(const QByteArray& json, const ImportOptions& options);

/// The frame rate @p rate (a RationalTime rate) stands for: NTSC rates such as 29.97 or
/// 23.976 become 30000/1001 and 24000/1001 exactly, other rates the nearest 1/1000.
[[nodiscard]] Rational rateToRational(double rate);

/// OTIO marker colour name ("RED", "MAGENTA", ...) closest to @p color.
[[nodiscard]] QString markerColorToOtio(const QColor& color);
/// Colour of the OTIO marker colour name @p name (case-insensitive); invalid for an unknown name.
[[nodiscard]] QColor markerColorFromOtio(const QString& name);

} // namespace kk::otio
