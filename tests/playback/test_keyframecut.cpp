// SPDX-License-Identifier: GPL-3.0-or-later
// Playing across a cut into a clip with animated Transform (zoom + rotation keyed from its
// first frame), measured against the same cut without keys and against starting on the clip
// (spec 72). Two clips cut from ONE screen-recording-like file, one after the other, played by
// the real engine (real Compositor, fake sound card as master clock).
//
// Per case it prints the frames of the keyed clip's first second that never reached the screen
// and the longest hold between two shown frames. The assertions compare cases with each other
// (generous margins; the machine may be busy and may have no GPU), never absolute speed.
// KK_KEYCUT_SIZE=1920x1080 for the owner's picture size (default 1280x720).
#include "PlaybackTestUtil.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <vector>

using namespace kk;

namespace {

const int kFps = qEnvironmentVariableIntValue("KK_KEYCUT_FPS") > 0
    ? qEnvironmentVariableIntValue("KK_KEYCUT_FPS")
    : 30;
constexpr int kGop = 250;
constexpr int kFileSeconds = 30;
constexpr int kClipFrames = 90; ///< 3 s
constexpr int kLead = 30; ///< frames played in the previous clip before the cut

bool makeScreenFile(const QString& path, const QString& source)
{
    if (QFile::exists(path))
        return true;
    const QString size = qEnvironmentVariable("KK_KEYCUT_SIZE", QStringLiteral("1280x720"));
    QProcess ffmpeg;
    ffmpeg.setProcessChannelMode(QProcess::ForwardedChannels);
    ffmpeg.start(QStringLiteral("ffmpeg"),
        {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
            QStringLiteral("%1=s=%2:r=%3:d=%4").arg(source, size).arg(kFps).arg(kFileSeconds), "-f",
            "lavfi", "-i", QStringLiteral("sine=frequency=440:d=%1").arg(kFileSeconds), "-c:v",
            "libx264", "-preset", "veryfast", "-crf", "20", "-g", QString::number(kGop),
            "-keyint_min", QString::number(kGop), "-sc_threshold", "0", "-pix_fmt", "yuv420p",
            "-c:a", "aac", "-shortest", path + ".tmp.mp4"});
    if (!ffmpeg.waitForFinished(5 * 60 * 1000) || ffmpeg.exitCode() != 0)
        return false;
    return QFile::rename(path + ".tmp.mp4", path);
}

struct Result
{
    double lost = 0; ///< frames of the keyed clip's first second never shown
    double hold = 0; ///< longest gap between shown frames there (ms)
    double lateMs = 0; ///< how late the cut frame was shown
    render::DecoderPool::PrepareStats look;
};

struct Case
{
    bool keyed = true;
    bool otherFile = false;
    bool startOnClip = false;
    int gapFrames = 120; ///< source gap between the two clips (0 = contiguous)
};

/// V1/A1: clip 0 (lead-in) then clip 1 (maybe keyed) cut from the same (or the other) file.
void buildTimeline(Project& project, const QString& a, const QString& b, const Case& c)
{
    const ImportResult imported = project.importMedia({a, b});
    Q_ASSERT(imported.importedIds.size() == 2);
    project.beginEdit(QStringLiteral("keycut"));
    TimelineData& tl = project.timeline()->mutableData();
    const media::MediaInfo& info = project.mediaPool()->item(imported.importedIds[0])->info;
    tl = makeDefaultTimeline(
        QStringLiteral("keycut"), {kFps, 1}, info.displayWidth, info.displayHeight, 2, 2);
    for (int i = 0; i < 2; ++i) {
        ClipData clip;
        clip.id = tl.allocateId();
        clip.mediaId = imported.importedIds[(c.otherFile && i == 0) ? 1 : 0];
        clip.start = i * kClipFrames;
        clip.duration = kClipFrames;
        clip.sourceDurationUs = info.durationUs;
        const int sourceFrame = 40 + i * (kClipFrames + c.gapFrames);
        clip.sourceInUs = framesToUs(sourceFrame, Rational {kFps, 1});
        clip.linkGroup = quint64(i + 1);
        if (i == 1 && c.keyed) {
            // Zoom 1.39 -> 1.75 -> 1.59 -> 1.39 and rotation 0 -> 9.2 -> 6.4 -> 0, from frame 0.
            const int at[] = {0, 20, 40, 60};
            const double zoom[] = {1.39, 1.75, 1.59, 1.39};
            const double rot[] = {0, 9.2, 6.4, 0};
            for (int k = 0; k < 4; ++k) {
                clip.transform.animated(keys::ZoomX, 1.0).setKey(at[k], zoom[k]);
                clip.transform.animated(keys::ZoomY, 1.0).setKey(at[k], zoom[k]);
                clip.transform.animated(keys::Rotation, 0.0).setKey(at[k], rot[k]);
            }
        }
        tl.videoTracks[0].clips.push_back(clip);
        clip.id = tl.allocateId();
        tl.audioTracks[0].clips.push_back(clip);
    }
    project.endEdit();
}

} // namespace

class TestKeyframeCut : public QObject
{
    Q_OBJECT

    Result play(const Case& c)
    {
        Project project;
        buildTimeline(project, m_dir + QStringLiteral("/screen_a.mp4"),
            m_dir + QStringLiteral("/screen_b.mp4"), c);
        auto* audio = new test::FakeAudioOutput(10, 40);
        PlaybackEngine::Options options;
        options.audio.reset(audio);
        PlaybackEngine engine(std::move(options));
        engine.setMediaPool(project.mediaPool());
        engine.setTimeline(project.timeline());
        int client = 0;
        engine.setOutputClient(&client, QSize(1000, 562), engine.compositor()->isGpu());
        engine.seek(c.startOnClip ? kClipFrames : kClipFrames - kLead);
        QTest::qWait(1000); // the paused pre-roll
        struct Shown
        {
            int64_t frame;
            double atMs;
        };
        std::vector<Shown> shown;
        QElapsedTimer clock;
        clock.start();
        QObject::connect(
            &engine, &PlaybackEngine::frameReady, &engine, [&](const RenderedFramePtr& f) {
                if (f && f->ok)
                    shown.push_back({f->frame, clock.nsecsElapsed() / 1e6});
            });
        const int64_t endFrame = kClipFrames + kFps + 10;
        engine.play();
        (void)QTest::qWaitFor(
            [&] { return engine.currentFrame() && engine.position() >= endFrame; }, 20000);
        engine.pause();
        Result r;
        // First shown frame of the window is "on time" by definition; later ones are compared
        // with the ideal schedule from it.
        double previous = -1.0;
        int seen = 0;
        double first = -1.0;
        for (const Shown& s : shown) {
            if (s.frame < kClipFrames || s.frame >= kClipFrames + kFps)
                continue;
            if (first < 0)
                first = s.atMs - double(s.frame - kClipFrames) * 1000.0 / kFps;
            ++seen;
            if (previous >= 0)
                r.hold = std::max(r.hold, s.atMs - previous);
            previous = s.atMs;
        }
        r.lost = kFps - seen;
        // Gap across the cut itself.
        for (size_t i = 1; i < shown.size(); ++i) {
            if (shown[i].frame >= kClipFrames && shown[i - 1].frame < kClipFrames)
                r.lateMs = shown[i].atMs - shown[i - 1].atMs;
        }
        r.look = engine.compositor()->prepareStats();
        return r;
    }

    void report(const char* name, const Case& c)
    {
        std::vector<Result> runs;
        for (int i = 0; i < 2; ++i)
            runs.push_back(play(c));
        std::sort(runs.begin(), runs.end(),
            [](const Result& a, const Result& b) { return a.lost < b.lost; });
        const Result& m = runs[0]; // the better of two: noise only ever adds losses
        qInfo("%s: lost %.0f of %d frames in the first second of the clip (runs: %.0f %.0f); "
              "longest hold %.1f ms; gap across the cut %.1f ms; look-ahead started "
              "%lld, ready %lld, hits ready %lld, waited %lld, cancelled %lld, expired %lld",
            name, m.lost, kFps, runs[0].lost, runs[1].lost, m.hold, m.lateMs,
            (long long)m.look.started, (long long)m.look.ready, (long long)m.look.hitsReady,
            (long long)m.look.hitsWaited, (long long)m.look.cancelled, (long long)m.look.expired);
        m_last = m;
    }

private Q_SLOTS:
    void initTestCase()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("no ffmpeg");
        QVERIFY(m_temp.isValid());
        m_dir = m_temp.path();
        QVERIFY(
            makeScreenFile(m_dir + QStringLiteral("/screen_a.mp4"), QStringLiteral("testsrc2")));
        QVERIFY(makeScreenFile(m_dir + QStringLiteral("/screen_b.mp4"), QStringLiteral("testsrc")));
    }

    void crossing_data()
    {
        QTest::addColumn<int>("gap");
        QTest::newRow("jump of 4 s in the file") << 120;
        QTest::newRow("contiguous") << 0;
        QTest::newRow("gap 20 frames") << 20;
    }

    void crossing()
    {
        QFETCH(int, gap);
        report("a keyed, start on the clip", Case {true, false, true, gap});
        const Result startOn = m_last;
        report("b keyed, start in the previous clip, same file", Case {true, false, false, gap});
        const Result keyed = m_last;
        report("c plain, start in the previous clip, same file", Case {false, false, false, gap});
        const Result plain = m_last;
        report("d keyed, start in the previous clip, other file", Case {true, true, false, gap});
        const Result other = m_last;
        // The keys must not make crossing worse than the same cut without keys, nor than a
        // cut between files, nor than starting on the clip (margins: noise on a busy machine).
        QVERIFY2(keyed.lost <= plain.lost + 3, "keys make the crossing lose frames");
        QVERIFY2(keyed.lost <= other.lost + 3, "same file loses more than another file");
        QVERIFY2(keyed.lost <= startOn.lost + 3, "crossing loses more than starting on the clip");
        QVERIFY2(keyed.hold <= std::max(plain.hold, startOn.hold) + 80.0,
            "a long hold only when crossing into the keyed clip");
    }

private:
    QTemporaryDir m_temp;
    QString m_dir;
    Result m_last;
};

QTEST_MAIN(TestKeyframeCut)
#include "test_keyframecut.moc"
