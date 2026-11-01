// SPDX-License-Identifier: GPL-3.0-or-later
// WaveformData — min/max audio peaks for drawing waveforms (+ computing and (de)serialising them).
#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>

#include <atomic>
#include <cstdint>
#include <memory>

namespace cw::media {

/// Audio overview for waveform drawing: per channel, the minimum and maximum sample of every
/// 1/peaksPerSecond-second block of the 48 kHz decoded audio, scaled from [-1, 1] to
/// [-127, 127]. Peak `i` covers media time [i / peaksPerSecond, (i + 1) / peaksPerSecond) s.
/// Mono files have identical L and R. Immutable once published (shared as
/// `std::shared_ptr<const WaveformData>`), so it can be read from any thread.
struct WaveformData
{
    int peaksPerSecond = 100;
    QVector<qint8> minL, maxL, minR, maxR;

    /// Number of peaks (all four vectors have this size).
    int size() const { return int(minL.size()); }
};

/// Default resolution of computed waveforms (peaks per second of audio).
inline constexpr int kWaveformPeaksPerSecond = 100;

/// Decodes the whole audio stream of `path` and computes its waveform. Slow (decodes the
/// entire file — roughly 50-200× real time); call it from a worker thread.
/// `cancel` (optional) is polled regularly; when it becomes true the function returns nullptr.
/// @return nullptr (with `*error` set) if the file has no decodable audio or was cancelled.
std::shared_ptr<WaveformData> computeWaveform(
    const QString& path, const std::atomic<bool>* cancel = nullptr, QString* error = nullptr);

/// Writes `data` to `filePath` in Cutwise's small binary waveform format. Returns success.
bool saveWaveform(const WaveformData& data, const QString& filePath);

/// Reads a file written by saveWaveform(). @return nullptr if missing, corrupt or outdated.
std::shared_ptr<WaveformData> loadWaveform(const QString& filePath);

} // namespace cw::media
