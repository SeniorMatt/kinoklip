// SPDX-License-Identifier: GPL-3.0-or-later
// Decoder look-ahead at cuts (spec 1.29 - 1.34): the decoder of the next clip is opened, seeked
// and decoded forward on a worker thread before the cut, so the render thread finds the first
// frame after the cut ready. Long-GOP H.264 clips cutting far from a keyframe. Counters and
// decoded-frame counts where possible; the engine tests also check the delivery time, with the
// margin stated there. tests/playback/test_cutperf.cpp measures the milliseconds.
#include "PlaybackTestUtil.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "media/VideoDecoder.h"
#include "playback/FrameScheduler.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"
#include "render/DecodeTargets.h"
#include "render/DecoderPool.h"
#include "render/TimelineEval.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>

#include <algorithm>

using namespace kk;
using render::DecoderPool;

namespace {

constexpr int kFps = 60;
constexpr int kGop = 250;
constexpr int kOffset = 230; ///< in-points lie this many frames after a keyframe
const Rational kRate {kFps, 1};

int64_t farTime(int gop, int extraFrames = 0)
{
    return framesToUs(gop * kGop + kOffset + extraFrames, kRate);
}

bool makeMedia(const QString& path, const char* source)
{
    QProcess ffmpeg;
    ffmpeg.setProcessChannelMode(QProcess::ForwardedChannels);
    ffmpeg.start(QStringLiteral("ffmpeg"),
        {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
            QStringLiteral("%1=s=640x360:r=%2:d=30").arg(QLatin1String(source)).arg(kFps), "-f",
            "lavfi", "-i", "sine=frequency=440:d=30", "-c:v", "libx264", "-preset", "ultrafast",
            "-g", QString::number(kGop), "-keyint_min", QString::number(kGop), "-sc_threshold", "0",
            "-bf", "2", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", path});
    return ffmpeg.waitForFinished(5 * 60 * 1000) && ffmpeg.exitCode() == 0;
}

/// Waits (pumping the pool's frame start) until @p done or 10 s.
template<typename Done>
bool pumpUntil(DecoderPool& pool, Done done)
{
    return QTest::qWaitFor(
        [&] {
            pool.beginFrame();
            return done();
        },
        10000);
}

ClipData mediaClip(TimelineData& tl, const QString& id, int64_t start, int64_t duration, int64_t inUs)
{
    ClipData c;
    c.id = tl.allocateId();
    c.mediaId = id;
    c.start = start;
    c.duration = duration;
    c.sourceInUs = inUs;
    return c;
}

/// V1 + A1: clips of @p clipFrames frames, alternately from the two files when @p otherFile,
/// each entering a different GOP far from its keyframe. @return the cut frames.
std::vector<int64_t> buildCuts(Project& project, const QStringList& files, int clips,
    int clipFrames, bool otherFile, bool sound = true)
{
    const ImportResult imported = project.importMedia(files);
    project.beginEdit(QStringLiteral("cuts"));
    TimelineData& tl = project.timeline()->mutableData();
    tl = makeDefaultTimeline(QStringLiteral("cuts"), kRate, 640, 360, 2, 2);
    std::vector<int64_t> cuts;
    int64_t at = 0;
    for (int i = 0; i < clips; ++i) {
        const QString id = imported.importedIds[otherFile ? i % 2 : 0];
        ClipData c = mediaClip(tl, id, at, clipFrames, farTime(i % 6));
        c.sourceDurationUs = 30'000'000;
        tl.videoTracks[0].clips.push_back(c);
        if (sound) {
            c.id = tl.allocateId();
            tl.audioTracks[0].clips.push_back(c);
        }
        if (i > 0)
            cuts.push_back(at);
        at += clipFrames;
    }
    project.endEdit();
    return cuts;
}

struct EngineFixture
{
    std::shared_ptr<Compositor> compositor;
    test::FakeAudioOutput* audio = nullptr;
    std::unique_ptr<PlaybackEngine> engine;
    int client = 0;

    explicit EngineFixture(Project& project)
    {
        // The CPU backend behaves the same everywhere; both backends share the look-ahead.
        Compositor::Options co;
        co.forceCpu = true;
        compositor = std::make_shared<Compositor>(co);
        PlaybackEngine::Options options;
        options.compositor = compositor;
        audio = new test::FakeAudioOutput(10, 40);
        options.audio.reset(audio);
        engine = std::make_unique<PlaybackEngine>(std::move(options));
        engine->setMediaPool(project.mediaPool());
        engine->setTimeline(project.timeline());
        engine->setOutputClient(&client, QSize(640, 360), false);
    }
};

} // namespace

class TestCutLookAhead : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    QString m_a, m_b;

private Q_SLOTS:
    void initTestCase()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("no ffmpeg");
        QVERIFY(m_dir.isValid());
        m_a = m_dir.filePath(QStringLiteral("a.mp4"));
        m_b = m_dir.filePath(QStringLiteral("b.mp4"));
        QVERIFY(makeMedia(m_a, "testsrc2"));
        QVERIFY(makeMedia(m_b, "testsrc"));
    }

    // ---- DecoderPool ----

    /// 1.29: the prepared decoder has the frame before it is asked for: the request decodes
    /// nothing, where a fresh decoder needs the whole way from the keyframe. Same picture.
    void preparedDecoderIsReadyBeforeTheCut()
    {
        const int64_t target = farTime(2);
        media::VideoDecoder fresh;
        QVERIFY(fresh.open(m_a));
        const int64_t beforeFresh = fresh.decodedFrameCount();
        const QImage expected = fresh.frameAt(target);
        QVERIFY(fresh.decodedFrameCount() - beforeFresh >= kOffset);

        QThreadPool threads;
        DecoderPool pool;
        pool.setPrepareThreads(&threads);
        pool.prepare(m_a, target, 5000.0, nullptr);
        QVERIFY(QTest::qWaitFor([&] { return pool.prepareStats().ready == 1; }, 10000));
        pool.beginFrame();
        DecoderPool::Entry* entry = pool.acquire(m_a, target);
        QVERIFY(entry);
        QCOMPARE(pool.prepareStats().hitsReady, int64_t(1));
        QCOMPARE(pool.prepareStats().hitsWaited, int64_t(0));
        QCOMPARE(pool.openCount(), 1);
        QCOMPARE(pool.reservedCount(), 0);
        const int64_t before = entry->decoder->decodedFrameCount();
        const QImage image = entry->decoder->frameAt(target);
        QCOMPARE(entry->decoder->decodedFrameCount(), before);
        QCOMPARE(image, expected);
        // It plays on from there.
        QVERIFY(entry->decoder->decodeFrame(target + 16667));
        QVERIFY(entry->decoder->decodedFrameCount() - before <= 3);
    }

    /// 72.7: the first frames after the cut are ready too, not only the cut frame: requests for
    /// the next DecoderPool::kReadAheadFrames frames decode nothing (decoded, and for hardware
    /// decoding downloaded, during the preparation), identical to what a fresh decoder returns,
    /// and playing on from there still works.
    void preparedDecoderHasTheFirstFramesAfterTheCut()
    {
        const int64_t target = farTime(2);
        const int64_t step = 16667;
        media::VideoDecoder fresh;
        QVERIFY(fresh.open(m_a));
        std::vector<QImage> expected;
        for (int k = 0; k <= DecoderPool::kReadAheadFrames + 2; ++k)
            expected.push_back(fresh.frameAt(target + k * step));

        QThreadPool threads;
        DecoderPool pool;
        pool.setPrepareThreads(&threads);
        pool.prepare(m_a, target, 5000.0, nullptr);
        QVERIFY(QTest::qWaitFor([&] { return pool.prepareStats().ready == 1; }, 10000));
        pool.beginFrame();
        DecoderPool::Entry* entry = pool.acquire(m_a, target);
        QVERIFY(entry);
        const int64_t before = entry->decoder->decodedFrameCount();
        for (int k = 0; k <= DecoderPool::kReadAheadFrames; ++k) {
            const auto frame = entry->decoder->decodeFrame(target + k * step);
            QVERIFY(frame);
            QCOMPARE(entry->decoder->decodedFrameCount(), before);
            QCOMPARE(frame->ptsUs, fresh.decodeFrame(target + k * step)->ptsUs);
        }
        // The same through the image path (CPU compositor), in play order.
        for (int k = 0; k <= DecoderPool::kReadAheadFrames; ++k)
            QCOMPARE(entry->decoder->frameAt(target + k * step), expected[size_t(k)]);
        QCOMPARE(entry->decoder->decodedFrameCount(), before);
        // On after the held frames, and back again: same pictures as a fresh decoder.
        for (int k = DecoderPool::kReadAheadFrames + 1; k <= DecoderPool::kReadAheadFrames + 2; ++k)
            QCOMPARE(entry->decoder->frameAt(target + k * step), expected[size_t(k)]);
        QCOMPARE(entry->decoder->frameAt(target), expected[0]);
    }

    /// 1.29: a request arriving before the preparation is done waits for the rest of it and
    /// gets the right frame.
    void requestDuringPreparationWaits()
    {
        const int64_t target = farTime(3);
        QThreadPool threads;
        DecoderPool pool;
        pool.setPrepareThreads(&threads);
        pool.prepare(m_a, target, 5000.0, nullptr);
        DecoderPool::Entry* entry = pool.acquire(m_a, target);
        QVERIFY(entry);
        QCOMPARE(pool.openCount(), 1); // not a second decoder
        const auto frame = entry->decoder->decodeFrame(target);
        QVERIFY(frame);
        QVERIFY(qAbs(frame->ptsUs - target) <= 16667);
    }

    /// 1.31: cancelling stops the work between two decoded frames; the decoder stays usable.
    void cancelStopsPreparation()
    {
        const int64_t target = farTime(4);
        QThreadPool threads;
        DecoderPool pool;
        pool.setPrepareThreads(&threads);
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        pool.prepare(m_a, target, 5000.0, cancelled);
        QCOMPARE(pool.reservedCount(), 1);
        cancelled->store(true);
        pool.beginFrame(); // sees the flag at once, never waits for the worker
        QCOMPARE(pool.prepareStats().cancelled, int64_t(1));
        QVERIFY(pumpUntil(pool, [&] { return pool.reservedCount() == 0; }));
        QCOMPARE(pool.prepareStats().ready, int64_t(0));
        // A wish still waiting for a reservation is dropped too.
        for (int i = 0; i < 6; ++i)
            pool.prepare(m_b, farTime(i), 5000.0, cancelled);
        pool.beginFrame();
        QCOMPARE(pool.reservedCount(), 0);
        QCOMPARE(pool.waitingCount(), 0);
        // Whatever is left of it serves the next request correctly.
        DecoderPool::Entry* entry = pool.acquire(m_a, farTime(1));
        QVERIFY(entry);
        const auto frame = entry->decoder->decodeFrame(farTime(1));
        QVERIFY(frame);
        QVERIFY(qAbs(frame->ptsUs - farTime(1)) <= 16667);
    }

    /// 1.31: VideoDecoder::prepareFrame() gives up mid-way and leaves a valid decoder.
    void prepareFrameStopsMidway()
    {
        media::VideoDecoder decoder;
        QVERIFY(decoder.open(m_a));
        int checks = 0;
        const int64_t start = decoder.decodedFrameCount();
        QVERIFY(!decoder.prepareFrame(farTime(1), [&] { return ++checks > 10; }));
        QVERIFY(decoder.decodedFrameCount() - start <= 11);
        const auto frame = decoder.decodeFrame(farTime(1));
        QVERIFY(frame);
        QVERIFY(qAbs(frame->ptsUs - farTime(1)) <= 16667);
    }

    /// 1.33: never more than kMaxReserved decoders held, kMaxPrepareRunning at work, the wish
    /// list stays short, however many cuts are ahead.
    void queuedWorkStaysBounded()
    {
        QThreadPool threads;
        DecoderPool pool;
        pool.setPrepareThreads(&threads);
        for (int i = 0; i < 24; ++i) {
            pool.prepare(i % 2 ? m_a : m_b, farTime(i % 6, i), 5000.0, nullptr);
            QVERIFY(pool.reservedCount() <= DecoderPool::kMaxReserved);
            QVERIFY(pool.waitingCount() <= DecoderPool::kMaxPrepareWaiting);
        }
        QVERIFY(pumpUntil(pool, [&] {
            return pool.prepareStats().ready == pool.reservedCount()
                && pool.reservedCount() == DecoderPool::kMaxReserved;
        }));
        QCOMPARE(pool.prepareStats().peakReserved, DecoderPool::kMaxReserved);
        QVERIFY(pool.prepareStats().peakRunning <= DecoderPool::kMaxPrepareRunning);
        QCOMPARE(pool.openCount(), DecoderPool::kMaxReserved);
    }

    /// 1.33: a reservation nobody came for ends after its due time, its decoder is ordinary.
    void overdueReservationExpires()
    {
        QThreadPool threads;
        DecoderPool pool;
        pool.setPrepareThreads(&threads);
        pool.prepare(m_a, farTime(1), 0.0, nullptr);
        QVERIFY(pumpUntil(pool, [&] { return pool.reservedCount() == 0; }));
        QCOMPARE(pool.prepareStats().expired, int64_t(1));
        QVERIFY(pool.openCount() <= 1);
    }

    /// 1.29: a reservation is kept for its position: another request of the same file does not
    /// take it (it opens or uses another decoder); an idle decoder is recycled for the next one.
    void reservationKeptAndIdleRecycled()
    {
        QThreadPool threads;
        DecoderPool pool;
        pool.setPrepareThreads(&threads);
        pool.prepare(m_a, farTime(5), 5000.0, nullptr);
        DecoderPool::Entry* other = pool.acquire(m_a, farTime(0));
        QVERIFY(other);
        QVERIFY(!other->reservation);
        QCOMPARE(pool.openCount(), 2);
        QCOMPARE(pool.reservedCount(), 1);
        QVERIFY(other->decoder->decodeFrame(farTime(0)));
        // Unused for kRecycleIdleMs: the next preparation repositions it instead of opening.
        QTest::qWait(int(DecoderPool::kRecycleIdleMs) + 20);
        pool.beginFrame();
        pool.prepare(m_a, farTime(3), 5000.0, nullptr);
        QCOMPARE(pool.openCount(), 2);
        QCOMPARE(pool.prepareStats().reused, int64_t(1));
    }

    /// 1.33: a file that cannot be opened costs one failed preparation and does not hurt.
    void missingFile()
    {
        QThreadPool threads;
        DecoderPool pool;
        pool.setPrepareThreads(&threads);
        const QString missing = m_dir.filePath(QStringLiteral("missing.mp4"));
        pool.prepare(missing, 0, 5000.0, nullptr);
        QVERIFY(!pool.acquire(missing, 0));
        QCOMPARE(pool.openCount(), 0);
    }

    // ---- What a frame needs ----

    /// 1.30: all video tracks, transitions, nested clips, proxies; razor cuts need nothing.
    void decodeTargets()
    {
        TimelineData tl = makeDefaultTimeline(QStringLiteral("t"), kRate, 640, 360, 3, 0);
        auto media = std::make_shared<MediaPathMap>();
        media->insert(QStringLiteral("a"), QStringLiteral("/a.mp4"));
        media->insert(QStringLiteral("b"), QStringLiteral("/b.mp4"));
        tl.videoTracks[0].clips.push_back(mediaClip(tl, QStringLiteral("a"), 0, 100, 1'000'000));
        // Razor cut: the same file on, no jump.
        tl.videoTracks[0].clips.push_back(
            mediaClip(tl, QStringLiteral("a"), 100, 100, framesToUs(60 + 100, kRate)));
        // V2: another file enters at 150.
        tl.videoTracks[1].clips.push_back(mediaClip(tl, QStringLiteral("b"), 150, 50, 2'000'000));
        // V3 is disabled.
        tl.videoTracks[2].enabled = false;
        tl.videoTracks[2].clips.push_back(mediaClip(tl, QStringLiteral("b"), 120, 10, 0));

        std::vector<render::DecodeTarget> at99, at100, at149, at150;
        render::collectDecodeTargets(tl, 99, tl.nested, media.get(), nullptr, at99);
        render::collectDecodeTargets(tl, 100, tl.nested, media.get(), nullptr, at100);
        render::collectDecodeTargets(tl, 149, tl.nested, media.get(), nullptr, at149);
        render::collectDecodeTargets(tl, 150, tl.nested, media.get(), nullptr, at150);
        QCOMPARE(at100.size(), size_t(1));
        QVERIFY(render::continuesFrom(at100[0], at99)); // razor cut
        QCOMPARE(at150.size(), size_t(2));
        QVERIFY(render::continuesFrom(at150[0], at149)); // V1 plays on
        QVERIFY(!render::continuesFrom(at150[1], at149)); // V2 is new
        QCOMPARE(at150[1].path, QStringLiteral("/b.mp4"));
        QCOMPARE(at150[1].timeUs, int64_t(2'000'000));

        // A jump inside the same file needs a decoder, unless it is only a few frames.
        tl.videoTracks[0].clips[1].sourceInUs = framesToUs(60 + 100 + 3, kRate);
        at100.clear();
        render::collectDecodeTargets(tl, 100, tl.nested, media.get(), nullptr, at100);
        QVERIFY(render::continuesFrom(at100[0], at99));
        tl.videoTracks[0].clips[1].sourceInUs = 20'000'000;
        at100.clear();
        render::collectDecodeTargets(tl, 100, tl.nested, media.get(), nullptr, at100);
        QVERIFY(!render::continuesFrom(at100[0], at99));

        // A proxy is what gets decoded.
        ProxyMap proxies;
        proxies.insert(QStringLiteral("b"), ProxyRef {QStringLiteral("/proxy.mov"), QSizeF(640, 360)});
        std::vector<render::DecodeTarget> proxied;
        render::collectDecodeTargets(tl, 150, tl.nested, media.get(), &proxies, proxied);
        QCOMPARE(proxied[1].path, QStringLiteral("/proxy.mov"));
    }

    /// 1.31: the scheduler asks once per frame, splits at a loop's wrap, restarts after clear()
    /// and an edit, and those flags stop what is in progress. Other requests are not counted.
    void schedulerRanges()
    {
        std::vector<RenderRequest> requests;
        FrameScheduler scheduler([&](RenderRequest r) {
            requests.push_back(r);
            std::promise<RenderedFramePtr> p;
            p.set_value(std::make_shared<RenderedFrame>());
            return p.get_future();
        });
        RenderParams params;
        params.timeline = std::make_shared<TimelineData>();
        scheduler.setParams(params);
        auto straight = [](int64_t key) { return key; };

        QCOMPARE(scheduler.prepareAhead(100, 160, 10.0, straight), 1);
        QCOMPARE(scheduler.issuedCount(), int64_t(0));
        QCOMPARE(scheduler.inFlight(), 0);
        QCOMPARE(requests.back().prepare->previous, int64_t(100));
        QCOMPARE(requests.back().prepare->first, int64_t(101));
        QCOMPARE(requests.back().prepare->end, int64_t(161));
        QCOMPARE(requests.back().prepare->firstDueMs, 10.0);
        // The window moves: only the new frames.
        QCOMPARE(scheduler.prepareAhead(100, 160, 10.0, straight), 0);
        QCOMPARE(scheduler.prepareAhead(101, 161, 10.0, straight), 1);
        QCOMPARE(requests.back().prepare->first, int64_t(161));
        QCOMPARE(requests.back().prepare->previous, int64_t(160));
        QCOMPARE(requests.back().prepare->firstDueMs, 600.0);

        // A size-only change keeps it, a seek (clear) and an edit stop it and start over.
        const std::shared_ptr<const std::atomic<bool>> flag = requests.back().cancelled;
        params.outSize = QSize(100, 100);
        scheduler.setParams(params, false);
        QVERIFY(!flag->load());
        QCOMPARE(scheduler.prepareAhead(102, 162, 10.0, straight), 1);
        QCOMPARE(requests.back().prepare->first, int64_t(162));
        scheduler.clear();
        QVERIFY(flag->load());
        QCOMPARE(scheduler.prepareAhead(500, 510, 10.0, straight), 1);
        QCOMPARE(requests.back().prepare->first, int64_t(501));
        const std::shared_ptr<const std::atomic<bool>> flag2 = requests.back().cancelled;
        QVERIFY(!flag2->load());
        params.timeline = std::make_shared<TimelineData>();
        scheduler.setParams(params, false); // even if the frames in flight stay
        QVERIFY(flag2->load());
        QCOMPARE(scheduler.prepareAhead(500, 510, 10.0, straight), 1);

        // Loop [0, 100): keys 95 .. 105 wrap.
        scheduler.clear();
        requests.clear();
        QCOMPARE(scheduler.prepareAhead(95, 105, 10.0, [](int64_t key) { return key % 100; }), 2);
        QCOMPARE(requests[0].prepare->first, int64_t(96));
        QCOMPARE(requests[0].prepare->end, int64_t(100));
        QCOMPARE(requests[1].prepare->first, int64_t(0));
        QCOMPARE(requests[1].prepare->end, int64_t(6));
        QCOMPARE(requests[1].prepare->previous, int64_t(-1)); // nothing continues over the wrap
        QCOMPARE(requests[1].prepare->firstDueMs, 50.0);
    }

    // ---- The compositor ----

    /// 1.29: a prepare request finds the clip entering in its range; the rendered frame is the
    /// same as without it.
    void compositorPrepare()
    {
        Project project;
        const std::vector<int64_t> cuts
            = buildCuts(project, {m_a, m_b}, 2, 30, true, false);
        auto snapshot = project.timeline()->snapshot();
        auto media = makeMediaPathMap(project.mediaPool()->state());
        Compositor::Options co;
        co.forceCpu = true;
        Compositor plain(co);
        plain.setMediaPaths(media);
        const QImage expected = plain.renderImage(snapshot, cuts[0], QSize(640, 360));
        QVERIFY(!expected.isNull());

        Compositor compositor(co);
        compositor.setMediaPaths(media);
        (void)compositor.renderImage(snapshot, 0, QSize(640, 360));
        RenderRequest request;
        request.timeline = snapshot;
        request.prepare = PrepareRange {0, 1, 45, 2000.0, 16.7};
        (void)compositor.renderAsync(request).get();
        QVERIFY(QTest::qWaitFor([&] { return compositor.prepareStats().ready == 1; }, 10000));
        QCOMPARE(compositor.prepareStats().started, int64_t(1));
        QCOMPARE(compositor.renderImage(snapshot, cuts[0], QSize(640, 360)), expected);
        QCOMPARE(compositor.prepareStats().hitsReady, int64_t(1));
    }

    // ---- The engine ----

    /// 1.29, 1.34: each cut's first frame is ready before the cut and reaches the screen on
    /// time (margin: 3 frames = 50 ms, median of the cuts; at most one of them may be dropped on
    /// a busy machine), and the sound has no gap at the cuts.
    void engineDeliversCutFramesOnTime()
    {
        Project project;
        const std::vector<int64_t> cuts = buildCuts(project, {m_a, m_b}, 6, 60, true);
        EngineFixture f(project);
        QVERIFY(QTest::qWaitFor([&] { return f.engine->currentFrame() != nullptr; }, 10000));
        std::vector<std::pair<int64_t, double>> shown;
        QElapsedTimer clock;
        clock.start();
        QObject::connect(f.engine.get(), &PlaybackEngine::frameReady, f.engine.get(),
            [&](const RenderedFramePtr& r) {
                if (r && r->ok)
                    shown.push_back({r->frame, double(clock.nsecsElapsed()) / 1e6});
            });
        f.engine->play();
        QVERIFY(QTest::qWaitFor([&] { return !f.engine->isPlaying(); }, 30000));

        const auto stats = f.compositor->prepareStats();
        qInfo("started %lld ready %lld hits %lld waited %lld", (long long)stats.started,
            (long long)stats.ready, (long long)stats.hitsReady, (long long)stats.hitsWaited);
        QVERIFY(stats.hitsReady >= int64_t(cuts.size()) - 1);
#ifndef KK_SANITIZED // sanitized builds play too slowly to keep the audio fed
        QCOMPARE(f.audio->underrunFrames(), int64_t(0));
#endif

        std::vector<double> offsets;
        for (const auto& s : shown)
            offsets.push_back(s.second - double(s.first) * 1000.0 / kFps);
        std::sort(offsets.begin(), offsets.end());
        const double onTime = offsets[offsets.size() / 10];
        std::vector<double> late;
        int exact = 0;
        for (const int64_t cut : cuts) {
            const auto it = std::find_if(
                shown.begin(), shown.end(), [cut](const auto& s) { return s.first >= cut; });
            QVERIFY(it != shown.end());
            exact += it->first == cut ? 1 : 0;
            late.push_back(it->second - (onTime + double(cut) * 1000.0 / kFps));
        }
        std::sort(late.begin(), late.end());
        qInfo("cut frames shown %d of %d; late median %.1f ms, max %.1f ms", exact,
            int(cuts.size()), late[late.size() / 2], late.back());
        QVERIFY(exact >= int(cuts.size()) - 1);
#ifndef KK_SANITIZED
        QVERIFY2(late[late.size() / 2] <= 50.0, qPrintable(QString::number(late[late.size() / 2])));
#endif
    }

    /// 1.30, 1.33: a row of short clips: the look-ahead sees past the next clip, within the
    /// limits.
    void manyShortClips()
    {
        Project project;
        const std::vector<int64_t> cuts = buildCuts(project, {m_a, m_b}, 20, 12, true, false);
        EngineFixture f(project);
        QVERIFY(QTest::qWaitFor([&] { return f.engine->currentFrame() != nullptr; }, 10000));
        f.engine->play();
        QVERIFY(QTest::qWaitFor([&] { return !f.engine->isPlaying(); }, 30000));
        const auto stats = f.compositor->prepareStats();
        qInfo("started %lld ready %lld hits %lld waited %lld expired %lld peak %d",
            (long long)stats.started, (long long)stats.ready, (long long)stats.hitsReady,
            (long long)stats.hitsWaited, (long long)stats.expired, stats.peakReserved);
        QVERIFY(stats.started >= int64_t(cuts.size()) / 2);
        QVERIFY(stats.hitsReady + stats.hitsWaited >= int64_t(cuts.size()) / 2);
        QVERIFY(stats.peakReserved >= 2); // more than the next clip
        QVERIFY(stats.peakReserved <= DecoderPool::kMaxReserved);
        QVERIFY(stats.peakRunning <= DecoderPool::kMaxPrepareRunning);
    }

    /// 1.30: two video tracks cutting on the same frames: both next clips are ready.
    void twoTracksCutTogether()
    {
        Project project;
        const std::vector<int64_t> cuts = buildCuts(project, {m_a, m_b}, 5, 60, true, false);
        project.beginEdit(QStringLiteral("v2"));
        TimelineData& tl = project.timeline()->mutableData();
        for (ClipData c : tl.videoTracks[0].clips) { // the same cuts, other in-points
            c.id = tl.allocateId();
            c.sourceInUs += framesToUs(kGop, kRate);
            tl.videoTracks[1].clips.push_back(c);
        }
        project.endEdit();
        EngineFixture f(project);
        QVERIFY(QTest::qWaitFor([&] { return f.engine->currentFrame() != nullptr; }, 10000));
        f.engine->play();
        QVERIFY(QTest::qWaitFor([&] { return !f.engine->isPlaying(); }, 30000));
        const auto stats = f.compositor->prepareStats();
        qInfo("started %lld hits %lld waited %lld expired %lld", (long long)stats.started,
            (long long)stats.hitsReady, (long long)stats.hitsWaited, (long long)stats.expired);
        QCOMPARE(stats.started, int64_t(cuts.size() * 2));
        QVERIFY(stats.hitsReady + stats.hitsWaited >= int64_t(cuts.size() * 2) - 1);
    }

    /// 1.31: a seek or an edit while the look-ahead holds decoders cancels them; an edit starts
    /// it over for the new clips.
    void seekAndEditCancel()
    {
        Project project;
        (void)buildCuts(project, {m_a, m_b}, 12, 60, true, false);
        EngineFixture f(project);
        QVERIFY(QTest::qWaitFor([&] { return f.engine->currentFrame() != nullptr; }, 10000));
        f.engine->play();
        QVERIFY(QTest::qWaitFor([&] { return f.compositor->prepareStats().started > 0; }, 5000));

        project.timeline()->setPlayhead(420); // a ruler click while playing
        QVERIFY(QTest::qWaitFor([&] { return f.compositor->prepareStats().cancelled > 0; }, 5000));
        QVERIFY(f.engine->isPlaying());
        const int64_t cancelled = f.compositor->prepareStats().cancelled;

        // After the resume: prepared again; then an edit.
        const int64_t started = f.compositor->prepareStats().started;
        QVERIFY(QTest::qWaitFor(
            [&] { return f.compositor->prepareStats().started > started; }, 5000));
        project.beginEdit(QStringLiteral("edit"));
        auto& clips = project.timeline()->mutableData().videoTracks[0].clips;
        for (ClipData& c : clips) {
            if (c.start >= 480)
                c.sourceInUs += 100'000;
        }
        project.endEdit();
        QVERIFY(QTest::qWaitFor(
            [&] { return f.compositor->prepareStats().cancelled > cancelled; }, 5000));
        f.engine->pause();
    }

    /// 1.32: no look-ahead in reverse or at 4x; at 2x and in slow play it works.
    void shuttleSpeeds()
    {
        Project project;
        (void)buildCuts(project, {m_a, m_b}, 12, 30, true, false);
        EngineFixture f(project);
        QVERIFY(QTest::qWaitFor([&] { return f.engine->currentFrame() != nullptr; }, 10000));
        project.timeline()->setPlayhead(300);
        QTest::qWait(100);
        f.engine->setRate(-1.0);
        QTest::qWait(500);
        QVERIFY(f.engine->isPlaying());
        f.engine->pause();
        QCOMPARE(f.compositor->prepareStats().started, int64_t(0));
        project.timeline()->setPlayhead(0);
        QTest::qWait(100);
        f.engine->setRate(4.0);
        QTest::qWait(500);
        f.engine->pause();
        QCOMPARE(f.compositor->prepareStats().started, int64_t(0));
        project.timeline()->setPlayhead(0);
        QTest::qWait(100);
        f.engine->setRate(2.0);
        QVERIFY(QTest::qWaitFor([&] { return f.compositor->prepareStats().started > 0; }, 3000));
        f.engine->pause();
    }

    /// 1.30: a loop's start is prepared too, for the jump back within one clip.
    void loopWrapIsPrepared()
    {
        Project project;
        (void)buildCuts(project, {m_a, m_b}, 1, 60, false, false);
        EngineFixture f(project);
        QVERIFY(QTest::qWaitFor([&] { return f.engine->currentFrame() != nullptr; }, 10000));
        f.engine->setLoop(true);
        f.engine->play();
        QVERIFY(QTest::qWaitFor([&] { return f.compositor->prepareStats().hitsReady >= 1; }, 8000));
        f.engine->pause();
    }
};

QTEST_MAIN(TestCutLookAhead)
#include "test_cutlookahead.moc"
