// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for proxy / optimized media links: stale detection, sidecars and automatic linking,
// the cache settings, project save / load (format 4) and the migration of format-3 files.
#include "core/MediaLinks.h"
#include "core/Project.h"
#include "core/Serialization.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;

namespace {

void writeBytes(const QString& path, const QByteArray& data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

MediaItem fileItem(const QString& id, const QString& path)
{
    MediaItem item;
    item.id = id;
    item.path = path;
    item.name = id;
    item.info.valid = true;
    item.info.hasVideo = true;
    item.info.width = item.info.displayWidth = 1920;
    item.info.height = item.info.displayHeight = 1080;
    item.info.frameRate = {25, 1};
    item.info.durationUs = 2'000'000;
    return item;
}

} // namespace

class TestMediaLinks : public QObject
{
    Q_OBJECT
private slots:
    void proxySizes()
    {
        // Choose automatically: largest of Original / Half / Quarter with at most 540 lines.
        QCOMPARE(proxySize(ProxyResolution::Auto, {1920, 1080}), QSize(960, 540));
        QCOMPARE(proxySize(ProxyResolution::Auto, {3840, 2160}), QSize(960, 540));
        QCOMPARE(proxySize(ProxyResolution::Auto, {720, 480}), QSize(720, 480));
        QCOMPARE(proxySize(ProxyResolution::Auto, {1280, 720}), QSize(640, 360));
        QCOMPARE(proxySize(ProxyResolution::Original, {1920, 1080}), QSize(1920, 1080));
        QCOMPARE(proxySize(ProxyResolution::Quarter, {1920, 1080}), QSize(480, 270));
        // Odd sizes round to even numbers (4:2:0).
        QCOMPARE(proxySize(ProxyResolution::Half, {1081, 607}), QSize(540, 304));
    }

    void stableNames()
    {
        const QString a = stableLinkFileName(QStringLiteral("/media/a/clip.mp4"));
        const QString b = stableLinkFileName(QStringLiteral("/media/b/clip.mp4"));
        QVERIFY(a.startsWith(QLatin1String("clip_")));
        QVERIFY(a.endsWith(QLatin1String(".mov")));
        QVERIFY(a != b); // same name in two folders never collides
        QCOMPARE(a, stableLinkFileName(QStringLiteral("/media/a/clip.mp4")));
    }

    void staleDetection()
    {
        QTemporaryDir dir;
        const QString original = dir.filePath(QStringLiteral("clip.mp4"));
        const QString proxy = dir.filePath(QStringLiteral("clip_proxy.mov"));
        writeBytes(original, "original");
        writeBytes(proxy, "proxy");
        MediaLinks links;
        QCOMPARE(links.state(QStringLiteral("m1"), LinkKind::Proxy, original), LinkState::None);
        MediaLink l;
        l.path = proxy;
        l.source = SourceFingerprint::of(original);
        QSignalSpy changed(&links, &MediaLinks::changed);
        links.setLink(QStringLiteral("m1"), LinkKind::Proxy, l);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(links.state(QStringLiteral("m1"), LinkKind::Proxy, original), LinkState::Ready);
        QCOMPARE(links.state(QStringLiteral("m1"), LinkKind::Optimized, original), LinkState::None);

        // The original is edited (size and time change): the proxy is stale.
        writeBytes(original, "a different, longer original");
        QFile f(original);
        QVERIFY(f.open(QIODevice::ReadWrite));
        QVERIFY(f.setFileTime(
            QDateTime::currentDateTime().addSecs(60), QFileDevice::FileModificationTime));
        f.close();
        QCOMPARE(links.state(QStringLiteral("m1"), LinkKind::Proxy, original), LinkState::Stale);

        // Offline original: the proxy stands in for it.
        QFile::remove(original);
        QCOMPARE(links.state(QStringLiteral("m1"), LinkKind::Proxy, original), LinkState::Ready);

        QFile::remove(proxy);
        QCOMPARE(links.state(QStringLiteral("m1"), LinkKind::Proxy, original), LinkState::Missing);
        links.clearLink(QStringLiteral("m1"), LinkKind::Proxy);
        QCOMPARE(links.size(), 0);
    }

    void sidecarAndAutoLink()
    {
        QTemporaryDir dir;
        const QString original = dir.filePath(QStringLiteral("clip.mp4"));
        writeBytes(original, "original");
        const QString folder = dir.filePath(QStringLiteral("ProxyMedia"));
        QVERIFY(QDir().mkpath(folder));
        const QString proxy = folder + QLatin1Char('/') + stableLinkFileName(original);
        writeBytes(proxy, "proxy");
        MediaLink l;
        l.path = proxy;
        l.source = SourceFingerprint::of(original);
        l.size = {960, 540};
        l.format = QStringLiteral("h264");
        QVERIFY(MediaLinks::writeSidecar(proxy, original, l));

        MediaPoolState pool;
        pool.items.append(fileItem(QStringLiteral("m1"), original));
        pool.items.append(
            fileItem(QStringLiteral("m2"), dir.filePath(QStringLiteral("other.mp4"))));
        MediaLinks links;
        QSignalSpy changed(&links, &MediaLinks::changed);
        QSignalSpy reset(&links, &MediaLinks::reset);
        QCOMPARE(links.autoLink(pool, LinkKind::Proxy, folder), 1);
        QCOMPARE(changed.count(), 0); // finding a proxy does not modify the project
        QCOMPARE(reset.count(), 1);
        const MediaLink found = links.link(QStringLiteral("m1"), LinkKind::Proxy);
        QCOMPARE(found.path, proxy);
        QCOMPARE(found.size, QSize(960, 540));
        QCOMPARE(found.format, QStringLiteral("h264"));
        QCOMPARE(links.state(QStringLiteral("m1"), LinkKind::Proxy, original), LinkState::Ready);
        QVERIFY(links.link(QStringLiteral("m2"), LinkKind::Proxy).isEmpty());
        // Already linked: nothing new.
        QCOMPARE(links.autoLink(pool, LinkKind::Proxy, folder), 0);
        // A sidecar for another original is not taken.
        MediaLink other;
        QVERIFY(!MediaLinks::readSidecar(proxy, dir.filePath(QStringLiteral("x.mp4")), &other));
    }

    void settingsJson()
    {
        MediaCacheSettings s;
        QCOMPARE(s.renderCacheMode, RenderCacheMode::None);
        QCOMPARE(s.proxyHandling, ProxyHandling::PreferProxies);
        QCOMPARE(MediaCacheSettings::fromJson({}), s);
        s.proxyResolution = ProxyResolution::Quarter;
        s.proxyFormat = QStringLiteral("dnxhr_lb");
        s.optimizedResolution = ProxyResolution::Half;
        s.renderCacheFormat = QStringLiteral("prores_proxy");
        s.backgroundCache = false;
        s.backgroundCacheDelaySec = 12;
        s.userCacheTransitions = true;
        s.userCacheComposites = true;
        s.proxyHandling = ProxyHandling::PreferOriginals;
        s.useOptimizedMedia = false;
        s.renderCacheMode = RenderCacheMode::User;
        QCOMPARE(MediaCacheSettings::fromJson(s.toJson()), s);
    }

    void projectRoundTrip()
    {
        QTemporaryDir dir;
        const QString original = dir.filePath(QStringLiteral("clip.mp4"));
        writeBytes(original, "original");
        Project p;
        p.beginEdit(QStringLiteral("add"));
        MediaPoolState pool = p.mediaPool()->state();
        pool.items.append(fileItem(QStringLiteral("m1"), original));
        p.mediaPool()->setState(pool);
        p.endEdit();
        MediaLink l;
        l.path = dir.filePath(QStringLiteral("proxy.mov"));
        l.source = SourceFingerprint::of(original);
        l.size = {960, 540};
        l.format = QStringLiteral("h264");
        p.mediaLinks()->setLink(QStringLiteral("m1"), LinkKind::Proxy, l);
        MediaLink o = l;
        o.path = dir.filePath(QStringLiteral("opt.mov"));
        o.format = QStringLiteral("dnxhr_lb");
        p.mediaLinks()->setLink(QStringLiteral("m1"), LinkKind::Optimized, o);
        p.mediaLinks()->setLink(QStringLiteral("gone"), LinkKind::Proxy, l); // no such item
        MediaCacheSettings s;
        s.renderCacheMode = RenderCacheMode::Smart;
        s.proxyHandling = ProxyHandling::Disabled;
        QSignalSpy settingsSpy(&p, &Project::cacheSettingsChanged);
        p.setCacheSettings(s);
        QCOMPARE(settingsSpy.count(), 1);
        QVERIFY(p.isDirty());

        // Undoing the import must not drop the links (they are not part of the undo state).
        const QString path = dir.filePath(QStringLiteral("p.kkproj"));
        QVERIFY(p.save(path));
        QVERIFY(!p.isDirty());

        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        QCOMPARE(root.value(QLatin1String("formatVersion")).toInt(), 4);
        QCOMPARE(root.value(QLatin1String("mediaLinks")).toArray().size(), 1); // "gone" dropped

        Project q;
        QVERIFY(q.load(path));
        QCOMPARE(q.mediaLinks()->link(QStringLiteral("m1"), LinkKind::Proxy), l);
        QCOMPARE(q.mediaLinks()->link(QStringLiteral("m1"), LinkKind::Optimized), o);
        QCOMPARE(q.cacheSettings(), s);
        QVERIFY(!q.isDirty());

        q.newProject();
        QCOMPARE(q.mediaLinks()->size(), 0);
        QCOMPARE(q.cacheSettings(), MediaCacheSettings {});
    }

    void linksSurviveUndo()
    {
        Project p;
        p.beginEdit(QStringLiteral("add"));
        MediaPoolState pool = p.mediaPool()->state();
        pool.items.append(fileItem(QStringLiteral("m1"), QStringLiteral("/x/clip.mp4")));
        p.mediaPool()->setState(pool);
        p.endEdit();
        MediaLink l;
        l.path = QStringLiteral("/x/proxy.mov");
        p.mediaLinks()->setLink(QStringLiteral("m1"), LinkKind::Proxy, l);
        p.undo();
        p.redo();
        QCOMPARE(p.mediaLinks()->link(QStringLiteral("m1"), LinkKind::Proxy), l);
    }

    void migratesFormat3()
    {
        // A format-3 file has neither "mediaLinks" nor "cacheSettings" nor "cacheOutput".
        QTemporaryDir dir;
        Project p;
        const QString path = dir.filePath(QStringLiteral("v3.kkproj"));
        QVERIFY(p.save(path));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        root[QLatin1String("formatVersion")] = 3;
        root.remove(QLatin1String("mediaLinks"));
        root.remove(QLatin1String("cacheSettings"));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(root).toJson());
        f.close();
        Project q;
        MediaCacheSettings s;
        s.renderCacheMode = RenderCacheMode::Smart;
        q.setCacheSettings(s);
        QVERIFY(q.load(path));
        QCOMPARE(q.cacheSettings(), MediaCacheSettings {}); // defaults, not the previous ones
        QCOMPARE(q.mediaLinks()->size(), 0);
        QVERIFY(!q.isDirty());
    }
};

QTEST_GUILESS_MAIN(TestMediaLinks)
#include "test_medialinks.moc"
