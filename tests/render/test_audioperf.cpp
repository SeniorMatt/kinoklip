// SPDX-License-Identifier: GPL-3.0-or-later
// Measurement: the audio mix as the playback feeder runs it (1024-frame chunks, 21 ms of sound
// each) with typical effect chains on 1 and 4 tracks. Prints the cost of a chunk (median, 99th
// percentile, worst) as a share of the chunk's duration, and the heap allocations per chunk in
// steady state. Asserts only generous bounds (a chunk must cost far less than its duration), and
// skips them when the machine is overloaded.
#include "RenderTestUtil.h"
#include "core/AudioEffectKeys.h"
#include "core/EffectRegistry.h"
#include "render/AudioMixer.h"

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <numbers>
#include <vector>

using namespace kk;
using namespace kk::test;

namespace {

std::atomic<long> g_allocations {0};

constexpr int kChunk = 1024;
const QString kMedia = QStringLiteral("fx");

bool writeNoiseWav(const QString& path, int seconds)
{
    const quint32 frames = quint32(48000 * seconds);
    QByteArray bytes;
    auto u32 = [&](quint32 v) {
        for (int i = 0; i < 4; ++i)
            bytes.append(char((v >> (8 * i)) & 0xff));
    };
    auto u16 = [&](quint16 v) {
        bytes.append(char(v & 0xff));
        bytes.append(char(v >> 8));
    };
    bytes.append("RIFF");
    u32(36 + frames * 4);
    bytes.append("WAVEfmt ");
    u32(16);
    u16(1);
    u16(2);
    u32(48000);
    u32(48000 * 4);
    u16(4);
    u16(16);
    bytes.append("data");
    u32(frames * 4);
    unsigned seed = 12345;
    for (quint32 n = 0; n < frames; ++n) {
        for (int c = 0; c < 2; ++c) {
            seed = seed * 1664525u + 1013904223u;
            const double noise = (double(seed >> 8) / double(1u << 24) - 0.5) * 0.3;
            const double tone
                = 0.4 * std::sin(2.0 * std::numbers::pi * 220.0 * double(n) / 48000.0);
            u16(quint16(qint16(std::lround((tone + noise) * 32767.0))));
        }
    }
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

EffectInstance fx(TimelineData& tl, const char* id)
{
    return EffectRegistry::instance().createEffect(QLatin1String(id), tl.allocateId());
}

} // namespace

// Counting heap allocations of this binary (the mix thread's allocations are what matters).
void* operator new(std::size_t size)
{
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept
{
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}

class TestAudioPerf : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_wav;

    struct Result
    {
        double medianMs = 0, p99Ms = 0, maxMs = 0;
        double allocsPerChunk = 0;
    };

    Result run(int tracks, const std::vector<const char*>& chain)
    {
        TimelineData tl = emptyTimeline();
        for (int t = 0; t < tracks; ++t) {
            ClipData c = mediaClip(tl, kMedia, 0, 250, 0, 10'000'000);
            for (const char* id : chain)
                c.effects.push_back(fx(tl, id));
            addClip(tl, TrackType::Audio, t, c);
        }
        auto map = std::make_shared<MediaPathMap>();
        map->insert(kMedia, m_wav);
        AudioMixer mixer;
        mixer.setMediaPaths(map);
        std::vector<float> out(2 * kChunk);
        int64_t pos = 0;
        for (int i = 0; i < 20; ++i, pos += kChunk) // warm-up: decoders open, state created
            mixer.mix(tl, pos, kChunk, out.data());
        std::vector<double> ms;
        const long before = g_allocations.load();
        for (int i = 0; i < 300; ++i, pos += kChunk) {
            QElapsedTimer timer;
            timer.start();
            mixer.mix(tl, pos, kChunk, out.data());
            ms.push_back(double(timer.nsecsElapsed()) / 1e6);
        }
        Result r;
        r.allocsPerChunk = double(g_allocations.load() - before) / 300.0;
        std::sort(ms.begin(), ms.end());
        r.medianMs = ms[ms.size() / 2];
        r.p99Ms = ms[ms.size() * 99 / 100];
        r.maxMs = ms.back();
        return r;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_wav = m_dir.filePath("noise.wav");
        QVERIFY(writeNoiseWav(m_wav, 12));
    }

    void chains()
    {
        struct Case
        {
            const char* name;
            int tracks;
            std::vector<const char*> chain;
        };
        const std::vector<Case> cases = {
            {"1 track, no effect", 1, {}},
            {"4 tracks, no effect", 4, {}},
            {"1 track, EQ", 1, {afxid::Eq}},
            {"1 track, compressor", 1, {afxid::Compressor}},
            {"1 track, limiter", 1, {afxid::Limiter}},
            {"1 track, delay", 1, {afxid::Delay}},
            {"1 track, reverb", 1, {afxid::Reverb}},
            {"1 track, EQ+compressor+reverb+limiter", 1,
                {afxid::Eq, afxid::Compressor, afxid::Reverb, afxid::Limiter}},
            {"4 tracks, EQ+compressor+reverb+limiter", 4,
                {afxid::Eq, afxid::Compressor, afxid::Reverb, afxid::Limiter}},
        };
        const double chunkMs = 1000.0 * kChunk / 48000.0;
        for (const Case& c : cases) {
            const Result r = run(c.tracks, c.chain);
            qInfo("%-42s median %.3f ms (%.1f%% of the %.1f ms chunk), p99 %.3f, max %.3f, %.1f "
                  "allocations/chunk",
                c.name, r.medianMs, 100.0 * r.medianMs / chunkMs, chunkMs, r.p99Ms, r.maxMs,
                r.allocsPerChunk);
            QVERIFY2(r.medianMs < chunkMs * 0.5, c.name);
        }
    }
};

QTEST_MAIN(TestAudioPerf)
#include "test_audioperf.moc"
