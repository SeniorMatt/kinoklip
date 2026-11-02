// SPDX-License-Identifier: GPL-3.0-or-later
// TimeStretch / SourceCache implementation (WSOLA, see TimeStretch.h).
#include "render/TimeStretch.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace kk::render {

namespace {

constexpr int kChannels = 2;
/// A forward jump up to this many frames is read through instead of seeking.
constexpr int64_t kReadThroughFrames = 96000;
/// Largest alignment window read at once (very high speeds skip the search).
constexpr int64_t kMaxSearchFrames = int64_t(1) << 18;

int64_t floorDiv(int64_t a, int64_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

} // namespace

// ---- SourceCache ----

const float* SourceCache::fetch(int64_t first, int count, const Reader& read)
{
    const int64_t end = first + count;
    const int64_t cacheEnd = m_start + m_frames;
    if (m_frames > 0 && first >= m_start && end <= cacheEnd)
        return m_buffer.data() + size_t(first - m_start) * kChannels;
    if (m_frames > 0 && first >= m_start && first <= cacheEnd + kReadThroughFrames) {
        // Extend forward (contiguous for the decoder): drop history older than kHistoryFrames
        // before the request, then read [cacheEnd, end).
        const int64_t keepFrom = std::clamp<int64_t>(first - kHistoryFrames, m_start, cacheEnd);
        const int64_t drop = keepFrom - m_start;
        if (drop > 0) {
            m_buffer.erase(m_buffer.begin(), m_buffer.begin() + std::ptrdiff_t(drop * kChannels));
            m_start = keepFrom;
            m_frames -= drop;
        }
        if (end > cacheEnd) {
            const int64_t add = end - cacheEnd;
            m_buffer.resize(size_t(m_frames + add) * kChannels);
            read(cacheEnd, m_buffer.data() + size_t(m_frames) * kChannels, int(add));
            m_frames += add;
        }
        return m_buffer.data() + size_t(first - m_start) * kChannels;
    }
    // Before the cache or far ahead: start over (the decoder seeks once).
    m_buffer.resize(size_t(count) * kChannels);
    read(first, m_buffer.data(), count);
    m_start = first;
    m_frames = count;
    return m_buffer.data();
}

void SourceCache::clear()
{
    m_buffer.clear();
    m_start = 0;
    m_frames = 0;
}

// ---- TimeStretch ----

TimeStretch::TimeStretch()
    : m_window(size_t(kWindow))
{
    // Periodic Hann: w[n] + w[n + N/2] == 1 exactly, so 50 % overlap-add has unity gain.
    for (int n = 0; n < kWindow; ++n)
        m_window[size_t(n)]
            = float(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(n) / double(kWindow)));
}

void TimeStretch::render(int64_t from, int frames, const Position& position, SourceCache& cache,
    const SourceCache::Reader& read, float* out)
{
    std::fill(out, out + size_t(frames) * kChannels, 0.0f);
    if (frames <= 0)
        return;
    const int64_t to = from + frames;
    const int64_t k0 = floorDiv(from - kWindow, kHop) + 1; // first grain reaching `from`
    const int64_t k1 = floorDiv(to - 1, kHop);
    for (int64_t k = k0; k <= k1; ++k) {
        const int64_t source = grainSource(k, position, cache, read);
        const float* src = cache.fetch(source, kWindow, read);
        const int64_t o = k * kHop;
        const int n0 = int(std::max<int64_t>(0, from - o));
        const int n1 = int(std::min<int64_t>(kWindow, to - o));
        float* dst = out + size_t(o + n0 - from) * kChannels;
        for (int n = n0; n < n1; ++n, dst += kChannels) {
            const float w = m_window[size_t(n)];
            dst[0] += w * src[size_t(n) * kChannels];
            dst[1] += w * src[size_t(n) * kChannels + 1];
        }
    }
}

int64_t TimeStretch::grainSource(
    int64_t k, const Position& position, SourceCache& cache, const SourceCache::Reader& read)
{
    if (m_lastGrain != kNoGrain && k == m_lastGrain)
        return m_lastSource;
    auto ideal = [&](int64_t grain) { return int64_t(std::llround(position(grain * kHop))); };
    const int64_t phase = k - floorDiv(k, kAnchorGrains) * kAnchorGrains;
    if (m_lastGrain != kNoGrain && k == m_lastGrain + 1 && phase != 0) {
        m_lastSource = align(ideal(k), m_lastSource + kHop, cache, read);
        m_lastGrain = k;
        return m_lastSource;
    }
    // Restart the chain kWarmupGrains before the anchor grain, aligned to the unshifted grain
    // before it, and continue grain by grain up to k (the same values a sequential render
    // produces). The warm-up lets the restarted chain lock onto the waveform the way the
    // running chain did (alignments snap to the local period), so the switch at the anchor is
    // seamless in practice.
    const int64_t first = k - phase - kWarmupGrains;
    int64_t source = align(ideal(first), ideal(first - 1) + kHop, cache, read);
    for (int64_t j = first + 1; j <= k; ++j)
        source = align(ideal(j), source + kHop, cache, read);
    m_lastGrain = k;
    m_lastSource = source;
    return source;
}

int64_t TimeStretch::align(
    int64_t ideal, int64_t continuation, SourceCache& cache, const SourceCache::Reader& read)
{
    if (ideal == continuation)
        return ideal; // 100 %: the natural continuation is the ideal position
    constexpr int kOverlap = kWindow - kHop;
    const int64_t lo = std::min(continuation, ideal - kTolerance);
    const int64_t hi = std::max(continuation + kOverlap, ideal + kTolerance + kOverlap);
    if (hi - lo > kMaxSearchFrames)
        return ideal;
    const int span = int(hi - lo);
    const float* p = cache.fetch(lo, span, read);
    m_mono.resize(size_t(span));
    for (int i = 0; i < span; ++i)
        m_mono[size_t(i)] = p[size_t(i) * kChannels] + p[size_t(i) * kChannels + 1];
    const float* tmpl = m_mono.data() + (continuation - lo);
    const float* base = m_mono.data() + (ideal - lo);
    auto score = [&](int d, int step) {
        const float* cand = base + d;
        float s = 0.0f;
        for (int n = 0; n < kOverlap; n += step)
            s += tmpl[n] * cand[n];
        return s;
    };
    // Coarse: every second offset and sample; then +-1 at full resolution. Offset 0 wins ties
    // (silence, or a pure DC template).
    int best = 0;
    float bestScore = score(0, 2);
    for (int d = -kTolerance; d <= kTolerance; d += 2) {
        const float s = score(d, 2);
        if (s > bestScore + 1e-4f * std::abs(bestScore) + 1e-9f) {
            bestScore = s;
            best = d;
        }
    }
    int refined = best;
    float refinedScore = score(best, 1);
    for (int d : {best - 1, best + 1}) {
        if (d < -kTolerance || d > kTolerance)
            continue;
        const float s = score(d, 1);
        if (s > refinedScore) {
            refinedScore = s;
            refined = d;
        }
    }
    return ideal + refined;
}

} // namespace kk::render
