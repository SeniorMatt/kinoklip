// SPDX-License-Identifier: GPL-3.0-or-later
// ImageFrameList — internal: a video source made of individually decoded images — image
// sequences (one file per frame) and animations decoded by Qt (animated WebP). Also the
// routing decision shared by probe() and VideoDecoder::open().
#pragma once

#include "media/ImageLoader.h"
#include "media/MediaInfo.h"

#include <QImage>
#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

class QImageReader;

namespace kk::media::detail {

/// How a path is opened by probe() and VideoDecoder.
enum class MediaRoute {
    FFmpeg, ///< libavformat/libavcodec (video, audio, animated GIF, unknown extensions)
    Still, ///< ImageLoader (single image)
    Animation, ///< ImageFrameList over a Qt-decoded animation (animated WebP)
    Sequence, ///< ImageFrameList over numbered files (ImageSequence path convention)
};

/// Decides the route for `path` (cheap for non-image extensions: no file access).
/// For Still/Animation `*header` (if given) receives the image header that was read.
/// Image extensions whose header cannot be read fall back to FFmpeg (probe-based acceptance).
MediaRoute routeFor(const QString& path, ImageHeader* header = nullptr);

/// MediaInfo of a still image from its header (defined in MediaProbe.cpp).
MediaInfo stillInfo(const QString& path, const ImageHeader& header);

/// Frame-indexed access to a sequence or animation. Every frame is a full-size premultiplied
/// ARGB32 image. Frame k covers media time [startUs(k), startUs(k + 1)).
///
/// Thread-safety: not thread-safe (owned by one VideoDecoder); the static probes are.
class ImageFrameList
{
public:
    ImageFrameList();
    ~ImageFrameList();
    ImageFrameList(const ImageFrameList&) = delete;
    ImageFrameList& operator=(const ImageFrameList&) = delete;

    /// MediaInfo of a sequence path (directory scan + header of the first file).
    static MediaInfo probeSequence(const QString& sequencePath);
    /// MediaInfo of an animated image whose `header` was read by Qt (frame delays are read
    /// without decoding when the plugin supports it).
    static MediaInfo probeAnimation(const QString& path, const ImageHeader& header);

    bool openSequence(const QString& sequencePath, QString* error);
    bool openAnimation(const QString& path, const ImageHeader& header, QString* error);

    const MediaInfo& info() const { return m_info; }
    int frameCount() const { return m_startsUs.empty() ? 0 : int(m_startsUs.size()) - 1; }
    /// Index of the frame shown at `timeUs`, clamped to [0, frameCount() - 1].
    int indexAt(int64_t timeUs) const;
    int64_t startUs(int index) const { return m_startsUs[size_t(index)]; }
    /// Display duration of frame `index` in µs.
    int64_t durationUs(int index) const;

    /// Decoded frame `index` (cached: asking for the frame shown last is free; within a
    /// sequence gap the previous file's image is reused without decoding).
    /// @return a null image on failure.
    QImage frame(int index);

private:
    QImage sequenceFrame(int index);
    QImage animationFrame(int index);

    MediaInfo m_info;
    std::vector<int64_t> m_startsUs; ///< start of every frame + one past the end (duration)

    // Sequence: frame k shows the largest present number <= first + k.
    bool m_isSequence = false;
    QString m_pattern;
    int m_firstNumber = 0;
    std::vector<int> m_presentNumbers; ///< sorted
    int m_currentNumber = -1;

    // Animation: sequential QImageReader; frames are kept while they fit kAnimationBudget.
    QString m_path;
    std::unique_ptr<QImageReader> m_reader;
    int m_readerNext = 0; ///< index of the frame the next read() returns
    std::vector<QImage> m_frames;
    int64_t m_cachedBytes = 0;

    int m_currentIndex = -1;
    QImage m_current;
};

} // namespace kk::media::detail
