// SPDX-License-Identifier: GPL-3.0-or-later
// Render cache: chunk keys change exactly when the pictures of a chunk can change (property
// change, trim, effect change, media relink, track switch, transition neighbours, nested
// timelines) and stay put otherwise (rename, unrelated clips); Smart / User selection; the
// background manager renders chunks, the Compositor shows cached chunks only for the
// snapshot they were made for, and a cache hit looks like the uncached render.
#include "ExportTestUtil.h"
#include "core/EditOps.h"
#include "core/TitleLayers.h"
#include "export/RenderCacheManager.h"
#include "render/Compositor.h"

#include <QSettings>
#include <QSignalSpy>

using namespace kk;
using namespace kk::test;

namespace {

constexpr int kTimeoutMs = 180'000;

/// V1: solid [0, 50) + solid [50, 100) (cut at 50); V2: title [40, 60). 25 fps -> chunks
/// [0,25) [25,50) [50,75) [75,100).
TimelineData keyTimeline()
{
    TimelineData tl = emptyTimeline(320, 180, {25, 1});
    addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::red, 0, 50));
    addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::blue, 50, 50));
    addClip(tl, TrackType::Video, 1, titleClip(tl, QStringLiteral("T"), 40, 20));
    return tl;
}

QVector<QByteArray> chunkKeys(const TimelineData& tl, const MediaKeyMap& media = {},
    RenderCacheMode mode = RenderCacheMode::Smart)
{
    CacheKeyOptions o;
    o.mode = mode;
    QVector<QByteArray> out;
    for (const CacheChunk& c : renderCacheChunks(tl, media, o))
        out.append(c.key);
    return out;
}

/// Indices of the chunks whose key differs.
QVector<int> changed(const QVector<QByteArray>& a, const QVector<QByteArray>& b)
{
    QVector<int> out;
    for (int i = 0; i < std::max(a.size(), b.size()); ++i) {
        if (i >= a.size() || i >= b.size() || a[i] != b[i])
            out.append(i);
    }
    return out;
}

ClipData& clipAt(TimelineData& tl, int track, int index)
{
    return tl.videoTracks[size_t(track)].clips[size_t(index)];
}

double meanDifference(const QImage& a, const QImage& b, int* worst, double* psnr)
{
    *worst = 255;
    if (a.size() != b.size())
        return 255;
    *worst = 0;
    double sum = 0;
    double squares = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            // The cache is opaque (rendered over black): compare the premultiplied RGB.
            const QRgb p = a.pixel(x, y);
            const QRgb q = b.pixel(x, y);
            const int d[3] = {std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                std::abs(qBlue(p) - qBlue(q))};
            sum += d[0] + d[1] + d[2];
            // PSNR of the luma: 4:2:2 intermediates smear chroma across the hard edges of
            // saturated synthetic colour bars (large RGB errors there, invisible in practice).
            const int dl = qGray(p) - qGray(q);
            squares += double(dl * dl);
            *worst = std::max({*worst, d[0], d[1], d[2]});
        }
    }
    const double n = 3.0 * a.width() * a.height();
    *psnr = squares > 0 ? 10.0 * std::log10(255.0 * 255.0 / (squares / (n / 3.0))) : 99.0;
    return sum / n;
}

} // namespace

class TestRenderCache : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_rendercache"));
        QSettings().clear();
    }

    void chunkGrid()
    {
        const TimelineData tl = keyTimeline();
        CacheKeyOptions o;
        const std::vector<CacheChunk> chunks = renderCacheChunks(tl, {}, o);
        QCOMPARE(int(chunks.size()), 4);
        QCOMPARE(chunks[0].start, 0);
        QCOMPARE(chunks[3].end, 100);
        QCOMPARE(chunks[0].key.size(), 40);
        // Smart: only the chunks where the title is composited over V1.
        QVERIFY(!chunks[0].wanted);
        QVERIFY(chunks[1].wanted && (chunks[1].reasons & CacheComposite));
        QVERIFY(chunks[2].wanted);
        QVERIFY(!chunks[3].wanted);
        // None: nothing wanted; empty timeline: no chunks.
        o.mode = RenderCacheMode::None;
        for (const CacheChunk& c : renderCacheChunks(tl, {}, o))
            QVERIFY(!c.wanted);
        QVERIFY(renderCacheChunks(emptyTimeline(), {}, o).empty());
        // Keys do not depend on the mode (switching modes keeps the cache).
        QCOMPARE(chunkKeys(tl, {}, RenderCacheMode::User), chunkKeys(tl));
    }

    void propertyChangeInvalidatesOnlyItsChunks()
    {
        TimelineData tl = keyTimeline();
        const auto before = chunkKeys(tl);
        clipAt(tl, 1, 0).transform.setBase(keys::Opacity, 50.0); // title [40, 60)
        QCOMPARE(changed(before, chunkKeys(tl)), (QVector<int> {1, 2}));
        // Keyframe on the red solid [0, 50): chunks 0, 1 (+2: a clip's data reaches its
        // neighbours' chunks only through transitions; there is none yet).
        TimelineData k = keyTimeline();
        clipAt(k, 0, 0).transform.animated(keys::PosX, 0).setKey(10, 20.0);
        QCOMPARE(changed(before, chunkKeys(k)), (QVector<int> {0, 1}));
    }

    // Spec 68.14: the layers of a title are part of its cache key.
    void titleLayersChangeTheKey()
    {
        TimelineData tl = keyTimeline();
        const auto before = chunkKeys(tl);
        // A layer added, then changed, then hidden: each changes the chunks of the title.
        TimelineData added = tl;
        const quint64 box = titlelayers::addLayer(clipAt(added, 1, 0), TitleLayerKind::Shape);
        QCOMPARE(changed(before, chunkKeys(added)), (QVector<int> {1, 2}));
        TimelineData tinted = added;
        titlelayers::params(clipAt(tinted, 1, 0), box)->setStatic(keys::Color, QColor(0, 255, 0));
        QCOMPARE(changed(chunkKeys(added), chunkKeys(tinted)), (QVector<int> {1, 2}));
        TimelineData keyed = added;
        titlelayers::params(clipAt(keyed, 1, 0), box)->animated(keys::TextPosX, 0.5).setKey(3, 0.8);
        QCOMPARE(changed(chunkKeys(added), chunkKeys(keyed)), (QVector<int> {1, 2}));
        TimelineData hidden = added;
        titlelayers::setVisible(clipAt(hidden, 1, 0), box, false);
        QCOMPARE(changed(chunkKeys(added), chunkKeys(hidden)), (QVector<int> {1, 2}));
        TimelineData moved = added;
        titlelayers::moveLayer(clipAt(moved, 1, 0), titlelayers::kBaseId, +1);
        QCOMPARE(changed(chunkKeys(added), chunkKeys(moved)), (QVector<int> {1, 2}));
        // Unrelated chunks keep their keys.
        QCOMPARE(chunkKeys(added)[0], before[0]);
        QCOMPARE(chunkKeys(added)[3], before[3]);
    }

    void trimInvalidates()
    {
        TimelineData tl = keyTimeline();
        const auto before = chunkKeys(tl);
        clipAt(tl, 1, 0).duration = 30; // title now [40, 70)
        QCOMPARE(changed(before, chunkKeys(tl)), (QVector<int> {1, 2}));
        TimelineData slip = keyTimeline();
        clipAt(slip, 1, 0).sourceInUs = 40'000; // a slip changes the content too
        QCOMPARE(changed(before, chunkKeys(slip)), (QVector<int> {1, 2}));
        TimelineData moved = keyTimeline();
        clipAt(moved, 1, 0).start = 80; // moved to [80, 100): old and new chunks
        QCOMPARE(changed(before, chunkKeys(moved)), (QVector<int> {1, 2, 3}));
    }

    void effectChangeInvalidates()
    {
        TimelineData tl = keyTimeline();
        const auto before = chunkKeys(tl);
        EffectInstance blur = EffectRegistry::instance().createEffect(
            QStringLiteral("gaussian_blur"), tl.allocateId());
        clipAt(tl, 0, 1).effects.append(blur); // blue solid [50, 100)
        const auto withEffect = chunkKeys(tl);
        QCOMPARE(changed(before, withEffect), (QVector<int> {2, 3}));
        // An effect makes the chunk heavy (Smart).
        CacheKeyOptions o;
        QVERIFY(renderCacheChunks(tl, {}, o)[3].reasons & CacheEffects);
        QVERIFY(renderCacheChunks(tl, {}, o)[3].wanted);
        // Changing a parameter, disabling the effect.
        clipAt(tl, 0, 1).effects[0].params.setBase(QStringLiteral("radius"), 33.0);
        QCOMPARE(changed(withEffect, chunkKeys(tl)), (QVector<int> {2, 3}));
        const auto changedParam = chunkKeys(tl);
        clipAt(tl, 0, 1).effects[0].enabled = false;
        QCOMPARE(changed(changedParam, chunkKeys(tl)), (QVector<int> {2, 3}));
    }

    void irrelevantChangesKeepTheCache()
    {
        TimelineData tl = keyTimeline();
        const auto before = chunkKeys(tl);
        clipAt(tl, 0, 0).name = QStringLiteral("renamed");
        clipAt(tl, 0, 0).label = QColor(Qt::green);
        clipAt(tl, 0, 0).markers.append(Marker {3, QStringLiteral("m"), QColor(Qt::red), {}});
        clipAt(tl, 0, 0).audio.setBase(keys::VolumeDb, -6.0);
        clipAt(tl, 0, 0).cacheOutput = true;
        tl.markers.append(Marker {7, QStringLiteral("timeline marker"), QColor(Qt::red), {}});
        tl.markIn = 5;
        tl.audioTracks[0].muted = true;
        QCOMPARE(chunkKeys(tl), before);
    }

    void trackAndFormatChanges()
    {
        TimelineData tl = keyTimeline();
        const auto before = chunkKeys(tl);
        tl.videoTracks[1].enabled = false;
        QCOMPARE(changed(before, chunkKeys(tl)), (QVector<int> {0, 1, 2, 3}));
        TimelineData big = keyTimeline();
        big.width = 640;
        QCOMPARE(changed(before, chunkKeys(big)).size(), 4);
        CacheKeyOptions o;
        o.format = QStringLiteral("prores_proxy");
        const TimelineData same = keyTimeline();
        QVERIFY(renderCacheChunks(same, {}, o)[0].key != before[0]);
    }

    void transitionNeighbours()
    {
        // A dissolve at the cut (50) shows the red clip's handle after 50 and the blue clip
        // before 50: changing either one invalidates both chunks around the cut.
        TimelineData tl = keyTimeline();
        clipAt(tl, 0, 1).transitionIn
            = EffectRegistry::instance().createTransition(QStringLiteral("cross_dissolve"), 10);
        const auto before = chunkKeys(tl);
        CacheKeyOptions o;
        QVERIFY(renderCacheChunks(tl, {}, o)[1].reasons & CacheTransition);
        QVERIFY(renderCacheChunks(tl, {}, o)[2].reasons & CacheTransition);
        TimelineData a = tl;
        clipAt(a, 0, 0).transform.setBase(keys::ZoomX, 2.0); // the outgoing clip
        QVERIFY(changed(before, chunkKeys(a)).contains(2));
        TimelineData b = tl;
        clipAt(b, 0, 1).transform.setBase(keys::ZoomX, 2.0); // the incoming clip
        QVERIFY(changed(before, chunkKeys(b)).contains(1));
        TimelineData d = tl;
        clipAt(d, 0, 1).transitionIn->duration = 20;
        // Every chunk the incoming clip's (longer) influence reaches.
        QCOMPARE(changed(before, chunkKeys(d)), (QVector<int> {1, 2, 3}));
    }

    void retimeInvalidates()
    {
        TimelineData tl = keyTimeline();
        const auto before = chunkKeys(tl);
        TimelineData slow = tl;
        clipAt(slow, 0, 1).speed = 0.5; // blue solid [50, 100) at half speed
        QCOMPARE(changed(before, chunkKeys(slow)), (QVector<int> {2, 3}));
        CacheKeyOptions o;
        QVERIFY(renderCacheChunks(slow, {}, o)[3].reasons & CacheRetime);
        QVERIFY(renderCacheChunks(slow, {}, o)[3].wanted); // Smart caches speed changes
        TimelineData blend = slow;
        clipAt(blend, 0, 1).retimeProcess = RetimeProcess::FrameBlend;
        QCOMPARE(changed(chunkKeys(slow), chunkKeys(blend)), (QVector<int> {2, 3}));
        TimelineData project = slow; // the project default process: every chunk
        project.retimeProcess = RetimeProcess::OpticalFlow;
        QCOMPARE(changed(chunkKeys(slow), chunkKeys(project)).size(), 4);
    }

    void mediaRelinkInvalidates()
    {
        TimelineData tl = keyTimeline();
        ClipData m = mediaClip(tl, QStringLiteral("m1"), 0, 25);
        tl.videoTracks[2 - 1].clips.insert(tl.videoTracks[1].clips.begin(), m); // V2 [0, 25)
        MediaKeyMap media;
        media.insert(QStringLiteral("m1"), QByteArrayLiteral("file:/a.mp4:10:1000"));
        const auto before = chunkKeys(tl, media);
        MediaKeyMap relinked = media;
        relinked.insert(QStringLiteral("m1"), QByteArrayLiteral("file:/b.mp4:10:1000"));
        QCOMPARE(changed(before, chunkKeys(tl, relinked)), (QVector<int> {0}));
        MediaKeyMap replaced = media; // same path, the file was overwritten
        replaced.insert(QStringLiteral("m1"), QByteArrayLiteral("file:/a.mp4:12:2000"));
        QCOMPARE(changed(before, chunkKeys(tl, replaced)), (QVector<int> {0}));

        // makeMediaKeys: fingerprint of the real file.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("a.bin"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("1234");
        f.close();
        MediaPoolState pool;
        MediaItem item;
        item.id = QStringLiteral("m1");
        item.path = path;
        pool.items.append(item);
        const QByteArray k1 = makeMediaKeys(pool).value(QStringLiteral("m1"));
        QVERIFY(k1.startsWith("file:"));
        QVERIFY(f.open(QIODevice::Append));
        f.write("5");
        f.close();
        QVERIFY(makeMediaKeys(pool).value(QStringLiteral("m1")) != k1);
        QFile::remove(path);
        QVERIFY(makeMediaKeys(pool).value(QStringLiteral("m1")).startsWith("offline:"));
    }

    void nestedTimelineInvalidates()
    {
        TimelineData inner = emptyTimeline(320, 180, {25, 1});
        inner.uid = 7;
        addClip(inner, TrackType::Video, 0, solidClip(inner, Qt::green, 0, 100));
        TimelineData outer = emptyTimeline(320, 180, {25, 1});
        outer.uid = 1;
        ClipData n;
        n.id = outer.allocateId();
        n.kind = ClipKind::Nested;
        n.nestedUid = 7;
        n.start = 0;
        n.duration = 50;
        n.sourceDurationUs = 4'000'000;
        addClip(outer, TrackType::Video, 0, n);
        auto withInner = [&outer](const TimelineData& in) {
            TimelineData copy = outer;
            auto map = std::make_shared<NestedTimelineMap>();
            map->insert(in.uid, std::make_shared<const TimelineData>(in));
            copy.nested.map = std::move(map);
            return copy;
        };
        const auto before = chunkKeys(withInner(inner));
        CacheKeyOptions o;
        QVERIFY(renderCacheChunks(withInner(inner), {}, o)[0].reasons & CacheNested);
        TimelineData edited = inner;
        edited.videoTracks[0].clips[0].solidColor = Qt::yellow;
        QCOMPARE(changed(before, chunkKeys(withInner(edited))), (QVector<int> {0, 1}));
    }

    void userMode()
    {
        TimelineData tl = keyTimeline();
        CacheKeyOptions o;
        o.mode = RenderCacheMode::User;
        for (const CacheChunk& c : renderCacheChunks(tl, {}, o))
            QVERIFY(!c.wanted); // composites are not cached in User mode by default
        o.userComposites = true;
        QVERIFY(renderCacheChunks(tl, {}, o)[1].wanted);
        o.userComposites = false;
        clipAt(tl, 0, 1).cacheOutput = true; // Render Cache Clip Output on the blue solid
        const std::vector<CacheChunk> c = renderCacheChunks(tl, {}, o);
        QVERIFY(!c[0].wanted && !c[1].wanted && c[2].wanted && c[3].wanted);
    }

    void backgroundCacheAndHit()
    {
        QString why;
        Project project;
        if (!buildExportProject(project, &why))
            QSKIP(qPrintable(why));
        QTemporaryDir dir;
        Compositor compositor;
        RenderCacheManager cache(&project, dir.path());
        cache.setCompositor(&compositor);
        QVERIFY(cache.ranges().isEmpty()); // mode None
        MediaCacheSettings s = project.cacheSettings();
        s.renderCacheMode = RenderCacheMode::Smart;
        s.backgroundCache = false; // explicit "render now" below
        if (!qEnvironmentVariableIsEmpty("KK_CACHE_FORMAT"))
            s.renderCacheFormat = qEnvironmentVariable("KK_CACHE_FORMAT");
        project.setCacheSettings(s);
        cache.refresh();
        QCOMPARE(int(cache.chunks().size()), 2); // 50 frames, 25 per chunk
        QVERIFY(!cache.ranges().isEmpty());
        QCOMPARE(cache.ranges().front().state, RenderCacheManager::RangeState::Uncached);
        QVERIFY(!cache.isCached(5));

        QSignalSpy viewChanged(&cache, &RenderCacheManager::viewChanged);
        cache.renderAllNow();
        QVERIFY(cache.waitForIdle(kTimeoutMs));
        QVERIFY(cache.isCached(0) && cache.isCached(49));
        QVERIFY(viewChanged.count() >= 2);
        QCOMPARE(cache.ranges().size(), 1);
        QCOMPARE(cache.ranges().front().state, RenderCacheManager::RangeState::Cached);
        for (const CacheChunk& c : cache.chunks())
            QVERIFY(QFile::exists(renderCacheFile(dir.path(), c.key)));

        // Cache hit renders the same picture as the uncached path (within the codec's
        // tolerance) and says so.
        const auto snapshot = project.timeline()->snapshot();
        const auto media = makeMediaPathMap(project.mediaPool()->state());
        for (const int64_t frame : {0, 12, 30, 35, 49}) {
            RenderRequest plain {snapshot, media, frame, {}, Quality::Full, OutputImage};
            RenderRequest cached = plain;
            cached.renderCache = cache.view();
            const RenderedFramePtr a = compositor.renderAsync(plain).get();
            const RenderedFramePtr b = compositor.renderAsync(cached).get();
            QVERIFY(a && a->ok && b && b->ok);
            QVERIFY(!a->fromCache);
            QVERIFY(b->fromCache);
            int worst = 0;
            double psnr = 0;
            const double mean = meanDifference(a->image, b->image, &worst, &psnr);
            if (!qEnvironmentVariableIsEmpty("KK_DUMP")) {
                a->image.save(
                    QStringLiteral("%1/a%2.png").arg(qEnvironmentVariable("KK_DUMP")).arg(frame));
                b->image.save(
                    QStringLiteral("%1/b%2.png").arg(qEnvironmentVariable("KK_DUMP")).arg(frame));
            }
            qInfo("frame %d: cached vs uncached mean %.2f, max %d, luma PSNR %.1f dB", int(frame),
                mean, worst, psnr);
            // DNxHR LB (the default cache format, chosen for decode speed) is 8-bit 4:2:2 at
            // ~36 Mbit/s for 1080p30. On this noisy synthetic 320x180 content: mean 2.9-4.2,
            // luma PSNR 34-37 dB (DNxHR SQ / ProRes 422: 2.1-3.4, 35-39 dB; set
            // KK_CACHE_FORMAT=dnxhr_sq to compare). Any mismatch in position, timing or
            // compositing would be far beyond these bounds.
            QVERIFY2(mean < 5.0 && psnr > 32.0,
                qPrintable(QStringLiteral("frame %1: mean %2, max %3, PSNR %4")
                               .arg(frame)
                               .arg(mean)
                               .arg(worst)
                               .arg(psnr)));
        }

        // An edit: the old view never applies to the new snapshot; the edited chunk is
        // uncached, the other one keeps its file (content-addressed).
        const auto oldView = cache.view();
        project.beginEdit(QStringLiteral("opacity"));
        project.timeline()->mutableData().videoTracks[1].clips[0].transform.setBase(
            keys::Opacity, 40.0); // title [10, 40): both chunks
        project.endEdit();
        const auto edited = project.timeline()->snapshot();
        QVERIFY(edited != snapshot);
        RenderRequest stale {edited, media, 12, {}, Quality::Full, OutputImage};
        stale.renderCache = oldView;
        QVERIFY(!compositor.renderAsync(stale).get()->fromCache);
        cache.refresh();
        QVERIFY(!cache.isCached(12));

        // Undo returns to the cached content: same keys, cache hit again.
        project.undo();
        cache.refresh();
        QVERIFY(cache.isCached(12) && cache.isCached(40));

        // Moving the playhead / pausing does not touch the cache; deleteAll() empties it.
        cache.deleteAll();
        QVERIFY(!cache.isCached(0));
        QCOMPARE(QDir(dir.path()).entryList({QStringLiteral("*.mov")}, QDir::Files).size(), 0);
    }

    void pauseAbortsAndBackgroundStarts()
    {
        QString why;
        Project project;
        if (!buildExportProject(project, &why))
            QSKIP(qPrintable(why));
        QTemporaryDir dir;
        Compositor compositor;
        RenderCacheManager cache(&project, dir.path());
        cache.setCompositor(&compositor);
        MediaCacheSettings s = project.cacheSettings();
        s.renderCacheMode = RenderCacheMode::Smart;
        s.backgroundCache = true;
        s.backgroundCacheDelaySec = 0; // start as soon as the event loop is idle
        project.setCacheSettings(s);
        QTRY_VERIFY_WITH_TIMEOUT(cache.isRendering() || cache.isCached(0), 10'000);
        cache.setPaused(true); // playback starts: the chunk in progress is abandoned
        QTRY_VERIFY_WITH_TIMEOUT(!cache.isRendering(), 30'000);
        QVERIFY(
            QDir(dir.path()).entryList({QStringLiteral("*.partial.mov")}, QDir::Files).isEmpty());
        cache.setPaused(false);
        QTRY_VERIFY_WITH_TIMEOUT(cache.isCached(0) && cache.isCached(49), kTimeoutMs);
    }

    void cleanupTestCase() { QSettings().clear(); }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestRenderCache>(argc, argv);
}

#include "test_rendercache.moc"
