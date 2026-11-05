// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineMedia — injectable thumbnail / waveform sources for clip drawing, with defaults
// backed by media::MediaCache.
#pragma once

#include <QImage>
#include <QString>

#include <cstdint>
#include <functional>

class QObject;

namespace kk {

/// Returns a thumbnail of the frame at media time @p timeUs (µs) about @p height px high, or a
/// null QImage if it is not available yet (the timeline then draws a neutral placeholder and
/// repaints when TimelineWidget::refreshMedia() is called).
using ThumbnailProvider = std::function<QImage(const QString& path, int64_t timeUs, int height)>;

/// Fills @p peaks[0 .. columns) and @p rms[0 .. columns) with the peak and RMS amplitude of
/// the media audio, linear 0..1 of full scale (0 dBFS = 1, never normalised), column i
/// covering media time [startUs + i * usPerColumn, startUs + (i + 1) * usPerColumn): the
/// largest |sample| / the RMS over that whole stretch. Returns false if the waveform is not
/// available (yet): a flat placeholder is drawn.
/// Called from paintEvent: must not block and should not allocate.
using WaveformProvider = std::function<bool(const QString& path, double startUs,
    double usPerColumn, int columns, float* peaks, float* rms)>;

/// Provider using media::MediaCache::thumbnail() (asynchronous, cached on disk).
[[nodiscard]] ThumbnailProvider mediaCacheThumbnails();
/// Provider using media::MediaCache::waveform() (per-column peak and RMS of
/// both channels from its multi-resolution summary).
[[nodiscard]] WaveformProvider mediaCacheWaveforms();
/// Calls @p onReady (in @p receiver's context) whenever MediaCache finishes a thumbnail or a
/// waveform, so the view can repaint.
void connectMediaCache(QObject* receiver, const std::function<void()>& onReady);

} // namespace kk
