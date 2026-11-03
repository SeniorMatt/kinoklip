// SPDX-License-Identifier: GPL-3.0-or-later
// AudioNormalize implementation.
#include "render/AudioNormalize.h"

#include "media/AudioDecoder.h"
#include "render/LoudnessMeter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace kk::audionorm {

namespace {

constexpr int kBlockFrames = 4800; ///< 100 ms per read: one meter block, cheap cancel checks
constexpr double kSilentDb = -150.0; ///< levels at or below this have nothing to normalize

double toDb(double linear)
{
    return linear > 0.0 ? 20.0 * std::log10(linear) : dsp::LoudnessMeter::kSilence;
}

} // namespace

double defaultTarget(Mode mode)
{
    switch (mode) {
    case Mode::SamplePeak:
        return -9.0;
    case Mode::TruePeak:
        return -2.0;
    case Mode::Loudness:
        break;
    }
    return -23.0;
}

double levelOf(const Measurement& m, Mode mode)
{
    switch (mode) {
    case Mode::SamplePeak:
        return m.samplePeakDb;
    case Mode::TruePeak:
        return m.truePeakDb;
    case Mode::Loudness:
        break;
    }
    return m.loudnessLufs;
}

Measurement measure(const Source& source, const std::atomic<bool>* cancel,
    const std::function<void(double)>& progress)
{
    Measurement result;
    media::AudioDecoder decoder;
    if (!decoder.open(source.path, &result.error))
        return result;
    decoder.setChannelMapping(source.attributes);
    constexpr int rate = media::AudioDecoder::kSampleRate;
    const int64_t first = source.startUs * rate / 1000000;
    const int64_t last = std::max(first, source.endUs * rate / 1000000);
    const int64_t total = last - first;

    dsp::LoudnessMeter meter; // one per measurement: the integrated value covers this clip
    std::vector<float> block(size_t(kBlockFrames) * media::AudioDecoder::kChannels);
    float peak = 0.0f;
    for (int64_t at = first; at < last; at += kBlockFrames) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            result.error = QStringLiteral("cancelled");
            return result;
        }
        const int frames = int(std::min<int64_t>(kBlockFrames, last - at));
        decoder.read(at, block.data(), frames);
        for (int i = 0; i < frames * media::AudioDecoder::kChannels; ++i)
            peak = std::max(peak, std::abs(block[size_t(i)]));
        meter.process(block.data(), frames);
        if (progress && total > 0)
            progress(double(at + frames - first) / double(total));
    }
    result.valid = true;
    result.samplePeakDb = toDb(peak);
    result.truePeakDb = meter.truePeakMax();
    result.loudnessLufs = meter.integrated();
    return result;
}

std::vector<double> gains(const std::vector<double>& levels, double target, Scope scope)
{
    std::vector<double> out(levels.size(), std::numeric_limits<double>::quiet_NaN());
    double loudest = -std::numeric_limits<double>::infinity();
    for (const double l : levels) {
        if (l > kSilentDb)
            loudest = std::max(loudest, l);
    }
    for (size_t i = 0; i < levels.size(); ++i) {
        if (levels[i] <= kSilentDb)
            continue;
        out[i] = scope == Scope::Relative ? target - loudest : target - levels[i];
    }
    return out;
}

} // namespace kk::audionorm
