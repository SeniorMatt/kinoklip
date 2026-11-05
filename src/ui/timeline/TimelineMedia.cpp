// SPDX-License-Identifier: GPL-3.0-or-later
// MediaCache-backed implementations of the timeline media providers.
#include "ui/timeline/TimelineMedia.h"

#include "media/MediaCache.h"

#include <QObject>


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
               float* peaks, float* rms) -> bool {
        const auto wave = media::MediaCache::instance().waveform(path);
        if (!wave || wave->size() == 0)
            return false;
        for (int c = 0; c < columns; ++c) {
            const double t0 = startUs + c * usPerColumn;
            const media::WaveformColumn v = wave->range(t0, t0 + usPerColumn);
            peaks[c] = v.peak;
            rms[c] = v.rms;
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
