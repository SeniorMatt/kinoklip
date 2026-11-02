// SPDX-License-Identifier: GPL-3.0-or-later
// Subtitles — captions as data: SRT / WebVTT parsing and writing, the subtitle style keys and
// their defaults, and conversions between cue lists and subtitle-track clips.
#pragma once

#include "core/TimelineData.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <cstdint>

namespace kk {

/// One caption of a subtitle file: [startUs, endUs) and its text (lines separated by '\n',
/// plain text: markup is removed on import).
struct SubtitleCue
{
    int64_t startUs = 0;
    int64_t endUs = 0;
    QString text;

    friend bool operator==(const SubtitleCue&, const SubtitleCue&) = default;
};

/// Text subtitle file formats we read and write.
enum class SubtitleFormat {
    Srt, ///< SubRip (.srt): "HH:MM:SS,mmm --> HH:MM:SS,mmm"
    WebVtt ///< WebVTT (.vtt): "WEBVTT" header, "HH:MM:SS.mmm --> ..."
};

namespace subtitles {

// ---- Style keys (TrackData::subtitleStyle = the track default; ClipData::title of a caption
//      holds keys::Text plus any key it overrides). The title keys of core/ParamKeys.h are
//      reused for font, size, colours, outline, shadow and background box; these add the
//      placement. Constants behave like enum values, hence no k prefix. ----
/// static choice "bottom" | "top" | "middle"; default "bottom"
inline const QString Position = QStringLiteral("subPosition");
/// animated percent of the frame height between the text block and the top/bottom edge;
/// default kDefaultMarginV
inline const QString MarginV = QStringLiteral("subMarginV");
/// animated percent of the frame width kept free left and right (left/right alignment and
/// line wrapping); default kDefaultMarginH
inline const QString MarginH = QStringLiteral("subMarginH");

/// Caption only: static bool. Off (default): the caption uses its track's style; on: its own
/// style keys (a full copy made when it is switched on) override the track's.
inline const QString OwnStyle = QStringLiteral("ownStyle");
/// Descriptor ids of the inspector groups (category ClipProperties, storage "title"): the
/// caption text + OwnStyle, and the style parameters (caption or track style).
inline const QString CaptionGroup = QStringLiteral("builtin.caption");
inline const QString StyleGroup = QStringLiteral("builtin.subtitle_style");

inline constexpr double kDefaultSize = 52.0; ///< font px at 1080 lines
inline constexpr double kDefaultMarginV = 7.0; ///< percent of the frame height
inline constexpr double kDefaultMarginH = 8.0; ///< percent of the frame width
inline constexpr double kDefaultOutlineWidth = 2.5; ///< px
inline constexpr double kDefaultBgOpacity = 60.0; ///< percent (box off by default)
/// Shortest caption a cue becomes on the timeline (frames).
inline constexpr int64_t kMinCaptionFrames = 1;

/// The built-in default caption style (white, black outline, bottom centre). A new subtitle
/// track starts with it; keys missing from a track's style fall back to it.
[[nodiscard]] const ParamSet& defaultStyle();

/// Style value lookup with the caption (only with OwnStyle on) → track → default chain
/// (animated keys at the caption-local frame @p localFrame). Allocation-free.
[[nodiscard]] double styleValue(
    const ClipData& caption, const TrackData* track, const QString& key, int64_t localFrame);
/// Static style lookup with the caption (OwnStyle on) → track → default chain.
[[nodiscard]] QVariant styleStatic(
    const ClipData& caption, const TrackData* track, const QString& key);

/// True if caption @p caption overrides its track's style (OwnStyle).
[[nodiscard]] bool hasOwnStyle(const ClipData& caption);
/// Text of a caption clip (keys::Text, empty when unset).
[[nodiscard]] QString captionText(const ClipData& caption);
/// A new caption clip (no id yet) showing @p text over [startFrame, startFrame + frames).
[[nodiscard]] ClipData makeCaption(const QString& text, int64_t startFrame, int64_t frames);
/// A new subtitle track (no id yet) with the default style.
[[nodiscard]] TrackData makeSubtitleTrack();

// ---- Files ----

/// Parses SRT or WebVTT data (format detected from the "WEBVTT" header; a UTF-8 BOM, CRLF and
/// both ',' and '.' millisecond separators are accepted). Cues are returned in file order;
/// markup (<i>, <b>, <font>, <c.x>, {\an8}) is removed, HTML entities of WebVTT decoded.
/// Returns false with @p error when nothing could be parsed.
[[nodiscard]] bool parse(const QByteArray& data, QVector<SubtitleCue>* cues,
    QString* error = nullptr, SubtitleFormat* detected = nullptr);
/// Reads and parses a subtitle file. False with @p error when it cannot be read / parsed.
[[nodiscard]] bool readFile(const QString& path, QVector<SubtitleCue>* cues, QString* error);
/// Encodes @p cues (UTF-8, LF line endings, numbered from 1 for SRT).
[[nodiscard]] QByteArray write(const QVector<SubtitleCue>& cues, SubtitleFormat format);
/// Writes write() to @p path atomically. False with @p error on I/O failure.
[[nodiscard]] bool writeFile(
    const QString& path, const QVector<SubtitleCue>& cues, SubtitleFormat format, QString* error);
/// "HH:MM:SS,mmm" (SRT) or "HH:MM:SS.mmm" (WebVTT) for @p us (rounded to milliseconds).
[[nodiscard]] QString formatTimestamp(int64_t us, SubtitleFormat format);
/// File suffix without dot: "srt" / "vtt".
[[nodiscard]] QString suffix(SubtitleFormat format);
/// True if @p path has a subtitle suffix we import (.srt, .vtt).
[[nodiscard]] bool isSubtitleFile(const QString& path);

// ---- Timeline conversion ----

/// Caption clips of @p track that overlap [@p rangeStart, @p rangeEnd) as cues, timed
/// relative to rangeStart and clipped to the range (frames → µs at @p fps). Disabled clips
/// and empty texts are skipped. Sorted by start.
[[nodiscard]] QVector<SubtitleCue> cuesFromTrack(
    const TrackData& track, Rational fps, int64_t rangeStart, int64_t rangeEnd);

/// Caption clips (no ids) for @p cues placed with their zero at timeline frame @p startFrame
/// (µs → nearest frame at @p fps, at least kMinCaptionFrames long). Overlapping cues cannot
/// share a track, so each clip gets a lane: lane 0 is the first subtitle track, later lanes
/// hold the cues that overlap an earlier one (greedy, first fitting lane).
struct PlacedCaption
{
    int lane = 0;
    ClipData clip;
};
[[nodiscard]] QVector<PlacedCaption> captionsFromCues(
    const QVector<SubtitleCue>& cues, Rational fps, int64_t startFrame);

/// End of the last cue in µs (0 for none).
[[nodiscard]] int64_t cuesDurationUs(const QVector<SubtitleCue>& cues);

} // namespace subtitles
} // namespace kk
