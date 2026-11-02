// SPDX-License-Identifier: GPL-3.0-or-later
// ExportTestUtil — shared helpers of the export tests: the small fixture project (two video
// tracks, a title, a dissolve, linked audio), a WAV writer for impulse fixtures, and checks on
// exported files (luma of decoded frames, audio level).
#pragma once

#include "../render/RenderTestUtil.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "media/AudioDecoder.h"
#include "media/MediaInfo.h"
#include "media/VideoDecoder.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <vector>

namespace kk::test {

/// Frames of the fixture project (2 s at 25 fps).
inline constexpr int64_t kProjectFrames = 50;
inline constexpr int kProjectWidth = 320;
inline constexpr int kProjectHeight = 180;

/// Fills @p project with the export fixture timeline (320x180, 25 fps, 50 frames):
/// V1: h264_25fps.mp4 [0, 30) then h264_2997.mp4 [30, 50) with a 10-frame cross dissolve;
/// V2: a title [10, 40); A1: the audio of the first clip over [0, 50) (linked).
/// @return false with @p why when fixtures are missing.
inline bool buildExportProject(Project& project, QString* why)
{
    if (!ensureFixtures(why)) {
        return false;
    }
    const ImportResult imported
        = project.importMedia({fixture("h264_25fps.mp4"), fixture("h264_2997.mp4")});
    if (imported.importedIds.size() != 2) {
        *why = QStringLiteral("cannot import fixtures: %1")
                   .arg(imported.warnings.join(QLatin1Char(' ')));
        return false;
    }
    const EffectRegistry& registry = EffectRegistry::instance();
    project.beginEdit(QStringLiteral("Export fixture"));
    TimelineData& tl = project.timeline()->mutableData();
    tl.width = kProjectWidth;
    tl.height = kProjectHeight;
    tl.fps = {25, 1};
    ClipData a = mediaClip(tl, imported.importedIds[0], 0, 30, 0, 10'000'000);
    a.linkGroup = 1;
    ClipData b = mediaClip(tl, imported.importedIds[1], 30, 20, 1'000'000, 5'000'000);
    b.transitionIn = registry.createTransition(QStringLiteral("cross_dissolve"), 10);
    addClip(tl, TrackType::Video, 0, a);
    addClip(tl, TrackType::Video, 0, b);
    addClip(tl, TrackType::Video, 1, titleClip(tl, QStringLiteral("Kinoklip"), 10, 30, 40));
    ClipData audio = mediaClip(tl, imported.importedIds[0], 0, kProjectFrames, 0, 10'000'000);
    audio.linkGroup = 1;
    addClip(tl, TrackType::Audio, 0, audio);
    project.endEdit();
    return true;
}

/// Writes a 48 kHz stereo 16-bit WAV of @p frames sample frames; `sample(i)` gives the value
/// of sample frame i in [-1, 1] (both channels).
template<typename Fn>
inline bool writeWav(const QString& path, int frames, Fn sample)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    const quint32 dataBytes = quint32(frames) * 4;
    out.writeRawData("RIFF", 4);
    out << quint32(36 + dataBytes);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(2) << quint32(48000) << quint32(48000 * 4)
        << quint16(4) << quint16(16);
    out.writeRawData("data", 4);
    out << dataBytes;
    for (int i = 0; i < frames; ++i) {
        const qint16 v = qint16(std::lround(std::clamp(sample(i), -1.0, 1.0) * 32767.0));
        out << v << v;
    }
    return file.error() == QFile::NoError;
}

/// Mean grey value (0..255) of @p image.
inline double meanLuma(const QImage& image)
{
    if (image.isNull()) {
        return 0;
    }
    double sum = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            sum += qGray(image.pixel(x, y));
        }
    }
    return sum / double(image.width() * image.height());
}

/// Decodes the frame at @p timeUs of @p path (null image on failure).
inline QImage decodeAt(const QString& path, int64_t timeUs)
{
    media::VideoDecoder decoder;
    if (!decoder.open(path)) {
        return {};
    }
    return decoder.frameAt(timeUs);
}

/// RMS level of the first @p frames 48 kHz sample frames of @p path's audio.
inline double audioRms(const QString& path, int frames)
{
    media::AudioDecoder decoder;
    if (!decoder.open(path)) {
        return 0;
    }
    std::vector<float> buffer(size_t(frames) * 2);
    decoder.read(0, buffer.data(), frames);
    double sum = 0;
    for (float v : buffer) {
        sum += double(v) * v;
    }
    return std::sqrt(sum / double(buffer.size()));
}

} // namespace kk::test
