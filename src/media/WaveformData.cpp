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

/// Peaks computed per AudioDecoder::read() call (bigger = fewer calls, more memory).
constexpr int kPeaksPerRead = 100;

/// File format identification ("CWWF") and version; bump the version when the layout changes.
constexpr quint32 kWaveformMagic = 0x43575746;
constexpr quint32 kWaveformVersion = 1;

/// Maps a sample in [-1, 1] to the stored range [-127, 127].
qint8 toPeak(float sample)
{
    return qint8(std::lround(std::clamp(sample, -1.0f, 1.0f) * 127.0f));
}

} // namespace

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
    data->minL.resize(peakCount);
    data->maxL.resize(peakCount);
    data->minR.resize(peakCount);
    data->maxR.resize(peakCount);

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
            for (int i = 0; i < kSamplesPerPeak; ++i) {
                minL = std::min(minL, samples[2 * i]);
                maxL = std::max(maxL, samples[2 * i]);
                minR = std::min(minR, samples[2 * i + 1]);
                maxR = std::max(maxR, samples[2 * i + 1]);
            }
            data->minL[first + p] = toPeak(minL);
            data->maxL[first + p] = toPeak(maxL);
            data->minR[first + p] = toPeak(minR);
            data->maxR[first + p] = toPeak(maxR);
        }
    }
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
        << data.maxL << data.minR << data.maxR;
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
    in >> data->minL >> data->maxL >> data->minR >> data->maxR;
    const int n = data->size();
    if (in.status() != QDataStream::Ok || data->maxL.size() != n || data->minR.size() != n
        || data->maxR.size() != n) {
        return nullptr;
    }
    return data;
}

} // namespace kk::media
