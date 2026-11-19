// SPDX-License-Identifier: GPL-3.0-or-later
// Smooth playback means every source frame is shown once, at its time: a 23.976 fps clip played
// at speed must put (almost) only new pictures on screen. The "fps" of the playback statistics
// counts presentations, which hides a decoder that delivers 16 new pictures a second and has the
// presenter show some of them twice; this test counts the distinct source pictures
// (PlaybackEngine::Stats::uniqueFrames, RenderedFrame::sourceId) instead.
//
// Real time: it plays for several seconds on the sound clock of a stand-in sound card (like a
// real session), so it belongs to the release group. Settings (environment):
//   KK_UNIQUE_FILE=<clip>   play this clip instead of the generated one (fps is read from it)
//   KK_UNIQUE_HW=1          allow hardware decoding (otherwise software, KINOKLIP_HWACCEL=off)
//   KK_UNIQUE_SECONDS=<n>   playing time measured (default 5)
//   KK_UNIQUE_TL=WxH        timeline size (default 640x360; a 4K clip on a 4K timeline needs a GPU)
//   KK_UNIQUE_QUALITY=0..   leave Full quality for Half (1) / Quarter (2) viewer resolution
#include "../render/RenderTestUtil.h"
#include "core/EditOps.h"
#include "core/Project.h"
#include "media/DecodeStats.h"
#include "media/VideoDecoder.h"
#include "playback/AudioOutput.h"
#include "playback/AudioRing.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"

#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

using namespace kk;
using namespace kk::test;

namespace {

/// Sound card stand-in: pulls 10 ms periods in real time and reports how much it played, so the
/// clock of the engine is the audio clock (as with a real device).
class ClockOutput final : public AudioOutput
{
public:
    ~ClockOutput() override { stop(); }
    bool isAvailable() const override { return true; }
    QString description() const override { return QStringLiteral("test clock"); }
    void setVolume(float) override { }
    bool start(AudioRing* ring) override
    {
        stop();
        m_ring = ring;
        m_startUs = monotonicUs();
        m_played = 0;
        m_quit = false;
        m_thread = std::thread([this] { run(); });
        return true;
    }
    void stop() override
    {
        if (m_thread.joinable()) {
            m_quit = true;
            m_thread.join();
        }
    }
    void playedFrames(int64_t* frames, int64_t* atUs) const override
    {
        *frames = m_played.load();
        *atUs = m_at.load();
    }

private:
    void run()
    {
        std::vector<float> buffer;
        int64_t pulled = 0;
        while (!m_quit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const int64_t now = monotonicUs();
            const int64_t target = (now - m_startUs) * 48 / 1000;
            if (target > pulled) {
                buffer.assign(size_t(target - pulled) * 2, 0.0f);
                m_ring->read(buffer.data(), size_t(target - pulled));
                pulled = target;
            }
            m_played = pulled;
            m_at = now;
        }
    }
    AudioRing* m_ring = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_quit {false};
    std::atomic<int64_t> m_played {0};
    std::atomic<int64_t> m_at {0};
    int64_t m_startUs = 0;
};

struct Result
{
    PlaybackEngine::Stats stats;
    double expected = 0.0; ///< new pictures a perfect playback shows in the measured time
    QString backend;
    QString decoder;
};

} // namespace

class TestUniqueFrames : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_clip;
    int64_t m_clipFrames = 0;

    /// Plays the clip for the measured time on a timeline of @p fps.
    Result play(Rational fps, double seconds)
    {
        Project project;
        const QString id = project.importMedia({m_clip}).importedIds.value(0);
        Q_ASSERT(!id.isEmpty());
        const MediaItem item = *project.mediaPool()->item(id);
        const QStringList size = qEnvironmentVariable("KK_UNIQUE_TL", "640x360").split('x');
        project.beginEdit(QStringLiteral("unique"));
        TimelineData& t = project.timeline()->mutableData();
        t.width = size.value(0).toInt();
        t.height = size.value(1).toInt();
        t.fps = fps;
        const int64_t frames = int64_t(std::floor(item.info.durationUs * 1e-6 * fps.toDouble()));
        editops::overwriteClip(t, {TrackType::Video, 0},
            mediaClip(t, id, 0, frames, 0, item.info.durationUs));
        project.endEdit();

        PlaybackEngine::Options options;
        options.audio = std::make_unique<ClockOutput>();
        PlaybackEngine engine(std::move(options));
        engine.setMediaPool(project.mediaPool());
        engine.setTimeline(project.timeline());
        int client = 0;
        engine.setOutputClient(&client, QSize(1013, 570), engine.compositor()->isGpu());
        engine.setQualityMode(PlaybackEngine::QualityMode::Full);
        if (qEnvironmentVariableIsSet("KK_UNIQUE_QUALITY"))
            engine.setQualityMode(
                PlaybackEngine::QualityMode(qEnvironmentVariableIntValue("KK_UNIQUE_QUALITY")));
        const bool shown
            = QTest::qWaitFor([&] { return engine.currentFrame() != nullptr; }, 30000);
        Q_ASSERT(shown);
        engine.play();
        QTest::qWait(1000); // the decoders open, the queues fill
        engine.resetStats();
        QTest::qWait(int(seconds * 1000));
        Result r;
        r.stats = engine.stats();
        engine.pause();
        r.expected = r.stats.playSeconds * (item.info.frameRate.isValid()
                ? item.info.frameRate.toDouble()
                : fps.toDouble());
        r.backend = engine.compositor()->backendDescription();
        return r;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_uniqueframes"));
        if (qEnvironmentVariableIsEmpty("KK_UNIQUE_HW"))
            qputenv("KINOKLIP_HWACCEL", "off");
        m_clip = qEnvironmentVariable("KK_UNIQUE_FILE");
        if (m_clip.isEmpty()) {
            QVERIFY(m_dir.isValid());
            m_clip = m_dir.filePath(QStringLiteral("clip_2398.mov"));
            QProcess ff;
            ff.start(QStringLiteral("ffmpeg"),
                {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
                    "testsrc2=size=640x360:rate=24000/1001", "-f", "lavfi", "-i",
                    "sine=frequency=440:sample_rate=48000", "-t", "12", "-c:v", "libx264",
                    "-preset", "ultrafast", "-g", "48", "-pix_fmt", "yuv420p", "-c:a", "pcm_s16le",
                    m_clip});
            if (!ff.waitForFinished(120000) || ff.exitCode() != 0)
                QSKIP("ffmpeg cannot make the test clip");
        }
        media::VideoDecoder probe;
        QVERIFY(probe.open(m_clip));
        qInfo("CLIP %s %dx%d %s %s %.3f fps", qPrintable(m_clip), probe.info().width,
            probe.info().height, qPrintable(probe.info().pixelFormat),
            qPrintable(probe.info().videoCodec), probe.info().frameRate.toDouble());
    }

    /// The decoder alone, asked for one timeline frame after the other at the pace of the clip
    /// (what the render thread does), with zero-copy export when the machine decodes on VAAPI:
    /// every frame must come back once; prints what each call costs.
    void decoderOnly()
    {
        media::VideoDecoder decoder;
        decoder.setZeroCopyGate(std::make_shared<std::atomic<bool>>(true));
        decoder.setZeroCopy(true); // before open(): sizes the surface pool
        QVERIFY(decoder.open(m_clip));
        const Rational fps = decoder.info().frameRate;
        const media::DecodeCounters before = media::decodeCounters();
        const int64_t frames = std::min<int64_t>(
            120, int64_t(decoder.info().durationUs * 1e-6 * fps.toDouble()) - 2);
        int64_t unique = 0, last = -1;
        double worstMs = 0.0, totalMs = 0.0;
        QElapsedTimer pace;
        pace.start();
        for (int64_t k = 0; k < frames; ++k) {
            QElapsedTimer call;
            call.start();
            const auto frame = decoder.decodeFrame(framesToUs(k, fps));
            const double ms = call.nsecsElapsed() / 1e6;
            worstMs = std::max(worstMs, ms);
            totalMs += ms;
            QVERIFY(frame);
            unique += frame->ptsUs != last ? 1 : 0;
            last = frame->ptsUs;
            // The next frame is asked for when it is due.
            const double due = double(k + 1) * 1000.0 * fps.den / fps.num;
            while (pace.nsecsElapsed() / 1e6 < due)
                std::this_thread::yield();
        }
        const media::DecodeCounters after = media::decodeCounters();
        qInfo("DECODER %s: %lld frames asked, %lld unique, %.1f ms per call (worst %.1f), %lld "
              "zero-copy exports (%.1f ms each), %lld downloaded, %lld software",
            qPrintable(media::hwAccelName(decoder.activeHwAccel())), qlonglong(frames),
            qlonglong(unique), totalMs / double(frames), worstMs,
            qlonglong(after.zeroCopyFrames - before.zeroCopyFrames),
            after.zeroCopyFrames > before.zeroCopyFrames
                ? double(after.zeroCopyUs - before.zeroCopyUs)
                    / double(after.zeroCopyFrames - before.zeroCopyFrames) / 1000.0
                : 0.0,
            qlonglong(after.downloadedFrames - before.downloadedFrames),
            qlonglong(after.softwareFrames - before.softwareFrames));
        QVERIFY2(unique >= frames * 99 / 100,
            qPrintable(QStringLiteral("%1 unique of %2").arg(unique).arg(frames)));
    }

    /// The clip's own rate on a timeline of the same rate: every frame once.
    void sourceRateTimeline()
    {
        media::VideoDecoder probe;
        QVERIFY(probe.open(m_clip));
        const Result r = play(probe.info().frameRate, qEnvironmentVariable("KK_UNIQUE_SECONDS", "5").toDouble());
        const PlaybackEngine::Stats& s = r.stats;
        qInfo("RESULT %s | %.1f s: %lld presented (%.2f fps), %lld unique (%.2f/s of %.2f), "
              "%lld repeated, %lld dropped, render %.1f ms [decode %.1f, upload %.1f, composite "
              "%.1f, output %.1f]",
            qPrintable(r.backend), s.playSeconds, qlonglong(s.presented), s.fps(),
            qlonglong(s.uniqueFrames), s.uniqueFrames / s.playSeconds,
            r.expected / s.playSeconds, qlonglong(s.repeatedFrames), qlonglong(s.dropped),
            s.averageRenderMs, s.averageDecodeMs, s.averageUploadMs, s.averageCompositeMs,
            s.averageOutputMs);
        QVERIFY(s.presented > 0);
        // Distinct pictures at least 99 % of what a perfect playback shows (this also catches
        // frames that were never shown) ...
        QVERIFY2(double(s.uniqueFrames) >= 0.99 * r.expected,
            qPrintable(QStringLiteral("%1 unique pictures of %2 expected").arg(s.uniqueFrames).arg(r.expected)));
        // ... and of the frames put on screen (this catches pictures shown twice).
        QVERIFY2(double(s.uniqueFrames) >= 0.99 * double(s.presented),
            qPrintable(QStringLiteral("%1 unique of %2 presented, %3 repeated")
                    .arg(s.uniqueFrames).arg(s.presented).arg(s.repeatedFrames)));
    }

    /// 23.976 fps footage on a 24 fps timeline (the usual project): a repeat every 1001 frames
    /// at most.
    void clipOnTimeline24()
    {
        media::VideoDecoder probe;
        QVERIFY(probe.open(m_clip));
        if (std::abs(probe.info().frameRate.toDouble() - 24.0) > 0.1)
            QSKIP("the clip is not a 24 fps clip");
        const Result r = play({24, 1}, qEnvironmentVariable("KK_UNIQUE_SECONDS", "5").toDouble());
        const PlaybackEngine::Stats& s = r.stats;
        qInfo("RESULT 24 fps timeline %s | %.1f s: %lld presented, %lld unique (%.2f/s), %lld "
              "repeated, %lld dropped",
            qPrintable(r.backend), s.playSeconds, qlonglong(s.presented), qlonglong(s.uniqueFrames),
            s.uniqueFrames / s.playSeconds, qlonglong(s.repeatedFrames), qlonglong(s.dropped));
        QVERIFY(s.presented > 0);
        QVERIFY2(double(s.uniqueFrames) >= 0.99 * double(s.presented),
            qPrintable(QStringLiteral("%1 unique of %2 presented, %3 repeated")
                    .arg(s.uniqueFrames).arg(s.presented).arg(s.repeatedFrames)));
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestUniqueFrames>(argc, argv);
}

#include "test_uniqueframes.moc"
