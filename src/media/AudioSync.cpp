// SPDX-License-Identifier: GPL-3.0-or-later
// AudioSync — onset features, FFT cross-correlation and the coarse-to-fine offset search.
#include "media/AudioSync.h"

#include "media/AudioDecoder.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>

namespace kk::media {

namespace {

/// 48 kHz samples per feature value (1 ms).
constexpr int kBlockSamples = AudioDecoder::kSampleRate / kSyncFeatureRate;
/// Mean-square floor added before the logarithm (about -70 dBFS): silence and very quiet
/// noise all map to the same level, so they produce no onsets.
constexpr double kEnergyFloor = 1e-7;
/// Onset = rise of the log energy over the mean of the blocks kOnsetLookbackFar ..
/// kOnsetLookbackNear before it (3-12 ms: fast enough for claps and consonants, slow
/// enough that a sustained level produces nothing).
constexpr int kOnsetLookbackNear = 3;
constexpr int kOnsetLookbackFar = 12;
/// Normalised onsets are clipped to this many standard deviations, so one very loud event
/// (a slammed door close to one microphone) cannot dominate the correlation.
constexpr float kOnsetClip = 8.0f;
/// Feature values merged into one value of the coarse search (100 values per second).
constexpr int kCoarseFactor = 10;
/// The fine search covers the best coarse lag +- this many feature values (ms).
constexpr int kFineRadius = 20;
/// Peaks closer than this (coarse values, 250 ms) to the best one do not count as rivals.
constexpr int kRivalGuard = 25;
/// Decode chunk of syncFeaturesFromFile() in sample frames (1 s).
constexpr int kDecodeChunk = AudioDecoder::kSampleRate;
/// Fewest overlapping feature values a coarse lag needs to be considered at all.
constexpr int kMinOverlapValues = 10;

using Complex = std::complex<double>;

/// In-place iterative radix-2 FFT (size a power of two). @p inverse computes the unscaled
/// inverse transform. Plain Cooley-Tukey: bit-reversal permutation, then log2(n) butterfly
/// passes with twiddles from one std::polar per pass (recurrence; the error stays ~1e-12 for
/// the sizes used here, far below the feature noise).
void fft(std::vector<Complex>& a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double angle = 2.0 * std::numbers::pi / double(len) * (inverse ? 1.0 : -1.0);
        const Complex step = std::polar(1.0, angle);
        for (size_t i = 0; i < n; i += len) {
            Complex w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const Complex u = a[i + k];
                const Complex v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

size_t nextPowerOfTwo(size_t n)
{
    size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

/// Zero mean, unit variance (all zeros when the signal is constant).
template<typename T>
void normalize(std::vector<T>& v)
{
    if (v.empty())
        return;
    double mean = 0.0;
    for (const T x : v)
        mean += double(x);
    mean /= double(v.size());
    double var = 0.0;
    for (const T x : v)
        var += (double(x) - mean) * (double(x) - mean);
    var /= double(v.size());
    const double inv = var > 1e-18 ? 1.0 / std::sqrt(var) : 0.0;
    for (T& x : v)
        x = T((double(x) - mean) * inv);
}

/// Mean of every @p factor values (the last partial group included), renormalised.
std::vector<double> reduce(const std::vector<float>& v, int factor)
{
    std::vector<double> out((v.size() + size_t(factor) - 1) / size_t(factor), 0.0);
    for (size_t i = 0; i < v.size(); ++i)
        out[i / size_t(factor)] += v[i];
    normalize(out);
    return out;
}

/// Prefix sums of squares: p[i] = sum_{k < i} v[k]^2.
template<typename T>
std::vector<double> prefixSquares(const std::vector<T>& v)
{
    std::vector<double> p(v.size() + 1, 0.0);
    for (size_t i = 0; i < v.size(); ++i)
        p[i + 1] = p[i] + double(v[i]) * double(v[i]);
    return p;
}

/// Overlap [lo, hi) of the other signal's indices at lag @p lag (other n <-> reference
/// n + lag) for lengths @p nr / @p no.
std::pair<int64_t, int64_t> overlapRange(int64_t lag, int64_t nr, int64_t no)
{
    return {std::max<int64_t>(0, -lag), std::min<int64_t>(no, nr - lag)};
}

/// Normalised correlation of the 1 kHz features at one lag (direct sum over the overlap).
double nccAt(const std::vector<float>& r, const std::vector<float>& o, int64_t lag)
{
    const auto [lo, hi] = overlapRange(lag, int64_t(r.size()), int64_t(o.size()));
    if (hi - lo < 1)
        return 0.0;
    double sum = 0.0, er = 0.0, eo = 0.0;
    for (int64_t n = lo; n < hi; ++n) {
        const double a = r[size_t(n + lag)];
        const double b = o[size_t(n)];
        sum += a * b;
        er += a * a;
        eo += b * b;
    }
    return er > 0.0 && eo > 0.0 ? sum / std::sqrt(er * eo) : 0.0;
}

} // namespace

double SyncFeatures::seconds() const
{
    return double(onset.size()) / kSyncFeatureRate;
}

// ---- Features ----

void SyncFeatureBuilder::endBlock()
{
    m_logEnergy.push_back(float(std::log(m_blockSum / kBlockSamples + kEnergyFloor)));
    m_blockSum = 0.0;
    m_blockFill = 0;
}

void SyncFeatureBuilder::push(const float* mono48k, int64_t count)
{
    for (int64_t i = 0; i < count; ++i) {
        const double x = mono48k[i];
        m_blockSum += x * x;
        if (++m_blockFill == kBlockSamples)
            endBlock();
    }
}

void SyncFeatureBuilder::pushStereo(const float* stereo48k, int64_t frames)
{
    for (int64_t i = 0; i < frames; ++i) {
        const double x = 0.5 * (double(stereo48k[2 * i]) + double(stereo48k[2 * i + 1]));
        m_blockSum += x * x;
        if (++m_blockFill == kBlockSamples)
            endBlock();
    }
}

SyncFeatures SyncFeatureBuilder::finish()
{
    if (m_blockFill > 0) {
        // The last partial block is scaled as if it were complete (same level).
        m_blockSum *= double(kBlockSamples) / m_blockFill;
        endBlock();
    }
    const std::vector<float> e = std::move(m_logEnergy);
    m_logEnergy = {};
    SyncFeatures f;
    f.onset.assign(e.size(), 0.0f);
    // Running sums of the log energy for the look-back mean.
    std::vector<double> prefix(e.size() + 1, 0.0);
    for (size_t i = 0; i < e.size(); ++i)
        prefix[i + 1] = prefix[i] + e[i];
    for (size_t i = kOnsetLookbackFar; i < e.size(); ++i) {
        const double mean = (prefix[i - kOnsetLookbackNear + 1] - prefix[i - kOnsetLookbackFar])
            / (kOnsetLookbackFar - kOnsetLookbackNear + 1);
        f.onset[i] = float(std::max(0.0, double(e[i]) - mean));
    }
    normalize(f.onset);
    for (float& x : f.onset)
        x = std::min(x, kOnsetClip);
    return f;
}

SyncFeatures syncFeatures(const float* mono48k, int64_t count)
{
    SyncFeatureBuilder b;
    b.push(mono48k, count);
    return b.finish();
}

std::optional<SyncFeatures> syncFeaturesFromFile(
    const QString& path, const std::atomic<bool>* cancel, QString* error)
{
    AudioDecoder decoder;
    if (!decoder.open(path, error))
        return std::nullopt;
    SyncFeatureBuilder builder;
    std::vector<float> buffer(size_t(kDecodeChunk) * AudioDecoder::kChannels);
    // lengthSamples() is an estimate until the end was reached once, exact afterwards, so
    // the loop ends at the real end even when the container's duration is off.
    for (int64_t pos = 0; pos < decoder.lengthSamples(); pos += kDecodeChunk) {
        if (cancel && cancel->load()) {
            if (error)
                *error = QStringLiteral("cancelled");
            return std::nullopt;
        }
        const int frames = int(std::min<int64_t>(kDecodeChunk, decoder.lengthSamples() - pos));
        decoder.read(pos, buffer.data(), frames);
        builder.pushStereo(buffer.data(), frames);
    }
    return builder.finish();
}

// ---- Correlation ----

std::vector<double> circularCorrelation(const std::vector<double>& a, const std::vector<double>& b)
{
    const size_t m = a.size();
    // Both real signals in one complex FFT: z = a + i b, then A = (Z[k] + conj Z[m-k]) / 2,
    // B = (Z[k] - conj Z[m-k]) / 2i. Cross-spectrum A * conj(B); its inverse transform is
    // the circular correlation.
    std::vector<Complex> z(m);
    for (size_t i = 0; i < m; ++i)
        z[i] = Complex(a[i], b[i]);
    fft(z, false);
    std::vector<Complex> cross(m);
    for (size_t k = 0; k < m; ++k) {
        const Complex zk = z[k];
        const Complex zm = std::conj(z[(m - k) % m]);
        const Complex fa = (zk + zm) * 0.5;
        const Complex fb = (zk - zm) * Complex(0.0, -0.5);
        cross[k] = fa * std::conj(fb);
    }
    fft(cross, true);
    std::vector<double> out(m);
    for (size_t i = 0; i < m; ++i)
        out[i] = cross[i].real() / double(m);
    return out;
}

SyncMatch findSyncOffset(
    const SyncFeatures& reference, const SyncFeatures& other, const SyncOptions& options)
{
    SyncMatch match;
    const std::vector<double> r = reduce(reference.onset, kCoarseFactor);
    const std::vector<double> o = reduce(other.onset, kCoarseFactor);
    const auto nr = int64_t(r.size());
    const auto no = int64_t(o.size());
    if (nr < kMinOverlapValues || no < kMinOverlapValues)
        return match;
    const double coarseRate = double(kSyncFeatureRate) / kCoarseFactor;
    const int64_t minOverlap = std::max<int64_t>(kMinOverlapValues,
        std::min<int64_t>(int64_t(options.minOverlapSeconds * coarseRate), std::min(nr, no) / 2));
    const int64_t maxLag = options.maxOffsetSeconds > 0.0
        ? int64_t(std::ceil(options.maxOffsetSeconds * coarseRate))
        : std::numeric_limits<int64_t>::max();

    // (1) Coarse: every lag at once. Zero padding to >= nr + no makes the circular
    // correlation equal the linear one: lag L >= 0 at index L, L < 0 at m + L.
    const size_t m = nextPowerOfTwo(size_t(nr + no));
    std::vector<double> a(m, 0.0), b(m, 0.0);
    std::copy(r.begin(), r.end(), a.begin());
    std::copy(o.begin(), o.end(), b.begin());
    const std::vector<double> c = circularCorrelation(a, b);
    const std::vector<double> pr = prefixSquares(r);
    const std::vector<double> po = prefixSquares(o);

    // Score per lag; lags without enough overlap get -inf.
    const int64_t lagMin = -(no - 1);
    const int64_t lagMax = nr - 1;
    std::vector<double> score(size_t(lagMax - lagMin + 1), -1e300);
    int64_t best = 0;
    double bestScore = -1e300;
    for (int64_t lag = lagMin; lag <= lagMax; ++lag) {
        if (std::abs(lag) > maxLag)
            continue;
        const auto [lo, hi] = overlapRange(lag, nr, no);
        const int64_t overlap = hi - lo;
        if (overlap < minOverlap)
            continue;
        const double er = pr[size_t(hi + lag)] - pr[size_t(lo + lag)];
        const double eo = po[size_t(hi)] - po[size_t(lo)];
        if (er <= 1e-12 || eo <= 1e-12)
            continue;
        const double raw = c[lag >= 0 ? size_t(lag) : size_t(int64_t(m) + lag)];
        const double s = raw / std::sqrt(er * eo) * std::sqrt(double(overlap));
        score[size_t(lag - lagMin)] = s;
        if (s > bestScore) {
            bestScore = s;
            best = lag;
        }
    }
    if (bestScore <= 0.0)
        return match;
    double rival = 0.0;
    for (int64_t lag = lagMin; lag <= lagMax; ++lag) {
        if (std::abs(lag - best) > kRivalGuard)
            rival = std::max(rival, score[size_t(lag - lagMin)]);
    }

    // (2) Fine: 1 kHz features around the coarse peak, direct normalised correlation.
    const std::vector<float>& rf = reference.onset;
    const std::vector<float>& of = other.onset;
    const int64_t centre = best * kCoarseFactor;
    const int64_t radius = kFineRadius + kCoarseFactor;
    int64_t fineBest = centre;
    double fineValue = -2.0;
    std::vector<double> fine(size_t(2 * radius + 1), 0.0);
    for (int64_t d = -radius; d <= radius; ++d) {
        const double v = nccAt(rf, of, centre + d);
        fine[size_t(d + radius)] = v;
        if (v > fineValue) {
            fineValue = v;
            fineBest = centre + d;
        }
    }
    // Parabola through the peak and its neighbours: sub-millisecond offset.
    double subSample = 0.0;
    const int64_t idx = fineBest - centre + radius;
    if (idx > 0 && idx < int64_t(fine.size()) - 1) {
        const double y0 = fine[size_t(idx - 1)], y1 = fine[size_t(idx)], y2 = fine[size_t(idx + 1)];
        const double denom = y0 - 2.0 * y1 + y2;
        if (denom < 0.0)
            subSample = std::clamp(0.5 * (y0 - y2) / denom, -0.5, 0.5);
    }

    const auto [lo, hi] = overlapRange(best, nr, no);
    match.offsetSeconds = (double(fineBest) + subSample) / kSyncFeatureRate;
    match.correlation = fineValue;
    match.score = bestScore;
    match.confidence = std::clamp(1.0 - std::max(0.0, rival) / bestScore, 0.0, 1.0);
    match.overlapSeconds = double(hi - lo) / coarseRate;
    match.found = match.score >= kSyncMinScore && match.confidence >= kSyncMinConfidence;
    return match;
}

} // namespace kk::media
