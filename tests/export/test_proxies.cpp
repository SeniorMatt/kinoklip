// SPDX-License-Identifier: GPL-3.0-or-later
// Proxy / optimized media end to end: generation through the background queue (progress,
// cancel, linking, sidecar), the choice of original vs proxy for playback (Proxy Handling,
// offline originals, stale proxies), frame-exact and geometry-preserving rendering with a
// proxy, and export always using the camera originals unless proxies are asked for.
#include "ExportTestUtil.h"
#include "export/ExportJob.h"
#include "export/ProxyQueue.h"
#include "render/Compositor.h"
#include "render/ProxySelection.h"

#include <QDateTime>
#include <QSignalSpy>

using namespace kk;
using namespace kk::test;

namespace {

constexpr int kJobTimeoutMs = 120'000;

/// Mean absolute difference of the RGB channels (0..255).
double meanDifference(const QImage& a, const QImage& b)
{
    if (a.size() != b.size())
        return 255;
    double sum = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y);
            const QRgb q = b.pixel(x, y);
            sum += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q))
                + std::abs(qBlue(p) - qBlue(q));
        }
    }
    return sum / (3.0 * a.width() * a.height());
}

/// Copies fixture @p name into @p dir (tests change and delete their copy).
QString copyFixture(const QTemporaryDir& dir, const char* name)
{
    const QString to = dir.filePath(QString::fromLatin1(name));
    QFile::remove(to);
    if (!QFile::copy(fixture(name), to))
        return {};
    return to;
}

/// Imports @p path into @p project and returns the new item (invalid id on failure).
MediaItem importOne(Project& project, const QString& path)
{
    const ImportResult r = project.importMedia({path});
    if (r.importedIds.size() != 1)
        return {};
    return *project.mediaPool()->item(r.importedIds.front());
}

/// One-clip timeline of @p item at its own size and rate.
std::shared_ptr<const TimelineData> clipTimeline(const MediaItem& item, int64_t frames)
{
    TimelineData tl
        = emptyTimeline(item.info.displayWidth, item.info.displayHeight, item.info.frameRate);
    addClip(tl, TrackType::Video, 0, mediaClip(tl, item.id, 0, frames, 0, item.info.durationUs));
    return snap(tl);
}

} // namespace

class TestProxies : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    QString m_why;
    bool m_haveFixtures = false;

    /// Generates the proxy of the only item of @p project with @p settings; QVERIFY inside.
    bool generate(Project& project, const MediaItem& item, const MediaCacheSettings& settings,
        LinkKind kind, Compositor* compositor)
    {
        ProxyJobSpec spec;
        QString error;
        if (!makeProxyJob(item, kind, settings, m_dir.filePath(QStringLiteral("ProxyMedia")), &spec,
                &error)) {
            qWarning() << error;
            return false;
        }
        ProxyQueue queue(project.mediaLinks());
        queue.setCompositor(compositor);
        QSignalSpy finished(&queue, &ProxyQueue::finished);
        if (!queue.enqueue(spec))
            return false;
        if (queue.enqueue(spec))
            return false; // a second job for the same item is refused
        if (!queue.isPending(item.id, kind))
            return false;
        if (!queue.waitForIdle(kJobTimeoutMs))
            return false;
        return finished.count() == 1 && finished.front().at(2).toBool();
    }

private slots:
    void initTestCase()
    {
        m_haveFixtures = ensureFixtures(&m_why);
        QVERIFY(m_dir.isValid());
    }

    void generateLinkAndPlay()
    {
        if (!m_haveFixtures)
            QSKIP(qPrintable(m_why));
        const QString original = copyFixture(m_dir, "h264_25fps.mp4");
        Project project;
        const MediaItem item = importOne(project, original);
        QVERIFY(!item.id.isEmpty());
        Compositor compositor;
        MediaCacheSettings settings; // Auto: 1280x720 -> 640x360, H.264 fast decode
        QVERIFY(generate(project, item, settings, LinkKind::Proxy, &compositor));

        const MediaLink link = project.mediaLinks()->link(item.id, LinkKind::Proxy);
        QVERIFY(!link.isEmpty());
        QVERIFY(link.path.endsWith(stableLinkFileName(original)));
        QCOMPARE(link.size, QSize(640, 360));
        QCOMPARE(link.format, QStringLiteral("h264"));
        QVERIFY(QFile::exists(link.path + QStringLiteral(".json"))); // sidecar
        QVERIFY(!QFile::exists(link.path + QStringLiteral(".partial.mov")));
        const media::MediaInfo info = media::probe(link.path);
        QVERIFY(info.valid);
        QCOMPARE(QSize(info.width, info.height), QSize(640, 360));
        QCOMPARE(info.videoCodec, QStringLiteral("h264"));
        QCOMPARE(info.hasAudio, item.info.hasAudio);
        QVERIFY(std::abs(info.durationUs - item.info.durationUs) < 50'000);
        QCOMPARE(project.mediaLinks()->state(item.id, LinkKind::Proxy, original), LinkState::Ready);

        // Playback chooses the proxy (Prefer Proxies), not with proxies disabled, not while
        // the original is online with Prefer Camera Originals.
        const MediaPoolState& pool = project.mediaPool()->state();
        const MediaLinks& links = *project.mediaLinks();
        auto map = makeProxyMap(pool, links, ProxyHandling::PreferProxies, true);
        QCOMPARE(map->size(), 1);
        QCOMPARE(map->value(item.id).path, link.path);
        QCOMPARE(map->value(item.id).displaySize, QSizeF(1280, 720));
        QVERIFY(makeProxyMap(pool, links, ProxyHandling::Disabled, true)->isEmpty());
        QVERIFY(makeProxyMap(pool, links, ProxyHandling::PreferOriginals, true)->isEmpty());

        // Frame-exact and same geometry: frame N of the timeline shows the same source frame
        // (barcode) with and without the proxy, at the same size, nearly the same picture.
        const auto tl = clipTimeline(item, 250);
        for (const int64_t frame : {0, 1, 37, 249}) {
            RenderRequest plain {tl, makeMediaPathMap(pool), frame, {}, Quality::Full, OutputImage};
            RenderRequest proxied = plain;
            proxied.proxies = map;
            const RenderedFramePtr a = compositor.renderAsync(plain).get();
            const RenderedFramePtr b = compositor.renderAsync(proxied).get();
            QVERIFY(a && a->ok && b && b->ok);
            QCOMPARE(b->image.size(), a->image.size());
            QCOMPARE(readFrameIndex(b->image), readFrameIndex(a->image));
            QCOMPARE(readFrameIndex(a->image), int(frame));
            QVERIFY2(meanDifference(a->image, b->image) < 12.0,
                qPrintable(QString::number(meanDifference(a->image, b->image))));
        }
        // Retimed clips follow the same source mapping with the proxy (2x speed and reverse).
        for (const double speed : {2.0, -1.0}) {
            TimelineData retimed = *tl;
            ClipData& c = retimed.videoTracks[0].clips[0];
            c.speed = speed;
            c.duration = 100;
            if (speed < 0)
                c.sourceInUs = 9'000'000; // reverse plays backwards from 9 s
            const auto r = snap(retimed);
            for (const int64_t frame : {3, 40}) {
                RenderRequest plain {
                    r, makeMediaPathMap(pool), frame, {}, Quality::Full, OutputImage};
                RenderRequest proxied = plain;
                proxied.proxies = map;
                const RenderedFramePtr a = compositor.renderAsync(plain).get();
                const RenderedFramePtr b = compositor.renderAsync(proxied).get();
                QVERIFY(a && a->ok && b && b->ok);
                QVERIFY(readFrameIndex(a->image) >= 0);
                QCOMPARE(readFrameIndex(b->image), readFrameIndex(a->image));
            }
        }
        // Crop is in the original's pixels: cropping 640 px from the right of the 1280 px
        // original removes the right half with and without the proxy.
        {
            TimelineData cropped = *tl;
            cropped.videoTracks[0].clips[0].transform.setBase(keys::CropR, 640.0);
            const auto c = snap(cropped);
            RenderRequest plain {c, makeMediaPathMap(pool), 5, {}, Quality::Full, OutputImage};
            RenderRequest proxied = plain;
            proxied.proxies = map;
            const QImage a = compositor.renderAsync(plain).get()->image;
            const QImage b = compositor.renderAsync(proxied).get()->image;
            QCOMPARE(qAlpha(a.pixel(1000, 400)), 0);
            QCOMPARE(qAlpha(b.pixel(1000, 400)), 0);
            QCOMPARE(qAlpha(b.pixel(200, 400)), 255);
            QVERIFY(meanDifference(a, b) < 12.0);
        }

        // Offline original: Prefer Camera Originals falls back to the proxy for picture and
        // sound.
        const QString moved = original + QStringLiteral(".offline");
        QVERIFY(QFile::rename(original, moved));
        QCOMPARE(makeProxyMap(pool, links, ProxyHandling::PreferOriginals, true)->size(), 1);
        if (item.info.hasAudio) {
            QCOMPARE(makeAudioPathMap(pool, links, ProxyHandling::PreferOriginals)->value(item.id),
                link.path);
        }
        QCOMPARE(makeAudioPathMap(pool, links, ProxyHandling::Disabled)->value(item.id), original);
        QVERIFY(QFile::rename(moved, original));
        QCOMPARE(makeAudioPathMap(pool, links, ProxyHandling::PreferProxies)->value(item.id),
            original); // online: audio always from the original

        // Stale: the original changed after the proxy was made -> not used any more.
        QFile f(original);
        QVERIFY(f.open(QIODevice::ReadWrite));
        QVERIFY(f.setFileTime(
            QDateTime::currentDateTime().addSecs(120), QFileDevice::FileModificationTime));
        f.close();
        QCOMPARE(links.state(item.id, LinkKind::Proxy, original), LinkState::Stale);
        QVERIFY(makeProxyMap(pool, links, ProxyHandling::PreferProxies, true)->isEmpty());

        // Delete Proxy Media removes file, sidecar and link.
        ProxyQueue::deleteLinked(project.mediaLinks(), item.id, LinkKind::Proxy,
            m_dir.filePath(QStringLiteral("ProxyMedia")));
        QVERIFY(!QFile::exists(link.path));
        QVERIFY(!QFile::exists(link.path + QStringLiteral(".json")));
        QVERIFY(project.mediaLinks()->link(item.id, LinkKind::Proxy).isEmpty());
    }

    void rotatedSourceKeepsGeometry()
    {
        if (!m_haveFixtures)
            QSKIP(qPrintable(m_why));
        Project project;
        const MediaItem item = importOne(project, copyFixture(m_dir, "rotated_portrait.mp4"));
        QVERIFY(!item.id.isEmpty());
        QVERIFY(item.info.displayHeight > item.info.displayWidth); // portrait after rotation
        Compositor compositor;
        MediaCacheSettings settings;
        settings.proxyResolution = ProxyResolution::Half;
        QVERIFY(generate(project, item, settings, LinkKind::Proxy, &compositor));
        const MediaLink link = project.mediaLinks()->link(item.id, LinkKind::Proxy);
        const media::MediaInfo info = media::probe(link.path);
        // Display-equivalent: upright (no rotation flag), half of the display size.
        QCOMPARE(info.rotation, 0);
        QCOMPARE(QSize(info.displayWidth, info.displayHeight),
            proxySize(
                ProxyResolution::Half, QSize(item.info.displayWidth, item.info.displayHeight)));
        // A landscape timeline: the portrait clip is pillar-boxed the same way either way.
        TimelineData tl = emptyTimeline(640, 360, item.info.frameRate);
        addClip(tl, TrackType::Video, 0, mediaClip(tl, item.id, 0, 20, 0, item.info.durationUs));
        const auto s = snap(tl);
        auto map = makeProxyMap(project.mediaPool()->state(), *project.mediaLinks(),
            ProxyHandling::PreferProxies, true);
        RenderRequest plain {
            s, makeMediaPathMap(project.mediaPool()->state()), 3, {}, Quality::Full, OutputImage};
        RenderRequest proxied = plain;
        proxied.proxies = map;
        const QImage a = compositor.renderAsync(plain).get()->image;
        const QImage b = compositor.renderAsync(proxied).get()->image;
        QCOMPARE(qAlpha(a.pixel(10, 180)), 0); // pillar box
        QCOMPARE(qAlpha(b.pixel(10, 180)), 0);
        QVERIFY2(meanDifference(a, b) < 12.0, qPrintable(QString::number(meanDifference(a, b))));
    }

    void exportUsesOriginals()
    {
        if (!m_haveFixtures)
            QSKIP(qPrintable(m_why));
        // A "proxy" that is obviously different (another clip, linked by hand): export must
        // still show the original unless proxies are requested explicitly.
        Project project;
        const MediaItem item = importOne(project, copyFixture(m_dir, "h264_25fps.ts"));
        QVERIFY(!item.id.isEmpty());
        MediaLink fake;
        fake.path = fixture("h264_2997.mp4");
        project.mediaLinks()->setLink(item.id, LinkKind::Proxy, fake);
        auto map = makeProxyMap(project.mediaPool()->state(), *project.mediaLinks(),
            ProxyHandling::PreferProxies, true);
        QCOMPARE(map->size(), 1);

        const auto tl = clipTimeline(item, 50);
        Compositor compositor;
        auto exportFrame = [&](bool useProxies) {
            ExportSettings s;
            s.container = QStringLiteral("png_still");
            s.video.codec = QStringLiteral("png");
            s.range = RangeMode::Custom;
            s.rangeIn = 40;
            s.rangeOut = 41;
            s.outputPath = m_dir.filePath(
                useProxies ? QStringLiteral("with.png") : QStringLiteral("without.png"));
            ExportJob job(s, tl, makeMediaPathMap(project.mediaPool()->state()));
            job.setCompositor(&compositor);
            if (useProxies)
                job.setProxies(map);
            const ExportResult r = job.run();
            return r.ok ? QImage(s.outputPath) : QImage();
        };
        const QImage original = exportFrame(false);
        QVERIFY(!original.isNull());
        QCOMPARE(readFrameIndex(original), 40); // h264_25fps.ts frame 40
        const QImage proxied = exportFrame(true);
        QVERIFY(!proxied.isNull());
        // h264_2997.mp4 at 1.6 s shows frame 47 (29.97 fps): the proxy really was used.
        QCOMPARE(readFrameIndex(proxied), int(expectedFrameIndex(1'600'000, 30000, 1001)));
    }

    void cancelLeavesNothing()
    {
        if (!m_haveFixtures)
            QSKIP(qPrintable(m_why));
        Project project;
        const MediaItem item = importOne(project, copyFixture(m_dir, "h264_25fps.mp4"));
        QVERIFY(!item.id.isEmpty());
        ProxyJobSpec spec;
        MediaCacheSettings settings;
        settings.optimizedResolution = ProxyResolution::Original;
        QVERIFY(makeProxyJob(item, LinkKind::Optimized, settings,
            m_dir.filePath(QStringLiteral("Optimized")), &spec, nullptr));
        QCOMPARE(spec.settings.video.codec, QStringLiteral("dnxhr"));
        QCOMPARE(spec.link.size, QSize(item.info.displayWidth, item.info.displayHeight));
        Compositor compositor;
        ProxyQueue queue(project.mediaLinks());
        queue.setCompositor(&compositor);
        QSignalSpy finished(&queue, &ProxyQueue::finished);
        QVERIFY(queue.enqueue(spec));
        queue.cancel(item.id, LinkKind::Optimized);
        QVERIFY(queue.waitForIdle(kJobTimeoutMs));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.front().at(2).toBool(), false);
        QVERIFY(!QFile::exists(spec.partialPath));
        QVERIFY(!QFile::exists(spec.outputPath));
        QVERIFY(project.mediaLinks()->link(item.id, LinkKind::Optimized).isEmpty());

        // Stills and timelines have no proxies.
        MediaItem still = item;
        still.info.isStillImage = true;
        QString why;
        QVERIFY(!makeProxyJob(still, LinkKind::Proxy, settings, m_dir.path(), &spec, &why));
        QVERIFY(!why.isEmpty());
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestProxies>(argc, argv);
}

#include "test_proxies.moc"
