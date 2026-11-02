// SPDX-License-Identifier: GPL-3.0-or-later
// TimeStretch — pitch-preserving audio time stretching (WSOLA) for retimed clips, with
// deterministic random access, plus SourceCache, the forward-reading sample cache it reads from.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace kk::render {

/// Recently read source samples of one clip (48 kHz interleaved stereo float). Retimed audio
/// reads overlapping and slightly backward ranges on every mix call; the cache serves them and
/// only reads the new samples after its end, so the decoder keeps its contiguous path (a
/// read before the cache seeks once and restarts it).
///
/// Thread-safety: not thread-safe (one per mixer decoder).
class SourceCache
{
public:
    /// Reads @p count frames starting at source sample @p first into @p out (2 * count floats).
    using Reader = std::function<void(int64_t first, float* out, int count)>;

    /// Pointer to frames [@p first, @p first + @p count), valid until the next fetch().
    const float* fetch(int64_t first, int count, const Reader& read);
    /// Forgets everything (the clip's media changed).
    void clear();

    /// History kept before the newest request (frames): covers the WSOLA look-back.
    static constexpr int kHistoryFrames = 16384;

private:
    std::vector<float> m_buffer; ///< interleaved, frames [m_start, m_start + m_frames)
    int64_t m_start = 0;
    int64_t m_frames = 0;
};

/// WSOLA (waveform-similarity overlap-add) time stretch: the output is built from Hann
/// windowed grains of kWindow samples every kHop output samples (50 % overlap, the windows
/// sum to exactly 1). Grain k sits at output offset k * kHop and takes its samples from the
/// source position the retime curve maps that offset to, moved by up to +-kTolerance samples
/// so that it continues the previous grain's waveform (best cross-correlation of the overlap
/// with the natural continuation of the previous grain). The pitch therefore stays while the
/// tempo follows any speed curve (also ramps).
///
/// Random access: the grain chain restarts every kAnchorGrains grains, computed from
/// kWarmupGrains grains earlier (whose first grain is aligned to the unshifted grain before
/// it), so rendering a range gives the same samples no matter where playback started or how
/// the range is split into calls. Sequential calls continue the chain and only recompute the
/// warm-up at each restart (~19 % more alignments).
///
/// Cost: ~70 k multiply-adds per grain (coarse search on every second sample and offset, then
/// a +-1 refinement), ~94 grains per second: ~12 ms of CPU per second of audio per clip
/// (test_timestretch).
///
/// Thread-safety: not thread-safe (one per mixer decoder).
class TimeStretch
{
public:
    static constexpr int kWindow = 1024; ///< grain length (21 ms at 48 kHz)
    static constexpr int kHop = kWindow / 2; ///< output hop (50 % overlap)
    static constexpr int kTolerance = 256; ///< alignment search range (+-5 ms)
    static constexpr int kAnchorGrains = 64; ///< chain restart period (~0.68 s)
    static constexpr int kWarmupGrains = 12; ///< grains computed before each restart

    /// Source sample (fractional) shown at output offset @p outSample (0 = the stretch origin,
    /// e.g. the clip start).
    using Position = std::function<double(int64_t outSample)>;

    TimeStretch();

    /// Writes output samples [@p from, @p from + @p frames) (offsets from the origin) into
    /// @p out (interleaved stereo, overwritten).
    void render(int64_t from, int frames, const Position& position, SourceCache& cache,
        const SourceCache::Reader& read, float* out);
    /// Forgets the grain chain (the curve or the clip changed).
    void invalidate() { m_lastGrain = kNoGrain; }

private:
    static constexpr int64_t kNoGrain = INT64_MIN;
    /// Source start of grain @p k (chain; uses and updates m_lastGrain / m_lastSource).
    int64_t grainSource(
        int64_t k, const Position& position, SourceCache& cache, const SourceCache::Reader& read);
    /// Best start near @p ideal continuing the waveform at @p continuation.
    int64_t align(
        int64_t ideal, int64_t continuation, SourceCache& cache, const SourceCache::Reader& read);

    std::vector<float> m_window; ///< periodic Hann, kWindow taps
    std::vector<float> m_mono; ///< scratch for the search (mono)
    int64_t m_lastGrain = kNoGrain;
    int64_t m_lastSource = 0;
};

} // namespace kk::render
