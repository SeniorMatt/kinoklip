// SPDX-License-Identifier: GPL-3.0-or-later
// Audio after a big copy / paste between timelines (spec 16.7): the mixer that was already
// playing the target timeline must play the pasted clips (also on audio tracks the paste
// created) exactly as a freshly started one does. Sources are float WAV files made here.
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "render/AudioMixer.h"
#include "render/RenderTypes.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <numbers>
#include <vector>

using namespace kk;
namespace ops = kk::editops;

namespace {

constexpr int kRate = 48000;
constexpr double kAmp = 0.25; ///< source sine amplitude (-12 dBFS, below the soft clipper)

/// Writes a 48 kHz stereo float WAV of @p seconds: left = sine(@p hz) * kAmp, right the same
/// unless @p leftOnly.
bool writeWav(const QString& path, double hz, double seconds, bool leftOnly = false)
{
    const int n = int(seconds * kRate);
    QByteArray data;
    data.resize(qsizetype(n) * 8);
    auto* f = reinterpret_cast<float*>(data.data());
    for (int i = 0; i < n; ++i) {
        const float v = float(kAmp * std::sin(2.0 * std::numbers::pi * hz * i / kRate));
        f[size_t(i) * 2] = v;
        f[size_t(i) * 2 + 1] = leftOnly ? 0.0f : v;
    }
    QByteArray h;
    auto u32 = [&h](quint32 v) { h.append(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&h](quint16 v) { h.append(reinterpret_cast<const char*>(&v), 2); };
    h.append("RIFF");
    u32(quint32(36 + data.size()));
    h.append("WAVEfmt ");
    u32(16);
    u16(3); // IEEE float
    u16(2);
    u32(kRate);
    u32(kRate * 8);
    u16(8);
    u16(32);
    h.append("data");
    u32(quint32(data.size()));
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(h) == h.size()
        && file.write(data) == data.size();
}


double energy(const std::vector<float>& s)
{
    double e = 0;
    for (float v : s)
        e += double(v) * v;
    return e;
}

ClipData mediaClip(TimelineData& tl, const QString& media, int64_t start, int64_t duration)
{
    ClipData c;
    c.id = tl.allocateId();
    c.kind = ClipKind::Media;
    c.mediaId = media;
    c.start = start;
    c.duration = duration;
    c.sourceDurationUs = 4'000'000;
    return c;
}

} // namespace

class TestPasteAudio : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    std::shared_ptr<MediaPathMap> m_paths = std::make_shared<MediaPathMap>();

    static std::vector<float> mixFrom(AudioMixer& mixer, const TimelineData& tl, int64_t sample,
        int frames)
    {
        std::vector<float> out(size_t(frames) * 2);
        mixer.mix(tl, sample, frames, out.data());
        return out;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        for (int i = 0; i < 4; ++i) {
            const QString p = m_dir.filePath(QStringLiteral("s%1.wav").arg(i));
            QVERIFY(writeWav(p, 300.0 + 200.0 * i, 4.0));
            m_paths->insert(QStringLiteral("m%1").arg(i), p);
        }
    }

    // Copy many linked clips from a timeline with four audio tracks into one with a single
    // audio track: the paste creates three audio tracks; a mixer that was running the target
    // before the paste plays them like a new one does.
    void pastedAudioIsMixedByARunningMixer()
    {
        TimelineData a = makeDefaultTimeline(QStringLiteral("A"), Rational {25, 1}, 1920, 1080, 4, 4);
        QVector<quint64> all;
        for (int track = 0; track < 4; ++track) {
            for (int k = 0; k < 20; ++k) {
                ClipData v = mediaClip(a, QStringLiteral("m%1").arg(track), k * 10, 10);
                ClipData au = v;
                au.id = a.allocateId();
                v.linkGroup = au.linkGroup = a.allocateId();
                a.videoTracks[size_t(track)].clips.push_back(v);
                a.audioTracks[size_t(track)].clips.push_back(au);
                all << v.id << au.id;
            }
        }
        const ops::Clipboard board = ops::copyClips(a, all);
        QVERIFY(!board.isEmpty());

        TimelineData b = makeDefaultTimeline(QStringLiteral("B"), Rational {25, 1}, 1920, 1080, 1, 1);
        b.audioTracks[0].clips.push_back(mediaClip(b, QStringLiteral("m0"), 0, 10));
        AudioMixer running;
        running.setMediaPaths(m_paths);
        // The target plays before the paste (decoders and channels exist).
        (void)mixFrom(running, b, 0, 4800);

        const QVector<quint64> ids = ops::pasteClips(b, board, 0);
        QCOMPARE(ids.size(), all.size());
        QCOMPARE(int(b.audioTracks.size()), 4);

        for (const int64_t at : {int64_t(0), int64_t(24000), int64_t(48000 * 4)}) {
            AudioMixer fresh;
            fresh.setMediaPaths(m_paths);
            const auto expected = mixFrom(fresh, b, at, 4800);
            const auto got = mixFrom(running, b, at, 4800);
            if (at < 48000 * 6) {
                QVERIFY2(energy(expected) > 1.0, "the fresh mixer plays the paste");
            }
            QVERIFY2(std::abs(energy(got) - energy(expected)) < 1e-6 * (1.0 + energy(expected)),
                qPrintable(QStringLiteral("at %1: running %2, fresh %3")
                               .arg(at).arg(energy(got)).arg(energy(expected))));
        }
    }

    // The mixer plays timeline A, then B whose clips reuse A's clip ids (ids count from 1 in
    // every timeline) on the same media: B sounds as it does on a fresh mixer.
    void switchingTimelinesWithEqualIds()
    {
        auto make = [&](int64_t shift, int tracks) {
            TimelineData t = makeDefaultTimeline(QStringLiteral("T"), Rational {25, 1}, 640, 360, 1, tracks);
            for (int tr = 0; tr < tracks; ++tr) {
                for (int k = 0; k < 6; ++k) {
                    ClipData c = mediaClip(t, QStringLiteral("m%1").arg((k + tr) % 4), k * 12, 12);
                    c.sourceInUs = shift + k * 90000;
                    t.audioTracks[size_t(tr)].clips.push_back(c);
                }
            }
            return t;
        };
        const TimelineData a = make(0, 2);
        const TimelineData b = make(400000, 4);
        AudioMixer running;
        running.setMediaPaths(m_paths);
        for (int64_t at = 0; at < 48000 * 3; at += 4800)
            (void)mixFrom(running, a, at, 4800);
        for (int64_t at = 0; at < 48000 * 3; at += 4800) {
            AudioMixer fresh;
            fresh.setMediaPaths(m_paths);
            const auto expected = mixFrom(fresh, b, at, 4800);
            const auto got = mixFrom(running, b, at, 4800);
            QVERIFY2(std::abs(energy(got) - energy(expected)) < 1e-3 * (1.0 + energy(expected)),
                qPrintable(QStringLiteral("at %1: running %2, fresh %3")
                               .arg(at).arg(energy(got)).arg(energy(expected))));
        }
    }

    // Random clips and pastes (overlaps, partial overwrites): the timeline stays valid and a
    // mixer that played along hears what a fresh one hears.
    void randomPastesStayAudible()
    {
        quint32 seed = 12345;
        auto rnd = [&seed](int n) {
            seed = seed * 1664525u + 1013904223u;
            return int((seed >> 8) % quint32(n));
        };
        for (int round = 0; round < 12; ++round) {
            TimelineData a = makeDefaultTimeline(QStringLiteral("A"), Rational {25, 1}, 1920, 1080, 3, 5);
            TimelineData b = makeDefaultTimeline(QStringLiteral("B"), Rational {25, 1}, 1920, 1080, 1, 1 + rnd(3));
            QVector<quint64> all;
            for (TimelineData* tl : {&a, &b}) {
                for (auto* tracks : {&tl->videoTracks, &tl->audioTracks}) {
                    for (TrackData& tr : *tracks) {
                        int64_t at = rnd(20);
                        for (int k = 0; k < 15; ++k) {
                            const int64_t len = 3 + rnd(30);
                            ClipData c = mediaClip(*tl, QStringLiteral("m%1").arg(rnd(4)), at, len);
                            c.sourceInUs = rnd(1000) * 1000;
                            tr.clips.push_back(c);
                            if (tl == &a)
                                all << c.id;
                            at += len + (rnd(3) == 0 ? rnd(10) : 0);
                        }
                    }
                }
            }
            const ops::Clipboard board = ops::copyClips(a, all);
            AudioMixer running;
            running.setMediaPaths(m_paths);
            (void)mixFrom(running, b, 0, 4800);
            for (int paste = 0; paste < 3; ++paste) {
                const int64_t where = rnd(150);
                QVERIFY(!ops::pasteClips(b, board, where, -1, -1, paste == 2).isEmpty());
                QString why;
                QVERIFY2(ops::checkInvariants(b, &why), qPrintable(why));
                for (const int64_t at : {int64_t(0), int64_t(where * 1920), int64_t(60000)}) {
                    AudioMixer fresh;
                    fresh.setMediaPaths(m_paths);
                    const auto expected = mixFrom(fresh, b, at, 4800);
                    const auto got = mixFrom(running, b, at, 4800);
                    QVERIFY2(std::abs(energy(got) - energy(expected)) < 1e-4 * (1.0 + energy(expected)),
                        qPrintable(QStringLiteral("round %1 paste %2 at %3").arg(round).arg(paste).arg(at)));
                }
            }
        }
    }
};

QTEST_MAIN(TestPasteAudio)
#include "test_pasteaudio.moc"
