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
#include "render/DecoderPool.h"

#include <QDir>
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
    : 60;
const int kGop = qEnvironmentVariableIntValue("KK_KEYCUT_GOP") > 0
    ? qEnvironmentVariableIntValue("KK_KEYCUT_GOP")
    : 600;
constexpr int kFileSeconds = 30;
const int kClipFrames = 3 * kFps; ///< 3 s
const int kLead
    = 2 * kFps + kFps / 2; ///< frames played in the previous clip before the cut (2.5 s)

bool makeScreenFile(const QString& path, const QString& source)
{
    if (QFile::exists(path))
        return true;
    const QString size = qEnvironmentVariable("KK_KEYCUT_SIZE", QStringLiteral("1920x1080"));
    QProcess ffmpeg;
    ffmpeg.setProcessChannelMode(QProcess::ForwardedChannels);
    ffmpeg.start(QStringLiteral("ffmpeg"),
        {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
            QStringLiteral("%1=s=%2:r=%3:d=%4").arg(source, size).arg(kFps).arg(kFileSeconds), "-f",
            "lavfi", "-i", QStringLiteral("sine=frequency=440:d=%1").arg(kFileSeconds), "-c:v",
            "libx264", "-preset", "veryfast", "-crf", qEnvironmentVariable("KK_KEYCUT_CRF", "20"),
            "-g", QString::number(kGop), "-keyint_min", QString::number(kGop), "-sc_threshold", "0",
            "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", path + ".tmp.mp4"});
    if (!ffmpeg.waitForFinished(5 * 60 * 1000) || ffmpeg.exitCode() != 0)
        return false;
    return QFile::rename(path + ".tmp.mp4", path);
}

struct Result
{
    double lost = 0; ///< frames of the keyed clip's first second never shown
    double hold = 0; ///< longest gap between shown frames there (ms)
    double lateMs = 0; ///< how late the cut frame was shown
    double decodeAvg = 0; ///< mean decode ms of the frames shown there (render thread)
    double decodeMax = 0;
    int slowDecodes = 0; ///< shown frames there that decoded for more than 4 ms
    render::DecoderPool::PrepareStats look;
};

struct Case
{
    bool keyed = true;
    bool otherFile = false;
    bool startOnClip = false;
    int openDelayMs = 0; ///< hardware-decoder-like slow open() of decoders opened at the cut
    int gapFrames = 2 * kFps; ///< source gap between the two clips (0 = contiguous)
    int sourceB = -1; ///< source frame of the second clip (-1: gapFrames decides)
    int leadFrames = 0; ///< frames played before the cut (0: kLead)
    int keyAt = 0; ///< frame of the clip where the first key sits
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
        const int base = 40 + (c.gapFrames < 0 ? 12 * kFps : 0);
        const int sourceFrame
            = (i == 1 && c.sourceB >= 0) ? c.sourceB : base + i * (kClipFrames + c.gapFrames);
        clip.sourceInUs = framesToUs(sourceFrame, Rational {kFps, 1});
        clip.linkGroup = quint64(i + 1);
        if (i == 1 && c.keyed) {
            // Zoom 1.39 -> 1.75 -> 1.59 -> 1.39 and rotation 0 -> 9.2 -> 6.4 -> 0, from frame 0.
            const int at[] = {c.keyAt, c.keyAt + 20, c.keyAt + 40, c.keyAt + 60};
            const double zoom[] = {1.39, 1.75, 1.59, 1.39};
            const double rot[] = {0, 9.2, 6.4, 0};
            for (int k = 0; k < 4; ++k) {
                clip.transform.animated(keys::ZoomX, 1.0).setKey(at[k], zoom[k]);
                clip.transform.animated(keys::ZoomY, 1.0).setKey(at[k], zoom[k]);
                clip.transform.animated(keys::Rotation, 0.0).setKey(at[k], rot[k]);
                clip.transform.animated(keys::PosX, 0.0).setKey(at[k], 40.0 * k);
                clip.transform.animated(keys::PosY, 0.0).setKey(at[k], -20.0 * k);
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
        engine.setQualityMode(PlaybackEngine::QualityMode::Half); // the owner's 691x389
        engine.setMediaPool(project.mediaPool());
        engine.setTimeline(project.timeline());
        int client = 0;
        engine.setOutputClient(&client, QSize(1382, 778), engine.compositor()->isGpu());
        engine.seek(
            c.startOnClip ? kClipFrames : kClipFrames - (c.leadFrames ? c.leadFrames : kLead));
        QTest::qWait(1000); // the paused pre-roll
        struct Shown
        {
            int64_t frame;
            double atMs;
            double decodeMs;
        };
        std::vector<Shown> shown;
        QElapsedTimer clock;
        clock.start();
        QObject::connect(
            &engine, &PlaybackEngine::frameReady, &engine, [&](const RenderedFramePtr& f) {
                if (f && f->ok)
                    shown.push_back({f->frame, clock.nsecsElapsed() / 1e6, f->decodeMs});
            });
        const int64_t endFrame = kClipFrames + kFps + 10;
        render::DecoderPool::setOpenDelayMs(c.openDelayMs);
        engine.play();
        (void)QTest::qWaitFor(
            [&] { return engine.currentFrame() && engine.position() >= endFrame; }, 20000);
        engine.pause();
        render::DecoderPool::setOpenDelayMs(0);
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
            r.decodeAvg += s.decodeMs;
            r.decodeMax = std::max(r.decodeMax, s.decodeMs);
            r.slowDecodes += s.decodeMs > 4.0;
            if (previous >= 0)
                r.hold = std::max(r.hold, s.atMs - previous);
            previous = s.atMs;
        }
        r.lost = kFps - seen;
        r.decodeAvg = seen ? r.decodeAvg / seen : 0.0;
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
              "decode avg %.1f max %.1f ms, %d frames over 4 ms; longest hold %.1f ms; gap across "
              "the cut %.1f ms; look-ahead started "
              "%lld, ready %lld, hits ready %lld, waited %lld, cancelled %lld, expired %lld",
            name, m.lost, kFps, runs[0].lost, runs[1].lost, m.decodeAvg, m.decodeMax, m.slowDecodes,
            m.hold, m.lateMs, (long long)m.look.started, (long long)m.look.ready,
            (long long)m.look.hitsReady, (long long)m.look.hitsWaited, (long long)m.look.cancelled,
            (long long)m.look.expired);
        m_last = m;
    }

private Q_SLOTS:
    void initTestCase()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("no ffmpeg");
        QVERIFY(m_temp.isValid());
        // KK_KEYCUT_DIR keeps the generated files between runs (they take a minute to encode).
        m_dir = qEnvironmentVariable("KK_KEYCUT_DIR", m_temp.path());
        QDir().mkpath(m_dir);
        QVERIFY(
            makeScreenFile(m_dir + QStringLiteral("/screen_a.mp4"), QStringLiteral("testsrc2")));
        QVERIFY(makeScreenFile(m_dir + QStringLiteral("/screen_b.mp4"), QStringLiteral("testsrc")));
    }

    void crossing_data()
    {
        QTest::addColumn<int>("gap");
        QTest::newRow("jump of 4 s in the file") << 2 * kFps;
        QTest::newRow("contiguous") << 0;
        QTest::newRow("jump back 2 s in the file") << -(3 * kFps + 2 * kFps);
    }

    void crossing()
    {
        QFETCH(int, gap);
        report("a keyed, start on the clip", Case {true, false, true, 0, gap});
        const Result startOn = m_last;
        report("b keyed, start in the previous clip, same file", Case {true, false, false, 0, gap});
        const Result keyed = m_last;
        report(
            "c plain, start in the previous clip, same file", Case {false, false, false, 0, gap});
        const Result plain = m_last;
        report("d keyed, start in the previous clip, other file", Case {true, true, false, 0, gap});
        const Result other = m_last;
        // The keys must not make crossing worse than a cut between files, nor than starting on
        // the clip (margins: noise on a busy machine). Keyed against plain is not compared by
        // lost frames: the CPU fallback draws a zoomed 1080p frame slower than a plain one, so
        // that number measures the compositor, not the cut. The cut itself must be prepared and
        // shown on time, like the plain one.
        QCOMPARE(keyed.look.hitsWaited, plain.look.hitsWaited);
        QVERIFY2(keyed.lateMs <= plain.lateMs + 50.0, "the keyed cut frame is late");
        QVERIFY2(keyed.lost <= other.lost + 3, "same file loses more than another file");
        QVERIFY2(keyed.lost <= startOn.lost + 3, "crossing loses more than starting on the clip");
        QVERIFY2(keyed.hold <= std::max(plain.hold, startOn.hold) + 80.0,
            "a long hold only when crossing into the keyed clip");
    }

    void deepInGop_data()
    {
        QTest::addColumn<int>("leadFrames");
        QTest::newRow("play 2.5 s before the cut") << 0;
        QTest::newRow("play 1 s before the cut") << kFps;
    }

    /// The second clip lies deep inside a long GOP of the file: preparing its decoder means
    /// decoding most of the GOP, which takes longer than the look-ahead of a short lead.
    void deepInGop()
    {
        QFETCH(int, leadFrames);
        report(
            "keyed, deep in the GOP", Case {true, false, false, 0, 0, 590 * kFps / 60, leadFrames});
        const Result keyed = m_last;
        report("plain, deep in the GOP",
            Case {false, false, false, 0, 0, 590 * kFps / 60, leadFrames});
        // The decoder prepared for the cut must be ready at it (before the paused look-ahead,
        // a play started 1 s before the cut found it still working: "waited"), keys or not.
        // Lost frames are only bounded loosely: the CPU compositor draws the keyed zoom and
        // rotation slower than the plain clip.
        // Measured as the gap across the cut (219 ms before, 20 ms after, on 4 busy CPUs).
        QVERIFY2(keyed.lateMs <= 100.0, "the picture holds at the cut: its decoder was not ready");
        QVERIFY2(keyed.look.cancelled == 0, "the look-ahead was cancelled");
        QVERIFY2(keyed.lost <= 12, "a cut into the depth of a long GOP loses frames");
    }

    void slowOpen_data()
    {
        QTest::addColumn<int>("delayMs");
        QTest::newRow("open 300 ms") << 300;
        QTest::newRow("open 800 ms") << 800;
        QTest::newRow("open 1200 ms") << 1200;
    }

    /// A hardware decoder opens slowly: the look-ahead for a same-file jump must still be early
    /// enough, or the cut loses frames.
    void slowOpen()
    {
        QFETCH(int, delayMs);
        report("keyed, jump in the same file", Case {true, false, false, delayMs, 2 * kFps});
        QVERIFY2(m_last.lateMs <= 100.0, "a slow decoder open at the cut loses frames");
    }

private:
    /// Frame-by-frame view of the cut (spec 72.7): for each frame shown around the cut, when it
    /// reached the screen compared with its ideal time (the schedule of the frames before the
    /// cut). Starting 16 frames / 2 s before the cut must be as good as starting 4 before.
    struct CutView
    {
        double worstLateMs = 0; ///< worst lateness of a shown frame in cut..cut+9
        int skipped = 0; ///< frames of cut..cut+9 never shown
        double decodeSumMs = 0; ///< decode time on the render path of the frames cut..cut+3
        double firstLateMs = 0; ///< lateness of the cut frame itself (a skipped one: 1000)
        std::vector<QString> lines;
        render::DecoderPool::PrepareStats look;
    };

    CutView cutView(int leadFrames, bool full, int gap)
    {
        Case c {true, false, false, 0, gap, -1, leadFrames};
        Project project;
        buildTimeline(project, m_dir + QStringLiteral("/screen_a.mp4"),
            m_dir + QStringLiteral("/screen_b.mp4"), c);
        auto* audio = new test::FakeAudioOutput(10, 40);
        PlaybackEngine::Options options;
        options.audio.reset(audio);
        PlaybackEngine engine(std::move(options));
        engine.setQualityMode(
            full ? PlaybackEngine::QualityMode::Full : PlaybackEngine::QualityMode::Half);
        engine.setMediaPool(project.mediaPool());
        engine.setTimeline(project.timeline());
        int client = 0;
        engine.setOutputClient(&client, QSize(1382, 778), engine.compositor()->isGpu());
        render::DecoderPool::setReadAheadFrames(
            qEnvironmentVariableIntValue("KK_KEYCUT_NO_READAHEAD")
                ? 0
                : render::DecoderPool::kReadAheadFrames);
        const int64_t startFrame = kClipFrames - leadFrames;
        engine.seek(startFrame);
        QTest::qWait(3000); // resting: the cut's decoder is prepared meanwhile
        struct Shown
        {
            int64_t frame;
            double atMs;
            double decodeMs, uploadMs, compositeMs;
        };
        std::vector<Shown> shown;
        QElapsedTimer clock;
        clock.start();
        QObject::connect(
            &engine, &PlaybackEngine::frameReady, &engine, [&](const RenderedFramePtr& f) {
                if (f && f->ok)
                    shown.push_back({f->frame, clock.nsecsElapsed() / 1e6, f->decodeMs, f->uploadMs,
                        f->compositeMs});
            });
        engine.play();
        (void)QTest::qWaitFor(
            [&] { return engine.currentFrame() && engine.position() >= kClipFrames + 30; }, 20000);
        engine.pause();
        // Ideal time of a frame: the median offset of the frames shown 2..12 before the cut.
        std::vector<double> offsets;
        for (const Shown& s : shown) {
            if (s.frame >= kClipFrames - 12 && s.frame < kClipFrames - 2)
                offsets.push_back(s.atMs - double(s.frame - startFrame) * 1000.0 / kFps);
        }
        CutView v;
        v.look = engine.compositor()->prepareStats();
        if (offsets.empty())
            offsets.push_back(shown.empty() ? 0.0 : shown.front().atMs);
        std::sort(offsets.begin(), offsets.end());
        const double base = offsets[offsets.size() / 2];
        for (int64_t f = kClipFrames - 3; f < kClipFrames + 10; ++f) {
            const auto it = std::find_if(
                shown.begin(), shown.end(), [f](const Shown& s) { return s.frame == f; });
            const bool after = f >= kClipFrames;
            if (it == shown.end()) {
                v.skipped += after;
                if (f == kClipFrames)
                    v.firstLateMs = 1000.0;
                v.lines.push_back(QStringLiteral("  frame cut%1%2: NOT SHOWN")
                                      .arg(f >= kClipFrames ? "+" : "")
                                      .arg(f - kClipFrames));
                continue;
            }
            const double late = it->atMs - (base + double(f - startFrame) * 1000.0 / kFps);
            if (after)
                v.worstLateMs = std::max(v.worstLateMs, late);
            if (f == kClipFrames)
                v.firstLateMs = late;
            if (after && f < kClipFrames + 4)
                v.decodeSumMs += it->decodeMs;
            v.lines.push_back(QStringLiteral("  frame cut%1%2: late %3 ms (decode %4, upload %5, "
                                             "composite %6)")
                                  .arg(f >= kClipFrames ? "+" : "")
                                  .arg(f - kClipFrames)
                                  .arg(late, 0, 'f', 1)
                                  .arg(it->decodeMs, 0, 'f', 1)
                                  .arg(it->uploadMs, 0, 'f', 1)
                                  .arg(it->compositeMs, 0, 'f', 1));
        }
        return v;
    }

private Q_SLOTS:
    void firstFramesAtCut_data()
    {
        QTest::addColumn<bool>("full");
        QTest::addColumn<int>("gap");
        QTest::newRow("full, jump in the file") << true << 2 * kFps;
        QTest::newRow("full, contiguous") << true << 0;
        QTest::newRow("half, jump in the file") << false << 2 * kFps;
    }

    /// Spec 72.7: in steady playback the first frames after the cut are ready before their
    /// display time, as when playing from 4 frames before the cut.
    void firstFramesAtCut()
    {
        QFETCH(bool, full);
        QFETCH(int, gap);
        const int leads[] = {4, 16, 2 * kFps};
        CutView views[3];
        for (int i = 0; i < 3; ++i) {
            views[i] = cutView(leads[i], full, gap);
            qInfo("start %d frames before the cut: cut frame late %.1f ms, worst late %.1f ms, "
                  "%d of 10 skipped, decode on the render path cut..cut+3 %.1f ms; look-ahead "
                  "started %lld, ready %lld, hits ready %lld, "
                  "waited %lld, expired %lld, cancelled %lld, last cost %d ms",
                leads[i], views[i].firstLateMs, views[i].worstLateMs, views[i].skipped,
                views[i].decodeSumMs, (long long)views[i].look.started,
                (long long)views[i].look.ready, (long long)views[i].look.hitsReady,
                (long long)views[i].look.hitsWaited, (long long)views[i].look.expired,
                (long long)views[i].look.cancelled, views[i].look.lastCostMs);
            for (const QString& l : views[i].lines)
                qInfo("%s", qPrintable(l));
        }
        const double frameMs = 1000.0 / kFps;
        // Generous margins (a busy machine may drop a frame anywhere): the frames of a start
        // far before the cut are as late as those of a start right before it, and the cut frame
        // is not the one that goes missing.
        for (int i = 1; i < 3; ++i) {
            QVERIFY2(views[i].skipped <= views[0].skipped + 2, "frames skipped after the cut");
            QVERIFY2(views[i].worstLateMs <= std::max(views[0].worstLateMs, 0.0) + frameMs,
                "a frame after the cut is shown more than a frame late");
            QVERIFY2(views[i].decodeSumMs <= views[0].decodeSumMs + 20.0,
                "the first frames after the cut are decoded on the render path");
        }
    }

private:
    QTemporaryDir m_temp;
    QString m_dir;
    Result m_last;
};

QTEST_MAIN(TestKeyframeCut)
#include "test_keyframecut.moc"
