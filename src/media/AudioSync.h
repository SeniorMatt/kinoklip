// SPDX-License-Identifier: GPL-3.0-or-later
// AudioSync — finds the time offset between two recordings of the same event from their
// sound (Auto Sync Audio Based on Waveform, multicam clips): onset envelopes + FFT
// cross-correlation, coarse to fine, with a confidence measure.
#pragma once

#include <QString>

#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

namespace kk::media {

/// Sync features of one recording: the onset strength of its downmixed audio at
/// kSyncFeatureRate values per second (value i covers media time [i, i + 1) ms), normalised
/// to zero mean and unit variance. Independent of gain (log energy) and cheap to correlate.
/// Plain value type.
struct SyncFeatures
{
    std::vector<float> onset; ///< kSyncFeatureRate values per second of audio
    /// Seconds of audio the features describe.
    [[nodiscard]] double seconds() const;
};

/// Feature rate of SyncFeatures (values per second): 1 ms resolution, the precision of the
/// final offset (well below one video frame).
inline constexpr int kSyncFeatureRate = 1000;

/// Builds SyncFeatures incrementally from 48 kHz mono samples (streaming: a one-hour file
/// never needs to be held in memory). Not thread-safe; one per recording.
class SyncFeatureBuilder
{
public:
    /// Appends @p count mono samples at 48 kHz.
    void push(const float* mono48k, int64_t count);
    /// Appends @p frames interleaved stereo frames at 48 kHz (downmixed to mono).
    void pushStereo(const float* stereo48k, int64_t frames);
    /// Finishes: onset detection and normalisation. The builder is empty afterwards.
    [[nodiscard]] SyncFeatures finish();

private:
    void endBlock();

    std::vector<float> m_logEnergy; ///< one value per 1 ms block
    double m_blockSum = 0.0; ///< sum of squares of the current block
    int m_blockFill = 0; ///< samples in the current block
};

/// Features of @p count mono samples at 48 kHz (convenience around SyncFeatureBuilder).
[[nodiscard]] SyncFeatures syncFeatures(const float* mono48k, int64_t count);

/// Decodes the audio of @p path (AudioDecoder: 48 kHz, downmixed) and computes its features.
/// Slow (decodes the whole stream; roughly 50-200x real time): call it from a worker thread.
/// @p cancel (optional) is polled; nullopt with @p error set on failure, no audio or cancel.
[[nodiscard]] std::optional<SyncFeatures> syncFeaturesFromFile(
    const QString& path, const std::atomic<bool>* cancel = nullptr, QString* error = nullptr);

/// Search options of findSyncOffset().
struct SyncOptions
{
    /// Largest |offset| searched, in seconds (0 = every offset with enough overlap).
    double maxOffsetSeconds = 0.0;
    /// Overlap the two recordings must have at least, in seconds (capped at half of the
    /// shorter recording, so short clips can still be synced).
    double minOverlapSeconds = 2.0;
};

/// Result of findSyncOffset().
struct SyncMatch
{
    /// True if a match was found with enough confidence; false = no reliable common sound.
    bool found = false;
    /// Media time 0 of the *other* recording lies at this time of the *reference* (seconds,
    /// may be negative): other(t) sounds like reference(t + offsetSeconds).
    double offsetSeconds = 0.0;
    /// 0..1: how clearly the best alignment beats every other one (1 - second / best peak
    /// score). Around 0 for unrelated recordings or periodic sound (ambiguous); above
    /// kSyncMinConfidence for a match.
    double confidence = 0.0;
    /// Normalised correlation (-1..1) of the features at the found offset.
    double correlation = 0.0;
    /// Detection score of the best peak: correlation x sqrt(overlap in feature values), about
    /// a standard normal variable for unrelated sound (so ~4-5 is chance level).
    double score = 0.0;
    /// Overlap of the two recordings at the found offset, in seconds.
    double overlapSeconds = 0.0;
};

/// Smallest SyncMatch::score reported as found.
inline constexpr double kSyncMinScore = 9.0;
/// Smallest SyncMatch::confidence reported as found.
inline constexpr double kSyncMinConfidence = 0.2;

/// Finds where @p other lies relative to @p reference.
///
/// Coarse to fine: (1) both feature signals are reduced to 100 values per second and
/// cross-correlated for every lag at once with an FFT (O(n log n); a one-hour pair takes
/// milliseconds); every lag's correlation is normalised by the energies of the overlapping
/// parts (prefix sums), so recordings of different lengths and partial overlaps compare
/// fairly, and scored with correlation x sqrt(overlap) (chance level is the same for every
/// overlap length). (2) Around the best coarse lag, the 1 kHz features are correlated
/// directly over +-20 ms and the peak is refined with a parabola (sub-millisecond).
/// Confidence compares the best peak with the best one at least 250 ms away.
[[nodiscard]] SyncMatch findSyncOffset(
    const SyncFeatures& reference, const SyncFeatures& other, const SyncOptions& options = {});

/// Circular cross-correlation helper exposed for tests: result[k] = sum_n a[(n + k) mod m] *
/// b[n] with m = a.size() = b.size() (a power of two), computed with an FFT.
[[nodiscard]] std::vector<double> circularCorrelation(
    const std::vector<double>& a, const std::vector<double>& b);

} // namespace kk::media
