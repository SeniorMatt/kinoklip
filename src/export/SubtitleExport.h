// SPDX-License-Identifier: GPL-3.0-or-later
// SubtitleExport — subtitles in a render job: the Deliver options (burn in / separate file /
// embedded stream), collecting the captions of the rendered range, sidecar files next to the
// render and the text-subtitle encoder for embedded streams (mov_text, subrip, ass, webvtt).
#pragma once

#include "core/Subtitles.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <functional>
#include <memory>

struct AVCodecContext;
struct AVPacket;

namespace kk {

/// How subtitles leave Deliver (Resolve's "Export Subtitle" options in the File tab).
enum class SubtitleExportMode {
    BurnIn, ///< drawn into the picture by the compositor
    Sidecar, ///< a separate .srt / .vtt file next to the render
    Embedded ///< a subtitle stream in the movie (MP4/MOV: mov_text, MKV: subrip or ass, WebM:
             ///< webvtt)
};

/// Subtitle part of ExportSettings.
struct SubtitleExportSettings
{
    bool enabled = false; ///< off: no subtitles in any form (Resolve's default)
    SubtitleExportMode mode = SubtitleExportMode::Sidecar;
    SubtitleFormat sidecarFormat = SubtitleFormat::Srt;
    /// Embedded stream codec ("mov_text", "subrip", "ass", "webvtt"); empty = the
    /// container's default (subtitleCodecs(container).front()).
    QString embeddedCodec;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static SubtitleExportSettings fromJson(
        const QJsonObject& json, const SubtitleExportSettings& base);

    friend bool operator==(const SubtitleExportSettings&, const SubtitleExportSettings&) = default;
};

/// Stable ids "burn_in" / "sidecar" / "embedded".
[[nodiscard]] QString subtitleModeId(SubtitleExportMode mode);
[[nodiscard]] SubtitleExportMode subtitleModeFromId(const QString& id);

/// Subtitle stream codecs container @p containerId can carry, default first (empty = none:
/// image sequences, audio-only and stills).
[[nodiscard]] QStringList subtitleCodecs(const QString& containerId);
/// UI name of a subtitle codec id ("mov_text" -> "MP4 Timed Text", ...).
[[nodiscard]] QString subtitleCodecName(const QString& codec);

/// Language code to store in container @p containerId: QuickTime (.mov) only knows the
/// ISO 639-2/B codes ("ger", "fre", ...), so the /T codes ("deu", "fra", ...) are mapped there;
/// other containers keep @p language.
[[nodiscard]] QString containerLanguage(const QString& language, const QString& containerId);

/// The captions of one enabled subtitle track inside the render range (cues relative to the
/// first rendered frame).
struct SubtitleTrackExport
{
    QString name; ///< track display name ("ST1" or the user's name)
    QString language; ///< ISO 639 code, may be empty
    QVector<SubtitleCue> cues;
};

/// Every enabled subtitle track of @p timeline with at least one caption in
/// [@p first, @p end), lowest track first.
[[nodiscard]] QVector<SubtitleTrackExport> collectSubtitleTracks(
    const TimelineData& timeline, int64_t first, int64_t end);

/// Sidecar path for track @p index of @p count next to render @p outputPath: "film.srt", or
/// "film.<language>.srt" / "film.<n>.srt" when several tracks are written.
[[nodiscard]] QString sidecarPath(const QString& outputPath, const SubtitleTrackExport& track,
    int index, int count, SubtitleFormat format);

/// Writes one sidecar file per track next to @p outputPath. Appends the written paths to
/// @p files. False with @p error on the first I/O failure.
[[nodiscard]] bool writeSidecars(const QString& outputPath,
    const QVector<SubtitleTrackExport>& tracks, SubtitleFormat format, QStringList* files,
    QString* error);

namespace exporter {

/// Encoder of one text subtitle stream. open() → context() for MediaWriter::addStream() →
/// after the header: encode() every cue (packets in ms time base).
/// Thread-safety: one thread.
class SubtitleEncoder
{
public:
    SubtitleEncoder();
    ~SubtitleEncoder();
    SubtitleEncoder(const SubtitleEncoder&) = delete;
    SubtitleEncoder& operator=(const SubtitleEncoder&) = delete;

    /// Opens encoder @p codec ("mov_text", "subrip", "ass", "webvtt") for a picture of
    /// @p width x @p height (ASS play resolution). False with @p error if FFmpeg lacks it.
    [[nodiscard]] bool open(
        const QString& codec, int width, int height, bool globalHeader, QString* error);
    [[nodiscard]] AVCodecContext* context() const;
    /// Encodes @p cue and hands the packet (pts/duration in the encoder time base, 1/1000) to
    /// @p sink. False with @p error on failure.
    [[nodiscard]] bool encode(
        const SubtitleCue& cue, const std::function<bool(AVPacket*)>& sink, QString* error);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace exporter
} // namespace kk
