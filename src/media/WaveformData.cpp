// SPDX-License-Identifier: GPL-3.0-or-later
// WaveformData — peak computation from AudioDecoder output and the binary cache file format.
#include "media/WaveformData.h"

#include "media/AudioDecoder.h"

#include <QDataStream>
#include <QFile>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <vector>

namespace kk::media {

namespace {

/// Blocks computed per AudioDecoder::read() call (bigger = fewer calls, more memory).
constexpr int kPeaksPerRead = 100;

/// File format identification ("CWWF") and version; bump the version when the layout or the
/// meaning of the stored values changes (older files are then recomputed).
/// 1: qint8 min/max per 10 ms. 2: qint16 min/max and RMS per 5 ms.
constexpr quint32 kWaveformMagic = 0x43575746;
constexpr quint32 kWaveformVersion = 2;

/// Each summary level merges this many blocks of the level below.
constexpr int kLevelFactor = 4;

/// Maps a sample in [-1, 1] to the stored range [-kWaveformScale, kWaveformScale].
qint16 toStored(float sample)
{
    return qint16(std::lround(std::clamp(sample, -1.0f, 1.0f) * float(kWaveformScale)));
}

} // namespace

void WaveformData::buildLevels()
{
    levels.clear();
    const int n = size();
    if (n == 0 || maxL.size() != n || minR.size() != n || maxR.size() != n
        || rmsL.size() != n || rmsR.size() != n)
        return;
    constexpr float kInv = 1.0f / float(kWaveformScale);
    Level base;
    base.peak.resize(size_t(n));
    base.meanSquare.resize(size_t(n));
    for (int i = 0; i < n; ++i) {
        const int peak = std::max({-int(minL[i]), int(maxL[i]), -int(minR[i]), int(maxR[i])});
        base.peak[size_t(i)] = float(peak) * kInv;
        const float l = float(rmsL[i]) * kInv;
        const float r = float(rmsR[i]) * kInv;
        base.meanSquare[size_t(i)] = 0.5f * (l * l + r * r);
    }
    levels.push_back(std::move(base));
    while (levels.back().peak.size() > size_t(kLevelFactor)) {
        const Level& below = levels.back();
        const size_t m = below.peak.size();
        Level up;
        up.peak.resize((m + kLevelFactor - 1) / kLevelFactor);
        up.meanSquare.resize(up.peak.size());
        for (size_t j = 0; j < up.peak.size(); ++j) {
            const size_t a = j * kLevelFactor;
            const size_t b = std::min(m, a + kLevelFactor);
            float peak = 0.0f;
            float sum = 0.0f;
            for (size_t i = a; i < b; ++i) {
                peak = std::max(peak, below.peak[i]);
                sum += below.meanSquare[i];
            }
            up.peak[j] = peak;
            up.meanSquare[j] = sum / float(b - a);
        }
        levels.push_back(std::move(up));
    }
}

WaveformColumn WaveformData::range(double t0Us, double t1Us) const
{
    if (levels.empty() || levels[0].peak.empty() || !(t1Us >= t0Us))
        return {};
    const Level& base = levels[0];
    const int n = int(base.peak.size());
    const double perUs = peaksPerSecond / 1e6;
    const double b0 = t0Us * perUs;
    const double b1 = t1Us * perUs;
    if (b1 - b0 < 1.0) {
        // Narrower than a block (zoomed in): linear between the block centres.
        const double c = 0.5 * (b0 + b1) - 0.5;
        if (c < -0.5 || c > n - 0.5)
            return {};
        const int i = int(std::floor(c));
        const float f = float(c - i);
        const int ia = std::clamp(i, 0, n - 1);
        const int ib = std::clamp(i + 1, 0, n - 1);
        const float peak = base.peak[size_t(ia)] * (1 - f) + base.peak[size_t(ib)] * f;
        const float ms = base.meanSquare[size_t(ia)] * (1 - f) + base.meanSquare[size_t(ib)] * f;
        return {peak, std::sqrt(ms)};
    }
    // Every block the range touches, read from the coarsest levels that fit exactly: the
    // unaligned ends at each level, the aligned middle one level up.
    int i0 = std::clamp(int(std::floor(b0)), 0, n);
    int i1 = std::clamp(int(std::ceil(b1)), 0, n);
    float peak = 0.0f;
    double sum = 0.0;
    double weight = 0.0;
    double blocks = 1.0; // blocks per entry at this level
    for (size_t lvl = 0; lvl < levels.size() && i0 < i1; ++lvl, blocks *= kLevelFactor) {
        const Level& L = levels[lvl];
        const bool top = lvl + 1 == levels.size();
        auto take = [&](int i) {
            peak = std::max(peak, L.peak[size_t(i)]);
            sum += double(L.meanSquare[size_t(i)]) * blocks;
            weight += blocks;
        };
        if (top) {
            for (int i = i0; i < i1; ++i)
                take(i);
            break;
        }
        while (i0 < i1 && i0 % kLevelFactor != 0)
            take(i0++);
        while (i0 < i1 && i1 % kLevelFactor != 0)
            take(--i1);
        i0 /= kLevelFactor;
        i1 /= kLevelFactor;
    }
    return {peak, weight > 0 ? float(std::sqrt(sum / weight)) : 0.0f};
}

qint64 WaveformData::memoryBytes() const
{
    qint64 bytes = qint64(size()) * 6 * qint64(sizeof(qint16));
    for (const Level& L : levels)
        bytes += qint64(L.peak.size() + L.meanSquare.size()) * qint64(sizeof(float));
    return bytes;
}

std::shared_ptr<WaveformData> computeWaveform(
    const QString& path, const std::atomic<bool>* cancel, QString* error)
{
    AudioDecoder decoder;
    if (!decoder.open(path, error)) {
        return nullptr;
    }
    constexpr int kSamplesPerPeak = AudioDecoder::kSampleRate / kWaveformPeaksPerSecond;
    const int64_t length = decoder.lengthSamples();
    const int peakCount = int((length + kSamplesPerPeak - 1) / kSamplesPerPeak);

    auto data = std::make_shared<WaveformData>();
    data->peaksPerSecond = kWaveformPeaksPerSecond;
    for (QVector<qint16>* v :
        {&data->minL, &data->maxL, &data->minR, &data->maxR, &data->rmsL, &data->rmsR})
        v->resize(peakCount);

    std::vector<float> block(size_t(kSamplesPerPeak) * kPeaksPerRead * AudioDecoder::kChannels);
    for (int first = 0; first < peakCount; first += kPeaksPerRead) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            if (error) {
                *error = QStringLiteral("Cancelled");
            }
            return nullptr;
        }
        const int count = std::min(kPeaksPerRead, peakCount - first);
        decoder.read(int64_t(first) * kSamplesPerPeak, block.data(), count * kSamplesPerPeak);
        for (int p = 0; p < count; ++p) {
            const float* samples = block.data() + size_t(p) * kSamplesPerPeak * 2;
            float minL = 0, maxL = 0, minR = 0, maxR = 0;
            double sumL = 0, sumR = 0;
            for (int i = 0; i < kSamplesPerPeak; ++i) {
                const float l = samples[2 * i];
                const float r = samples[2 * i + 1];
                minL = std::min(minL, l);
                maxL = std::max(maxL, l);
                minR = std::min(minR, r);
                maxR = std::max(maxR, r);
                sumL += double(l) * l;
                sumR += double(r) * r;
            }
            const int k = first + p;
            data->minL[k] = toStored(minL);
            data->maxL[k] = toStored(maxL);
            data->minR[k] = toStored(minR);
            data->maxR[k] = toStored(maxR);
            data->rmsL[k] = toStored(float(std::sqrt(sumL / kSamplesPerPeak)));
            data->rmsR[k] = toStored(float(std::sqrt(sumR / kSamplesPerPeak)));
        }
    }
    data->buildLevels();
    return data;
}

bool saveWaveform(const WaveformData& data, const QString& filePath)
{
    QSaveFile file(filePath); // atomic: readers never see a half-written file
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    QDataStream out(&file);
    out.setVersion(QDataStream::Qt_6_4);
    out << kWaveformMagic << kWaveformVersion << qint32(data.peaksPerSecond) << data.minL
        << data.maxL << data.minR << data.maxR << data.rmsL << data.rmsR;
    return out.status() == QDataStream::Ok && file.commit();
}

std::shared_ptr<WaveformData> loadWaveform(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return nullptr;
    }
    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_6_4);
    quint32 magic = 0, version = 0;
    qint32 peaksPerSecond = 0;
    in >> magic >> version >> peaksPerSecond;
    if (magic != kWaveformMagic || version != kWaveformVersion || peaksPerSecond <= 0) {
        return nullptr;
    }
    auto data = std::make_shared<WaveformData>();
    data->peaksPerSecond = peaksPerSecond;
    in >> data->minL >> data->maxL >> data->minR >> data->maxR >> data->rmsL >> data->rmsR;
    const int n = data->size();
    if (in.status() != QDataStream::Ok || data->maxL.size() != n || data->minR.size() != n
        || data->maxR.size() != n || data->rmsL.size() != n || data->rmsR.size() != n) {
        return nullptr;
    }
    data->buildLevels();
    return data;
}

} // namespace kk::media
