// SPDX-License-Identifier: GPL-3.0-or-later
// MediaCache-backed implementations of the timeline media providers.
#include "ui/timeline/TimelineMedia.h"

#include "media/MediaCache.h"

#include <QObject>

#include <algorithm>
#include <cmath>

namespace kk {

ThumbnailProvider mediaCacheThumbnails()
{
    return [](const QString& path, int64_t timeUs, int height) {
        return media::MediaCache::instance().thumbnail(path, timeUs, height);
    };
}

WaveformProvider mediaCacheWaveforms()
{
    return [](const QString& path, double startUs, double usPerColumn, int columns,
               float* peaks) -> bool {
        const auto wave = media::MediaCache::instance().waveform(path);
        if (!wave || wave->size() == 0)
            return false;
        const int n = wave->size();
        const double peaksPerUs = wave->peaksPerSecond / 1e6;
        const qint8* minL = wave->minL.constData();
        const qint8* maxL = wave->maxL.constData();
        const qint8* minR = wave->minR.constData();
        const qint8* maxR = wave->maxR.constData();
        constexpr float kScale = 1.0f / 127.0f;
        // Each column takes the largest |sample| over the peaks it covers (at least one, so
        // zoomed-in views repeat a peak rather than leaving holes).
        for (int c = 0; c < columns; ++c) {
            const double t0 = startUs + c * usPerColumn;
            int i0 = int(std::floor(t0 * peaksPerUs));
            int i1 = int(std::floor((t0 + usPerColumn) * peaksPerUs));
            if (i1 <= i0)
                i1 = i0 + 1;
            i0 = std::clamp(i0, 0, n);
            i1 = std::clamp(i1, 0, n);
            int peak = 0;
            for (int i = i0; i < i1; ++i) {
                peak = std::max({peak, -int(minL[i]), int(maxL[i]), -int(minR[i]), int(maxR[i])});
            }
            peaks[c] = float(peak) * kScale;
        }
        return true;
    };
}

void connectMediaCache(QObject* receiver, const std::function<void()>& onReady)
{
    media::MediaCache& cache = media::MediaCache::instance();
    QObject::connect(
        &cache, &media::MediaCache::thumbnailReady, receiver, [onReady] { onReady(); });
    QObject::connect(&cache, &media::MediaCache::waveformReady, receiver, [onReady] { onReady(); });
}

} // namespace kk
