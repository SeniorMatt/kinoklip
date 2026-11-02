// SPDX-License-Identifier: GPL-3.0-or-later
// ExportSettings — everything one render job needs to know besides the timeline: container,
// video/audio codec settings, output size, render range and output file. Plain value type with
// a JSON form (presets, render queue persistence).
#pragma once

#include "export/ExportFormats.h"

#include <QJsonObject>
#include <QSize>
#include <QString>

#include <cstdint>

namespace kk {

struct TimelineData;

/// Which encoder implementation to prefer for codecs that have hardware encoders.
enum class EncoderBackend {
    Auto, ///< best available: NVENC → VAAPI → QSV → software (see EncoderCatalog)
    Software, ///< libx264 / libx265 / libsvtav1 / libvpx-vp9
    Nvenc, ///< NVIDIA NVENC
    Vaapi, ///< VAAPI on an Intel/AMD render node
    Qsv ///< Intel Quick Sync (oneVPL / libmfx)
};

/// Stable id of a backend ("auto", "software", "nvenc", "vaapi", "qsv").
[[nodiscard]] QString encoderBackendId(EncoderBackend backend);
/// Parses encoderBackendId(); unknown → Auto.
[[nodiscard]] EncoderBackend encoderBackendFromId(const QString& id);
/// UI name ("Auto", "Software", "NVIDIA NVENC", ...).
[[nodiscard]] QString encoderBackendName(EncoderBackend backend);

/// Constant quality (CRF / CQ / QP / qscale) or target average bitrate.
enum class RateControl {
    Quality,
    Bitrate
};

/// Which frames to render.
enum class RangeMode {
    Entire, ///< frame 0 to the end of the last clip
    InOut, ///< the timeline's mark in / mark out (falls back to Entire edges if unset)
    Custom ///< ExportSettings::rangeIn / rangeOut
};

/// Video stream settings.
struct VideoExportSettings
{
    QString codec = QStringLiteral("h264"); ///< VideoCodecInfo id
    EncoderBackend encoder = EncoderBackend::Auto;
    QString profile; ///< VideoCodecInfo::profiles entry (ProRes/DNxHR); empty = default
    RateControl rateControl = RateControl::Quality;
    int quality = 20; ///< in the codec's scale (VideoCodecInfo::qualityMin..Max)
    int bitrateKbps = 16000; ///< average target in kbit/s (RateControl::Bitrate)
    /// Encoder speed/efficiency trade-off: "fast", "medium" or "slow" (mapped per encoder).
    QString speed = QStringLiteral("medium");
    /// Output width in pixels; 0 = timeline width, or proportional when only height is set.
    int width = 0;
    /// Output height in pixels; 0 = timeline height, or proportional when only width is set.
    int height = 0;
    bool alpha = false; ///< keep transparency (only for alpha-capable codecs/profiles)
    /// Distance between key frames in frames; 0 = automatic (one every 2 seconds).
    int keyframeInterval = 0;
    /// Allow B-frames (frame reordering) in inter-frame codecs. Off: every frame only refers
    /// to earlier ones (some editing and streaming tools want that).
    bool frameReordering = true;

    friend bool operator==(const VideoExportSettings&, const VideoExportSettings&) = default;
};

/// Audio stream settings.
struct AudioExportSettings
{
    bool enabled = true; ///< false = no audio stream
    QString codec = QStringLiteral("aac"); ///< AudioCodecInfo id
    int bitrateKbps = 320; ///< lossy codecs only
    int sampleRate = 48000; ///< Hz (the mixer runs at 48 kHz; other rates are resampled)
    int bitDepth = 16; ///< FLAC only: 16 or 24

    friend bool operator==(const AudioExportSettings&, const AudioExportSettings&) = default;
};

/// Complete settings of one render job.
struct ExportSettings
{
    QString container = QStringLiteral("mp4"); ///< ContainerInfo id
    bool videoEnabled = true; ///< ignored by audio-only containers
    VideoExportSettings video;
    AudioExportSettings audio;
    RangeMode range = RangeMode::Entire;
    int64_t rangeIn = 0; ///< RangeMode::Custom: first frame
    int64_t rangeOut = 0; ///< RangeMode::Custom: exclusive end frame
    /// Output file. Image sequences: a "%0Nd" in the name is the frame number; without one,
    /// "_%05d" is inserted before the extension (see sequencePattern()).
    QString outputPath;

    /// The container descriptor (never null for a validated settings object).
    [[nodiscard]] const ContainerInfo* containerInfo() const { return findContainer(container); }
    /// Output kind of the container (Movie when the container is unknown).
    [[nodiscard]] OutputKind kind() const;
    /// True if a video stream / image output is produced.
    [[nodiscard]] bool hasVideo() const;
    /// True if an audio stream is produced.
    [[nodiscard]] bool hasAudio() const;
    /// True if the output keeps transparency (alpha requested and supported by codec/profile).
    [[nodiscard]] bool keepsAlpha() const;

    /// Output size for @p timeline (custom size or timeline size; rounded down to even for
    /// chroma-subsampled codecs; at least 2x2).
    [[nodiscard]] QSize outputSize(const TimelineData& timeline) const;
    /// Frame range [first, end) for @p timeline. An empty timeline yields an empty range.
    [[nodiscard]] std::pair<int64_t, int64_t> frameRange(const TimelineData& timeline) const;

    /// Checks codec/container combinations and values. @return false with a message.
    [[nodiscard]] bool validate(QString* error) const;
    /// Fixes up a combination after the container changed: picks the container's default
    /// codecs if the current ones are not allowed there, clamps the quality range.
    void normalize();

    /// JSON form (only settings that matter for the container are meaningful).
    [[nodiscard]] QJsonObject toJson() const;
    /// Reads toJson() output; missing keys keep the values of @p base.
    [[nodiscard]] static ExportSettings fromJson(
        const QJsonObject& json, const ExportSettings& base);

    friend bool operator==(const ExportSettings&, const ExportSettings&) = default;
};

/// "/out/shot.png" → "/out/shot_%05d.png"; a path already containing "%" is returned as is.
[[nodiscard]] QString sequencePattern(const QString& path);

/// Replaces (or appends) the extension of @p path with the container's extension.
[[nodiscard]] QString withContainerExtension(const QString& path, const QString& containerId);

} // namespace kk
