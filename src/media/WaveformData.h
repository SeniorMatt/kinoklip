// SPDX-License-Identifier: GPL-3.0-or-later
// WaveformData — audio peaks and RMS for drawing waveforms (+ computing and (de)serialising
// them, and a multi-resolution summary for per-pixel-column queries).
#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace kk::media {

/// Full scale of the stored values: a sample of ±1.0 (0 dBFS) is stored as ±kWaveformScale.
inline constexpr int kWaveformScale = 32767;

/// Peak and RMS amplitude of a stretch of audio, linear, 0..1 of full scale (0 dBFS = 1).
struct WaveformColumn
{
    float peak = 0.0f; ///< largest |sample| of both channels
    float rms = 0.0f; ///< root mean square of both channels
};

/// Audio overview for waveform drawing. Per channel, the minimum and maximum sample and the RMS
/// of every 1/peaksPerSecond-second block of the 48 kHz decoded audio, linear, scaled from
/// [-1, 1] to [-kWaveformScale, kWaveformScale] (never normalised: 0 dBFS is full scale).
/// Block `i` covers media time [i / peaksPerSecond, (i + 1) / peaksPerSecond) s. Mono files
/// have identical L and R. Immutable once published (shared as
/// `std::shared_ptr<const WaveformData>`), so it can be read from any thread.
struct WaveformData
{
    int peaksPerSecond = 200;
    QVector<qint16> minL, maxL, minR, maxR;
    QVector<qint16> rmsL, rmsR;

    /// Both channels merged, per block (levels[0]) and every next level 4 times coarser: the
    /// largest |sample| and the mean square of each 4^level blocks. Not stored on disk; made
    /// by buildLevels().
    struct Level
    {
        std::vector<float> peak;
        std::vector<float> meanSquare;
    };
    std::vector<Level> levels;

    /// Number of blocks (all six vectors have this size).
    int size() const { return int(minL.size()); }

    /// (Re)builds `levels` from the per-channel vectors. Call once before publishing.
    void buildLevels();

    /// Peak and RMS of the media time [t0Us, t1Us): exact over the blocks the range covers
    /// (O(log n) blocks read thanks to the levels, so any zoom level is cheap). A range
    /// shorter than a block is interpolated between the neighbouring blocks, so zoomed-in
    /// views are smooth instead of stair steps. Requires buildLevels().
    WaveformColumn range(double t0Us, double t1Us) const;

    /// Approximate memory use in bytes (for cache budgets).
    qint64 memoryBytes() const;
};

/// Default resolution of computed waveforms (blocks per second of audio; 240 samples each).
inline constexpr int kWaveformPeaksPerSecond = 200;

/// Decodes the whole audio stream of `path` and computes its waveform. Slow (decodes the
/// entire file — roughly 50-200× real time); call it from a worker thread.
/// `cancel` (optional) is polled regularly; when it becomes true the function returns nullptr.
/// @return nullptr (with `*error` set) if the file has no decodable audio or was cancelled.
std::shared_ptr<WaveformData> computeWaveform(
    const QString& path, const std::atomic<bool>* cancel = nullptr, QString* error = nullptr);

/// Writes `data` to `filePath` in Kinoklip's small binary waveform format. Returns success.
bool saveWaveform(const WaveformData& data, const QString& filePath);

/// Reads a file written by saveWaveform() (levels built). @return nullptr if missing, corrupt
/// or of an older format version (the caller then computes it again).
std::shared_ptr<WaveformData> loadWaveform(const QString& filePath);

} // namespace kk::media
