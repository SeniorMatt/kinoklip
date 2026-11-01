// SPDX-License-Identifier: GPL-3.0-or-later
// ImageFrameList — image sequences and Qt-decoded animations as frame-indexed video sources,
// and routeFor(), the probe/decoder routing decision.
#include "media/ImageFrameList.h"

#include "media/ImageBackends.h"
#include "media/ImageSequence.h"
#include "media/MediaLog.h"

#include <QFileInfo>
#include <QImageReader>

#include <algorithm>
#include <numeric>

namespace kk::media::detail {

namespace {

/// Browsers (and therefore authors) treat GIF/WebP frame delays of 10 ms or less as 100 ms.
constexpr int kMinFrameDelayMs = 11;
constexpr int kDefaultFrameDelayMs = 100;

/// Decoded animation frames are kept in memory up to this many bytes; longer/bigger
/// animations re-decode from the start when stepping backwards.
constexpr int64_t kAnimationBudgetBytes = 128LL * 1024 * 1024;

/// ceil(index * den * 1e6 / num): first microsecond of frame `index` at `rate`.
int64_t frameStartUs(int64_t index, Rational rate)
{
    const int64_t numerator = index * rate.den * 1'000'000;
    return (numerator + rate.num - 1) / rate.num;
}

/// Frame delays (ms) of an animation, read with the image plugin's frame iteration.
std::vector<int> readFrameDelays(const QString& path, int frameCount)
{
    QImageReader reader(path);
    std::vector<int> delays;
    const int count = frameCount > 0 ? frameCount : reader.imageCount();
    delays.reserve(size_t(std::max(count, 1)));
    for (int i = 0; i < count; ++i) {
        // nextImageDelay() is the delay of the frame the reader is positioned on. Plugins that
        // can skip frames without decoding (WebP: WebPDemux iterator) answer jumpToNextImage();
        // others need a real read().
        const int delay = reader.nextImageDelay();
        delays.push_back(delay >= kMinFrameDelayMs ? delay : kDefaultFrameDelayMs);
        if (i + 1 < count && !reader.jumpToNextImage() && reader.read().isNull()) {
            break;
        }
    }
    return delays;
}

/// Nominal frame rate for MediaInfo: exact when every delay is equal, else the average.
Rational rateFromDelays(const std::vector<int>& delays)
{
    if (delays.empty()) {
        return {0, 1};
    }
    const int64_t total = std::accumulate(delays.begin(), delays.end(), int64_t(0));
    const bool uniform
        = std::all_of(delays.begin(), delays.end(), [&](int d) { return d == delays.front(); });
    int64_t num = uniform ? 1000 : 1000 * int64_t(delays.size());
    int64_t den = uniform ? delays.front() : total;
    const int64_t g = std::gcd(num, den);
    return {int(num / g), int(den / g)};
}

/// Common MediaInfo fields of a frame-list source.
void fillVideoInfo(MediaInfo& info, const ImageHeader& header)
{
    info.valid = info.hasVideo = true;
    info.isStillImage = false;
    info.width = info.displayWidth = header.size.width();
    info.height = info.displayHeight = header.size.height();
    info.hasAlpha = header.hasAlpha;
    info.videoCodec = header.format;
    info.pixelFormat = header.pixelFormat;
}

} // namespace

MediaRoute routeFor(const QString& path, ImageHeader* header)
{
    if (ImageSequence::isSequencePath(path)) {
        return MediaRoute::Sequence;
    }
    if (!ImageLoader::handlesExtension(path)) {
        return MediaRoute::FFmpeg;
    }
    ImageHeader local;
    ImageHeader& h = header ? *header : local;
    h = ImageLoader::readHeader(path);
    if (!h.valid) {
        return MediaRoute::FFmpeg; // maybe a mislabelled video: let libavformat try
    }
    if (h.animated) {
        // Animated GIF: FFmpeg's gif demuxer gives exact timestamps and seeking. Animated
        // WebP: FFmpeg 6.1 cannot decode it, Qt's plugin can.
        return h.backend == ImageBackend::Qt && h.format != QLatin1String("gif")
            ? MediaRoute::Animation
            : MediaRoute::FFmpeg;
    }
    return MediaRoute::Still;
}

ImageFrameList::ImageFrameList() = default;
ImageFrameList::~ImageFrameList() = default;

// ---- Probing --------------------------------------------------------------------------------

MediaInfo ImageFrameList::probeSequence(const QString& sequencePath)
{
    MediaInfo info;
    info.path = sequencePath;
    QString pattern;
    Rational rate;
    if (!ImageSequence::parsePath(sequencePath, &pattern, &rate)) {
        info.error = QStringLiteral("Not an image sequence path: %1").arg(sequencePath);
        return info;
    }
    const SequenceInfo sequence = ImageSequence::scan(pattern);
    if (sequence.fileCount <= 0) {
        info.error = QStringLiteral("No files match the image sequence %1").arg(pattern);
        return info;
    }
    const QString first = ImageSequence::fileForNumber(pattern, sequence.firstNumber);
    const ImageHeader header = ImageLoader::readHeader(first);
    if (!header.valid) {
        info.error = QStringLiteral("Cannot read the first image of the sequence (%1): %2")
                         .arg(first, header.error);
        return info;
    }
    fillVideoInfo(info, header);
    info.isImageSequence = true;
    info.container = QStringLiteral("sequence");
    info.frameRate = rate;
    info.durationUs = frameStartUs(sequence.frameCount(), rate);
    info.sequenceFirstNumber = sequence.firstNumber;
    info.sequenceLastNumber = sequence.lastNumber;
    info.sequenceMissingFrames = int(sequence.missing.size());
    if (!sequence.missing.isEmpty()) {
        info.error = QStringLiteral("%1 missing frame(s) in the sequence, first missing number %2")
                         .arg(sequence.missing.size())
                         .arg(sequence.missing.front());
    }
    return info;
}

namespace {

MediaInfo animationInfo(
    const QString& path, const ImageHeader& header, const std::vector<int>& delays)
{
    MediaInfo info;
    info.path = path;
    fillVideoInfo(info, header);
    info.container = header.format;
    info.frameRate = rateFromDelays(delays);
    info.durationUs = 1000 * std::accumulate(delays.begin(), delays.end(), int64_t(0));
    return info;
}

} // namespace

MediaInfo ImageFrameList::probeAnimation(const QString& path, const ImageHeader& header)
{
    return animationInfo(path, header, readFrameDelays(path, header.frameCount));
}

// ---- Opening --------------------------------------------------------------------------------

bool ImageFrameList::openSequence(const QString& sequencePath, QString* error)
{
    m_info = probeSequence(sequencePath);
    if (!m_info.valid) {
        if (error) {
            *error = m_info.error;
        }
        return false;
    }
    Rational rate;
    ImageSequence::parsePath(sequencePath, &m_pattern, &rate);
    const SequenceInfo sequence = ImageSequence::scan(m_pattern);
    m_isSequence = true;
    m_firstNumber = sequence.firstNumber;
    // Present numbers = the full range minus the (sorted) gaps.
    m_presentNumbers.clear();
    m_presentNumbers.reserve(size_t(sequence.fileCount));
    auto gap = sequence.missing.cbegin();
    for (int n = sequence.firstNumber; n <= sequence.lastNumber; ++n) {
        if (gap != sequence.missing.cend() && *gap == n) {
            ++gap;
        } else {
            m_presentNumbers.push_back(n);
        }
    }
    const int count = sequence.frameCount();
    m_startsUs.resize(size_t(count) + 1);
    for (int k = 0; k <= count; ++k) {
        m_startsUs[size_t(k)] = frameStartUs(k, rate);
    }
    return true;
}

bool ImageFrameList::openAnimation(const QString& path, const ImageHeader& header, QString* error)
{
    const std::vector<int> delays = readFrameDelays(path, header.frameCount);
    m_info = animationInfo(path, header, delays);
    if (delays.empty()) {
        if (error) {
            *error = QStringLiteral("Animation has no frames");
        }
        return false;
    }
    m_isSequence = false;
    m_path = path;
    m_startsUs.resize(delays.size() + 1);
    m_startsUs[0] = 0;
    for (size_t i = 0; i < delays.size(); ++i) {
        m_startsUs[i + 1] = m_startsUs[i] + 1000 * int64_t(delays[i]);
    }
    m_frames.assign(delays.size(), QImage());
    return true;
}

// ---- Frame access ---------------------------------------------------------------------------

int ImageFrameList::indexAt(int64_t timeUs) const
{
    // Last frame whose start is <= timeUs (binary search over frame starts).
    const auto end = m_startsUs.end() - 1; // the last entry is the end of the last frame
    const auto it = std::upper_bound(m_startsUs.begin(), end, timeUs);
    return std::clamp(int(it - m_startsUs.begin()) - 1, 0, frameCount() - 1);
}

int64_t ImageFrameList::durationUs(int index) const
{
    return m_startsUs[size_t(index) + 1] - m_startsUs[size_t(index)];
}

QImage ImageFrameList::frame(int index)
{
    if (index < 0 || index >= frameCount()) {
        return {};
    }
    if (index == m_currentIndex) {
        return m_current;
    }
    QImage image = m_isSequence ? sequenceFrame(index) : animationFrame(index);
    if (!image.isNull()) {
        m_currentIndex = index;
        m_current = image;
    }
    return image;
}

QImage ImageFrameList::sequenceFrame(int index)
{
    // Holds over gaps: the largest present number not after the requested one.
    const int wanted = m_firstNumber + index;
    const auto it = std::upper_bound(m_presentNumbers.begin(), m_presentNumbers.end(), wanted);
    const int number = it == m_presentNumbers.begin() ? m_presentNumbers.front() : *(it - 1);
    if (number == m_currentNumber && !m_current.isNull()) {
        return m_current;
    }
    // Uncached on purpose: a sequence would flush every real still out of the shared cache.
    const LoadedImage loaded = ImageLoader::load(ImageSequence::fileForNumber(m_pattern, number));
    if (loaded.image.isNull()) {
        qCWarning(lcMedia) << "image sequence frame" << number << "failed:" << loaded.error;
        return {};
    }
    m_currentNumber = number;
    return loaded.image;
}

QImage ImageFrameList::animationFrame(int index)
{
    if (!m_frames[size_t(index)].isNull()) {
        return m_frames[size_t(index)];
    }
    // Animations composite each frame onto the previous one, so frames can only be decoded in
    // order: restart the reader when stepping backwards.
    if (!m_reader || m_readerNext > index) {
        m_reader = std::make_unique<QImageReader>(m_path);
        m_readerNext = 0;
    }
    QImage image;
    while (m_readerNext <= index) {
        image = toPremultipliedArgb(m_reader->read());
        if (image.isNull()) {
            qCWarning(lcMedia) << "animation frame" << m_readerNext << "of" << m_path
                               << "failed:" << m_reader->errorString();
            m_reader.reset();
            return {};
        }
        const int64_t bytes = image.sizeInBytes();
        if (m_cachedBytes + bytes <= kAnimationBudgetBytes) {
            m_frames[size_t(m_readerNext)] = image;
            m_cachedBytes += bytes;
        }
        ++m_readerNext;
    }
    return image;
}

} // namespace kk::media::detail
