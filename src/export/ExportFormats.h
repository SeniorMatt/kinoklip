// SPDX-License-Identifier: GPL-3.0-or-later
// ExportFormats — the export format matrix (docs/archive/plan.md §5.8) as data: containers, video codecs
// and audio codecs with their capabilities. The Deliver page builds its combo boxes from these
// tables and ExportSettings::validate() checks combinations against them.
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace kk {

/// What kind of output a container produces; decides which pipeline ExportJob runs.
enum class OutputKind {
    Movie, ///< video and/or audio streams in one file (mp4, mov, mkv, webm)
    Audio, ///< audio only (wav, flac, mp3, m4a, opus)
    ImageSequence, ///< one image file per frame (png, jpeg, tiff, exr via the image2 muxer)
    Gif, ///< animated GIF (palettegen/paletteuse filter graph)
    Still ///< one frame at the start of the range, written with QImage (png, jpeg)
};

/// One output container ("format" in the Deliver page).
struct ContainerInfo
{
    QString id; ///< stable id stored in presets ("mp4", "mov", "png_seq", ...)
    QString name; ///< UI name ("MP4", "QuickTime", ...)
    QString extension; ///< file suffix without dot
    QString muxer; ///< libavformat muxer name ("mp4", "image2", ...); empty for Still
    OutputKind kind = OutputKind::Movie;
    QStringList videoCodecs; ///< allowed VideoCodecInfo ids, first = default
    QStringList audioCodecs; ///< allowed AudioCodecInfo ids, first = default
    bool faststart = false; ///< write the index (moov) at the front (mp4/mov/m4a)
};

/// One video codec family; the concrete encoder is chosen by EncoderCatalog at run time.
struct VideoCodecInfo
{
    /// "h264", "hevc", "av1", "vp9", "prores", "dnxhr", "png", "mjpeg", "tiff", "exr", "gif"
    QString id;
    QString name; ///< UI name
    QStringList profiles; ///< selectable profile ids (ProRes/DNxHR), first = default
    QStringList profileNames; ///< UI names of `profiles`
    bool hasQuality = false; ///< supports a constant-quality mode (CRF/CQ/QP/qscale)
    bool hasBitrate = false; ///< supports a target bitrate
    int qualityMin = 0; ///< best quality value (lower = better for every codec here)
    int qualityMax = 0; ///< worst quality value
    int qualityDefault = 0;
    bool alphaCapable = false; ///< can carry alpha (ProRes 4444, PNG, TIFF, EXR)
    bool needsEvenSize = true; ///< chroma-subsampled: width/height rounded down to even
};

/// One audio codec.
struct AudioCodecInfo
{
    QString id; ///< "aac", "opus", "flac", "mp3", "pcm_s16le", "pcm_s24le", "pcm_f32le"
    QString name; ///< UI name
    QString encoder; ///< libavcodec encoder name
    bool lossless = false; ///< no bitrate control
    QList<int> sampleRates; ///< allowed rates in Hz, first = default
};

/// All containers in UI order.
[[nodiscard]] const QList<ContainerInfo>& exportContainers();
/// All video codecs.
[[nodiscard]] const QList<VideoCodecInfo>& exportVideoCodecs();
/// All audio codecs.
[[nodiscard]] const QList<AudioCodecInfo>& exportAudioCodecs();

/// Container by id, or nullptr.
[[nodiscard]] const ContainerInfo* findContainer(const QString& id);
/// Video codec by id, or nullptr.
[[nodiscard]] const VideoCodecInfo* findVideoCodec(const QString& id);
/// Audio codec by id, or nullptr.
[[nodiscard]] const AudioCodecInfo* findAudioCodec(const QString& id);

} // namespace kk
