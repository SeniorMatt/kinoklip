// SPDX-License-Identifier: GPL-3.0-or-later
// Playback across cuts, measured: long-GOP H.264 clips whose in-points lie far from a keyframe,
// played by the real engine (real Compositor, fake sound card as master clock). For each cut it
// prints how late the first frame after the cut reached the screen, the frames lost around the
// cut and the render thread's decode time for that frame, plus the raw decoder costs (open, seek
// + decode forward). Numbers are medians over all cuts of KK_CUTPERF_RUNS plays (default 3).
//
// Not an assertion test (the machine may be busy): tests/playback/test_cutlookahead.cpp holds the
// deterministic checks. Media is generated once into KK_CUTPERF_DIR (default: a temporary
// folder); takes a minute at 1080p. KK_CUTPERF_SIZE=1280x720 for a smaller picture.
#include "../media/MediaTestUtil.h"
#include "PlaybackTestUtil.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "media/AudioDecoder.h"
#include "media/VideoDecoder.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <map>
#include <vector>

using namespace kk;

namespace {

constexpr int kFps = 60;
constexpr int kGop = 250; ///< frames between keyframes in the generated files
constexpr int kOffset = 230; ///< in-point: frames past a keyframe (far from it)
constexpr int kFileSeconds = 40;

double median(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double maximum(const std::vector<double>& v)
{
    return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end());
}

/// Generates @p name (fixed GOP, moving test picture, sine sound) in @p dir unless it exists.
bool makeLongGop(const QString& dir, const QString& name, const QString& source)
{
    const QString path = dir + QLatin1Char('/') + name;
    if (QFile::exists(path))
        return true;
    const QString size = qEnvironmentVariable("KK_CUTPERF_SIZE", QStringLiteral("1920x1080"));
    QProcess ffmpeg;
    ffmpeg.setProcessChannelMode(QProcess::ForwardedChannels);
    ffmpeg.start(QStringLiteral("ffmpeg"),
        {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
            QStringLiteral("%1=s=%2:r=%3:d=%4").arg(source, size).arg(kFps).arg(kFileSeconds), "-f",
            "lavfi", "-i", QStringLiteral("sine=frequency=440:d=%1").arg(kFileSeconds), "-c:v",
            "libx264", "-preset", "veryfast", "-crf", "20", "-g", QString::number(kGop),
            "-keyint_min", QString::number(kGop), "-sc_threshold", "0", "-pix_fmt", "yuv420p",
            "-c:a", "aac", "-shortest", path + ".tmp.mp4"});
    if (!ffmpeg.waitForFinished(10 * 60 * 1000) || ffmpeg.exitCode() != 0)
        return false;
    return QFile::rename(path + ".tmp.mp4", path);
}

struct Scenario
{
    int clips = 16;
    int clipFrames = 90;
    bool otherFile = false;
    int offset = kOffset; ///< in-point: frames past a keyframe
};

/// V1 + A1: @p s.clips clips one after the other, each starting kOffset frames after a keyframe
/// of a different GOP. @return the frames of the cuts.
std::vector<int64_t> buildTimeline(Project& project, const QString& a, const QString& b, Scenario s)
{
    const ImportResult imported = project.importMedia({a, b});
    Q_ASSERT(imported.importedIds.size() == 2);
    const int gops = kFileSeconds * kFps / kGop - 1;
    project.beginEdit(QStringLiteral("cuts"));
    TimelineData& tl = project.timeline()->mutableData();
    const media::MediaInfo& info = project.mediaPool()->item(imported.importedIds[0])->info;
    tl = makeDefaultTimeline(
        QStringLiteral("cuts"), {kFps, 1}, info.displayWidth, info.displayHeight, 2, 2);
    std::vector<int64_t> cuts;
    int64_t at = 0;
    for (int i = 0; i < s.clips; ++i) {
        ClipData c;
        c.id = tl.allocateId();
        c.mediaId = imported.importedIds[s.otherFile ? i % 2 : 0];
        c.start = at;
        c.duration = s.clipFrames;
        c.sourceDurationUs = info.durationUs;
        // A different GOP each time, not in order, so nothing is reachable by decoding forward.
        const int gop = (i * 5) % gops;
        c.sourceInUs = framesToUs((gop * kGop + s.offset), Rational {kFps, 1});
        c.linkGroup = quint64(i + 1);
        tl.videoTracks[0].clips.push_back(c);
        c.id = tl.allocateId();
        tl.audioTracks[0].clips.push_back(c);
        if (i > 0)
            cuts.push_back(at);
        at += s.clipFrames;
    }
    project.endEdit();
    return cuts;
}

struct Shown
{
    int64_t frame = 0;
    double atMs = 0.0;
    double decodeMs = 0.0;
    double uploadMs = 0.0;
    double renderMs = 0.0;
};

} // namespace

class TestCutPerf : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("no ffmpeg");
        m_dir = qEnvironmentVariable("KK_CUTPERF_DIR");
        if (m_dir.isEmpty())
            m_dir = m_temp.path();
        QDir().mkpath(m_dir);
        QVERIFY(makeLongGop(m_dir, QStringLiteral("gop250_a.mp4"), QStringLiteral("testsrc2")));
        QVERIFY(makeLongGop(m_dir, QStringLiteral("gop250_b.mp4"), QStringLiteral("testsrc")));
    }

    /// What one jump costs when done synchronously: open, seek + decode forward.
    void decoderCosts()
    {
        const QString path = m_dir + QStringLiteral("/gop250_a.mp4");
        std::vector<double> open, far, near, step, audioOpen;
        for (int i = 0; i < 7; ++i) {
            QElapsedTimer t;
            t.start();
            media::VideoDecoder d;
            QVERIFY(d.open(path));
            open.push_back(t.nsecsElapsed() / 1e6);
            const int gop = 1 + i % 5;
            t.restart();
            QVERIFY(d.decodeFrame(framesToUs(gop * kGop + kOffset, Rational {kFps, 1})));
            far.push_back(t.nsecsElapsed() / 1e6);
            t.restart();
            for (int k = 1; k <= 30; ++k)
                QVERIFY(d.decodeFrame(framesToUs(gop * kGop + kOffset + k, Rational {kFps, 1})));
            step.push_back(t.nsecsElapsed() / 1e6 / 30.0);
            t.restart();
            QVERIFY(d.decodeFrame(framesToUs((gop + 1) * kGop + 2, Rational {kFps, 1})));
            near.push_back(t.nsecsElapsed() / 1e6);
            t.restart();
            media::AudioDecoder a;
            QVERIFY(a.open(path));
            std::vector<float> samples(2048 * 2);
            a.read(48000 * (i + 3), samples.data(), 2048);
            audioOpen.push_back(t.nsecsElapsed() / 1e6);
        }
        qInfo("decoder (median of 7, ms): open + first frame %.1f; seek + decode forward %d frames "
              "%.1f (max %.1f); seek to just after a keyframe %.1f; one frame forward %.2f; audio "
              "open + seek + 2048 samples %.1f (max %.1f)",
            median(open), kOffset, median(far), maximum(far), median(near), median(step),
            median(audioOpen), maximum(audioOpen));
    }

    void cuts_data()
    {
        QTest::addColumn<int>("clips");
        QTest::addColumn<int>("clipFrames");
        QTest::addColumn<bool>("otherFile");
        QTest::addColumn<int>("offset");
        QTest::newRow("same file, 1.5 s clips") << 12 << 90 << false << kOffset;
        QTest::newRow("two files, 1.5 s clips") << 12 << 90 << true << kOffset;
        QTest::newRow("two files, 0.4 s clips, 100 frames from a keyframe")
            << 24 << 24 << true << 100;
        // Decoding 230 + 24 frames per 0.4 s is more than this machine's decoder can do.
        QTest::newRow("two files, 0.4 s clips, overload") << 24 << 24 << true << kOffset;
    }

    void cuts()
    {
        QFETCH(int, clips);
        QFETCH(int, clipFrames);
        QFETCH(bool, otherFile);
        QFETCH(int, offset);
        Project project;
        const std::vector<int64_t> cutFrames
            = buildTimeline(project, m_dir + QStringLiteral("/gop250_a.mp4"),
                m_dir + QStringLiteral("/gop250_b.mp4"), Scenario {clips, clipFrames, otherFile, offset});
        const int runs = qEnvironmentVariableIntValue("KK_CUTPERF_RUNS") > 0
            ? qEnvironmentVariableIntValue("KK_CUTPERF_RUNS")
            : 3;
        std::vector<double> late, lost, freeze, decode, upload, plain;
        int64_t underruns = 0;
        for (int run = 0; run < runs; ++run) {
            auto* audio = new test::FakeAudioOutput(10, 40);
            PlaybackEngine::Options options;
            options.audio.reset(audio);
            PlaybackEngine engine(std::move(options));
            engine.setMediaPool(project.mediaPool());
            engine.setTimeline(project.timeline());
            int client = 0;
            engine.setOutputClient(&client, QSize(1000, 562), engine.compositor()->isGpu());
            QVERIFY(QTest::qWaitFor([&] { return engine.currentFrame() != nullptr; }, 10000));
            std::vector<Shown> shown;
            QElapsedTimer clock;
            clock.start();
            QObject::connect(
                &engine, &PlaybackEngine::frameReady, &engine, [&](const RenderedFramePtr& f) {
                    if (f && f->ok)
                        shown.push_back({f->frame, clock.nsecsElapsed() / 1e6, f->decodeMs,
                            f->uploadMs, f->renderMs});
                });
            const int64_t end = project.timeline()->snapshot()->duration();
            engine.play();
            QVERIFY(QTest::qWaitFor([&] { return !engine.isPlaying(); },
                int(end * 1000 / kFps) + 15000));
            underruns += audio->underrunFrames();
            const render::DecoderPool::PrepareStats ps = engine.compositor()->prepareStats();
            qInfo("look-ahead: started %lld (%lld on idle decoders), ready %lld, hits ready %lld, "
                  "hits waited %lld, cancelled %lld, expired %lld, peak reserved %d, running %d",
                (long long)ps.started, (long long)ps.reused, (long long)ps.ready,
                (long long)ps.hitsReady, (long long)ps.hitsWaited, (long long)ps.cancelled,
                (long long)ps.expired, ps.peakReserved, ps.peakRunning);

            // The presentation offset of ordinary frames (the frame before each cut region) is
            // "on time"; due(f) = offset + f / fps.
            std::vector<double> offsets;
            for (const Shown& s : shown) {
                bool nearCut = s.frame < 30;
                for (const int64_t c : cutFrames)
                    nearCut = nearCut || (s.frame >= c - 5 && s.frame < c + 20);
                if (!nearCut)
                    offsets.push_back(s.atMs - double(s.frame) * 1000.0 / kFps);
            }
            if (offsets.empty()) // very short clips: everything is near a cut
                for (const Shown& s : shown)
                    offsets.push_back(s.atMs - double(s.frame) * 1000.0 / kFps);
            std::sort(offsets.begin(), offsets.end());
            const double offset = offsets[offsets.size() / 10]; // on-time presentation, not the median

            for (const int64_t c : cutFrames) {
                const auto first = std::find_if(
                    shown.begin(), shown.end(), [&](const Shown& s) { return s.frame >= c; });
                if (first == shown.end())
                    continue;
                late.push_back(first->atMs - (offset + double(c) * 1000.0 / kFps));
                decode.push_back(first->decodeMs);
                upload.push_back(first->uploadMs);
                // Frames of [c - 4, c + 16) that never reached the screen.
                const int64_t windowEnd = std::min<int64_t>(c + 16, end);
                int seen = 0;
                double longest = 0.0;
                double previous = -1.0;
                for (const Shown& s : shown) {
                    if (s.frame >= c - 4 && s.frame < windowEnd) {
                        ++seen;
                        if (previous >= 0.0)
                            longest = std::max(longest, s.atMs - previous);
                        previous = s.atMs;
                    }
                }
                lost.push_back(double(windowEnd - (c - 4) - seen));
                freeze.push_back(longest);
            }
            for (const Shown& s : shown) {
                bool nearCut = false;
                for (const int64_t c : cutFrames)
                    nearCut = nearCut || (s.frame >= c - 2 && s.frame < c + 4);
                if (!nearCut)
                    plain.push_back(s.decodeMs);
            }
        }
        const double on2 = double(std::count_if(late.begin(), late.end(), [](double v) {
            return v <= 2000.0 / kFps;
        })) / double(std::max<size_t>(1, late.size())) * 100.0;
        qInfo("%s: %d cuts x %d plays. Cut frame late: median %.1f ms, max %.1f; on time (<= 2 "
              "frames) %.0f%%. Lost frames in the 20 around a cut: median %.0f, max %.0f. Longest "
              "picture hold: median %.1f ms, max %.1f. Render-thread decode of the first frame "
              "after a cut: median %.1f ms, max %.1f (upload %.1f); of other frames %.2f. Audio "
              "underrun: %lld frames",
            QTest::currentDataTag(), int(cutFrames.size()), runs, median(late), maximum(late), on2,
            median(lost), maximum(lost), median(freeze), maximum(freeze), median(decode),
            maximum(decode), median(upload), median(plain), (long long)(underruns));
    }

private:
    QTemporaryDir m_temp;
    QString m_dir;
};

QTEST_MAIN(TestCutPerf)
#include "test_cutperf.moc"
