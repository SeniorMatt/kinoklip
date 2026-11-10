// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the Stabilize button's job (compositing/tracking/StabilizeJob.h, spec section 58):
// which source frames a clip needs (trim, speed, reverse, retime curve), what is refused and why,
// the measurement of a real (PNG sequence) clip, and the background runner with progress,
// cancel and a second start while running.
#include "ShakyFootage.h"
#include "compositing/tracking/StabilizeJob.h"
#include "core/MediaItem.h"
#include "core/ParamKeys.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;
using namespace kk::test;
using namespace kk::tracking;
namespace st = kk::stabilize;

namespace {

const Rational kFps {25, 1};

MediaItem videoItem(int64_t frames, Rational rate = kFps)
{
    MediaItem item;
    item.id = QStringLiteral("m");
    item.path = QStringLiteral("/nowhere/clip.mp4");
    item.info.hasVideo = true;
    item.info.frameRate = rate;
    item.info.durationUs = framesToUs(frames, rate);
    return item;
}

ClipData clipOf(int64_t start, int64_t duration, int64_t sourceInFrames = 0)
{
    ClipData c;
    c.kind = ClipKind::Media;
    c.mediaId = QStringLiteral("m");
    c.start = start;
    c.duration = duration;
    c.sourceInUs = framesToUs(sourceInFrames, kFps);
    return c;
}

} // namespace

class TestStabilizeJob : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_sequence;
    std::vector<CameraPose> m_poses;

    MediaItem sequenceItem() const
    {
        MediaItem item;
        item.id = QStringLiteral("v");
        item.path = m_sequence;
        item.info = media::probe(m_sequence);
        return item;
    }

private slots:
    void initTestCase()
    {
        ShakeSettings shake;
        shake.frames = 24;
        shake.jitterPx = 3.0;
        m_poses = shakyPath(shake);
        const QImage big = texturedPicture(900, 560, 5);
        std::vector<QImage> frames;
        for (const CameraPose& p : m_poses)
            frames.push_back(shakyFrame(big, 320, 180, p));
        m_sequence = writeSequence(m_tmp.filePath(QStringLiteral("seq")), frames);
    }

    // The source frames the clip shows, whatever its trim, speed, direction or retime curve.
    void planCoversTheShownSourceFrames()
    {
        QString why;
        auto plan = [&](const ClipData& c, int64_t mediaFrames = 500) {
            return planStabilizeJob(c, videoItem(mediaFrames), kFps, st::Mode::Similarity, &why);
        };
        auto job = plan(clipOf(100, 50, 30));
        QVERIFY2(job, qPrintable(why));
        QCOMPARE(job->request.firstFrame, int64_t(30));
        QCOMPARE(job->request.lastFrame, int64_t(79));
        QCOMPARE(job->request.mode, st::Mode::Similarity);
        QCOMPARE(job->request.fps, kFps);
        QCOMPARE(job->path, QStringLiteral("/nowhere/clip.mp4"));

        ClipData fast = clipOf(0, 20, 10);
        fast.speed = 2.0;
        job = plan(fast);
        QCOMPARE(job->request.firstFrame, int64_t(10));
        QCOMPARE(job->request.lastFrame, int64_t(48));

        ClipData reversed = clipOf(0, 20, 50);
        reversed.speed = -1.0;
        job = plan(reversed);
        QCOMPARE(job->request.firstFrame, int64_t(31));
        QCOMPARE(job->request.lastFrame, int64_t(50));

        ClipData curved = clipOf(0, 40, 0);
        AnimatedValue& v = curved.retime.animated(keys::RetimeFrame, 0.0);
        v.setKey(0, 0.0);
        v.setKey(20, 20.0);
        v.setKey(40, 5.0); // forward to 20, then back to 5
        job = plan(curved);
        QCOMPARE(job->request.firstFrame, int64_t(0));
        QCOMPARE(job->request.lastFrame, int64_t(20));

        // Never past the end of the media (a clip trimmed beyond it), never before 0.
        job = plan(clipOf(0, 50, 20), 40);
        QCOMPARE(job->request.lastFrame, int64_t(39));
        // A media rate that differs from the timeline's: numbers are in the media's frames.
        job = planStabilizeJob(
            clipOf(0, 25, 0), videoItem(500, Rational {50, 1}), kFps, st::Mode::Translation, &why);
        QCOMPARE(job->request.fps, (Rational {50, 1}));
        QCOMPARE(job->request.lastFrame, int64_t(48));
    }

    // What cannot be stabilized is refused with a sentence.
    void refusals()
    {
        QString why;
        ClipData freeze = clipOf(0, 20, 5);
        freeze.speed = 0.0;
        QVERIFY(!planStabilizeJob(freeze, videoItem(100), kFps, st::Mode::Similarity, &why));
        QVERIFY2(why.contains(QStringLiteral("freeze")), qPrintable(why));

        MediaItem still = videoItem(100);
        still.info.isStillImage = true;
        why.clear();
        QVERIFY(!planStabilizeJob(clipOf(0, 20), still, kFps, st::Mode::Similarity, &why));
        QVERIFY2(why.contains(QStringLiteral("still")), qPrintable(why));

        MediaItem audio = videoItem(100);
        audio.info.hasVideo = false;
        why.clear();
        QVERIFY(!planStabilizeJob(clipOf(0, 20), audio, kFps, st::Mode::Similarity, &why));
        QVERIFY(!why.isEmpty());

        ClipData title = clipOf(0, 20);
        title.kind = ClipKind::Title;
        why.clear();
        QVERIFY(!planStabilizeJob(title, videoItem(100), kFps, st::Mode::Similarity, &why));
        QVERIFY(why.contains(QStringLiteral("video file")));

        MediaItem noRate = videoItem(100);
        noRate.info.frameRate = Rational {0, 1};
        why.clear();
        QVERIFY(!planStabilizeJob(clipOf(0, 20), noRate, kFps, st::Mode::Similarity, &why));
        QVERIFY(why.contains(QStringLiteral("frame rate")));
    }

    // A real clip: the measurement follows the true camera path.
    void measuresASequenceClip()
    {
        ClipData clip;
        clip.kind = ClipKind::Media;
        clip.mediaId = QStringLiteral("v");
        clip.duration = 24;
        QString why;
        const auto job = planStabilizeJob(clip, sequenceItem(), kFps, st::Mode::Translation, &why);
        QVERIFY2(job, qPrintable(why));
        int last = 0;
        const StabilizeOutcome out = runStabilizeJob(*job, [&](int done, int) { last = done; });
        QVERIFY2(out.data, qPrintable(out.error));
        QCOMPARE(last, 24);
        for (int t = 0; t < 24; ++t) {
            QVERIFY(std::abs(double(out.data->path()[size_t(2 * t)]) * 320 - m_poses[size_t(t)].tx)
                < 0.4);
            QVERIFY(
                std::abs(double(out.data->path()[size_t(2 * t + 1)]) * 320 - m_poses[size_t(t)].ty)
                < 0.4);
        }
        // An unreadable file is an error, not a crash.
        StabilizeJob broken = *job;
        broken.path = QStringLiteral("/nowhere/missing.mp4");
        const StabilizeOutcome failed = runStabilizeJob(broken);
        QVERIFY(!failed.data && !failed.error.isEmpty());
    }

    void applyStabilizationSwitchesItOn()
    {
        ClipData clip = clipOf(0, 10);
        st::Settings s;
        s.mode = st::Mode::Similarity;
        s.strength = 7.0; // out of range
        const auto data = std::make_shared<const st::Data>(
            st::Mode::Similarity, kFps, 0, 0.5625, std::vector<float>(8, 0.0f));
        applyStabilization(clip, s, data);
        QVERIFY(clip.stabilization.settings.enabled);
        QCOMPARE(clip.stabilization.settings.strength, 1.0);
        QVERIFY(clip.stabilization.data == data);
        QVERIFY(st::isActive(clip.stabilization));
    }

    // The runner works off the calling thread: progress arrives, finished comes once.
    void runnerReportsProgressAndFinishes()
    {
        ClipData clip;
        clip.kind = ClipKind::Media;
        clip.mediaId = QStringLiteral("v");
        clip.duration = 24;
        QString why;
        auto job = planStabilizeJob(clip, sequenceItem(), kFps, st::Mode::Translation, &why);
        QVERIFY2(job, qPrintable(why));
        StabilizeRunner runner;
        QSignalSpy progress(&runner, &StabilizeRunner::progress);
        QSignalSpy finished(&runner, &StabilizeRunner::finished);
        QVERIFY(runner.start(42, *job));
        QVERIFY(runner.isRunning());
        QCOMPARE(runner.clipId(), quint64(42));
        QVERIFY(!runner.start(43, *job)); // one run at a time
        QVERIFY(finished.wait(60000));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().first().toULongLong(), quint64(42));
        QVERIFY(!runner.isRunning());
        QVERIFY(runner.outcome().data);
        QVERIFY(progress.count() >= 1);
        QCOMPARE(runner.total(), 24);
    }

    void runnerCancel()
    {
        ClipData clip;
        clip.kind = ClipKind::Media;
        clip.mediaId = QStringLiteral("v");
        clip.duration = 24;
        QString why;
        auto job = planStabilizeJob(clip, sequenceItem(), kFps, st::Mode::Perspective, &why);
        QVERIFY2(job, qPrintable(why));
        StabilizeRunner runner;
        QSignalSpy finished(&runner, &StabilizeRunner::finished);
        QVERIFY(runner.start(1, *job));
        runner.cancel();
        QVERIFY(finished.wait(60000));
        QVERIFY(runner.outcome().cancelled);
        QVERIFY(!runner.outcome().data);
        // It can run again afterwards.
        QVERIFY(runner.start(1, *job));
        QVERIFY(finished.wait(60000));
        QVERIFY(runner.outcome().data);
    }
};

QTEST_MAIN(TestStabilizeJob)
#include "test_stabilizejob.moc"
