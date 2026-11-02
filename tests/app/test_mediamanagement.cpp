// SPDX-License-Identifier: GPL-3.0-or-later
// Application wiring of proxy / optimized media and the render cache: media pool commands
// (generate, unlink, delete) through the action registry, Playback > Proxy Handling and Use
// Proxy Media reaching the program engine, the viewer's proxy indicator state, Render Cache
// modes, Clip > Render Cache Clip Output (undoable) and the media pool's Proxy column.
#include "../render/RenderTestUtil.h"
#include "core/EditOps.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "export/ProxyQueue.h"
#include "export/RenderCacheManager.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/MediaManagement.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/mediapool/MediaPoolModel.h"

#include <QAction>
#include <QSettings>
#include <QTemporaryDir>

using namespace kk;
using namespace kk::test;

class TestMediaManagement : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_dir;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        initResources(); // action icons
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_mediamanagement"));
        QSettings().clear();
        // Keep every generated file inside the test's temporary folder.
        QSettings().setValue(
            QLatin1String(prefs::kCacheDirectory), m_dir.filePath(QStringLiteral("cache")));
    }

    void endToEnd()
    {
        QString why;
        if (!ensureFixtures(&why))
            QSKIP(qPrintable(why));
        const QString original = m_dir.filePath(QStringLiteral("clip.mp4"));
        QVERIFY(QFile::copy(fixture("h264_25fps.mp4"), original));

        Project project;
        SelectionModel selection;
        ActionRegistry actions;
        registerStandardActions(actions);
        PlaybackEngine::Options options;
        options.noAudio = true;
        PlaybackEngine engine(std::move(options));
        engine.setMediaPool(project.mediaPool());
        engine.setTimeline(project.timeline());
        AppContext ctx;
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ctx.playback = &engine;
        MediaManagement media(ctx);
        QCOMPARE(ctx.media, &media);

        const QString id = project.importMedia({original}).importedIds.value(0);
        QVERIFY(!id.isEmpty());
        const MediaItem item = *project.mediaPool()->item(id);
        project.beginEdit(QStringLiteral("place"));
        TimelineData& tl = project.timeline()->mutableData();
        tl.width = 1280;
        tl.height = 720;
        const quint64 clipId = editops::overwriteClip(
            tl, {TrackType::Video, 0}, mediaClip(tl, id, 0, 50, 0, item.info.durationUs));
        project.endEdit();

        MediaPoolModel model(&project);
        model.setLinkStatusProvider([&media](const QString& mid) { return media.status(mid); });
        connect(&media, &MediaManagement::statusChanged, &model,
            [&model](const QString& mid) { model.linkStatusChanged(mid); });
        const QModelIndex proxyCell = model.index(model.rowOf(id), MediaPoolModel::ProxyColumn);
        QCOMPARE(proxyCell.data().toString(), QStringLiteral("None"));
        QVERIFY(!engine.proxiesInUse());

        // Media pool > Generate Proxy Media on the selection.
        QAction* generate = actions.action(QStringLiteral("mediapool.generate_proxy"));
        generate->setData(QStringList {id});
        generate->trigger();
        QVERIFY(media.proxyQueue()->isPending(id, LinkKind::Proxy));
        QVERIFY(model.index(model.rowOf(id), 0)
                    .data(MediaPoolModel::LinkBadgeRole)
                    .toString()
                    .startsWith(QLatin1String("P ")));
        QVERIFY(media.proxyQueue()->waitForIdle(120'000));
        const MediaLink link = project.mediaLinks()->link(id, LinkKind::Proxy);
        QVERIFY(!link.isEmpty());
        QVERIFY(link.path.startsWith(prefs::proxyDirectory()));
        QCOMPARE(model.index(model.rowOf(id), MediaPoolModel::ProxyColumn).data().toString(),
            QStringLiteral("640x360"));
        QCOMPARE(model.index(model.rowOf(id), 0).data(MediaPoolModel::LinkBadgeRole).toString(),
            QStringLiteral("P"));

        // Playback uses it (viewer indicator on), until proxies are disabled.
        QVERIFY(engine.proxies() && engine.proxies()->contains(id));
        QVERIFY(engine.proxiesInUse());
        QVERIFY(actions.action(QStringLiteral("playback.use_proxy"))->isChecked());
        actions.action(QStringLiteral("playback.proxy_disable"))->trigger();
        QCOMPARE(project.cacheSettings().proxyHandling, ProxyHandling::Disabled);
        QVERIFY(!actions.action(QStringLiteral("playback.use_proxy"))->isChecked());
        QVERIFY(!engine.proxiesInUse());
        actions.action(QStringLiteral("playback.use_proxy"))->trigger(); // on again
        QCOMPARE(project.cacheSettings().proxyHandling, ProxyHandling::PreferProxies);
        QVERIFY(actions.action(QStringLiteral("playback.proxy_prefer"))->isChecked());
        QVERIFY(engine.proxiesInUse());
        actions.action(QStringLiteral("playback.proxy_originals"))->trigger();
        QVERIFY(!engine.proxiesInUse()); // the original is online
        actions.action(QStringLiteral("playback.proxy_prefer"))->trigger();

        // Saved and reloaded: still linked and used.
        const QString path = m_dir.filePath(QStringLiteral("p.kkproj"));
        QVERIFY(project.save(path));
        QVERIFY(project.load(path));
        QCOMPARE(project.mediaLinks()->link(id, LinkKind::Proxy), link);
        QVERIFY(engine.proxiesInUse());

        // A new project that imports the same file finds the proxy by itself (sidecar).
        project.newProject();
        QVERIFY(!engine.proxiesInUse());
        const QString again = project.importMedia({original}).importedIds.value(0);
        QCOMPARE(project.mediaLinks()->link(again, LinkKind::Proxy).path, link.path);

        // Unlink keeps the file, Delete removes it.
        QAction* unlink = actions.action(QStringLiteral("mediapool.unlink_proxy"));
        unlink->setData(QStringList {again});
        unlink->trigger();
        QVERIFY(project.mediaLinks()->link(again, LinkKind::Proxy).isEmpty());
        QVERIFY(QFile::exists(link.path));
        media.autoLink();
        QVERIFY(!project.mediaLinks()->link(again, LinkKind::Proxy).isEmpty());
        media.deleteLinked({again}, LinkKind::Proxy);
        QVERIFY(!QFile::exists(link.path));

        // Render cache modes and Clip > Render Cache Clip Output.
        actions.action(QStringLiteral("playback.render_cache_smart"))->trigger();
        QCOMPARE(project.cacheSettings().renderCacheMode, RenderCacheMode::Smart);
        actions.action(QStringLiteral("playback.render_cache_user"))->trigger();
        QCOMPARE(project.cacheSettings().renderCacheMode, RenderCacheMode::User);
        QVERIFY(actions.action(QStringLiteral("playback.render_cache_user"))->isChecked());
        project.beginEdit(QStringLiteral("place"));
        TimelineData& t2 = project.timeline()->mutableData();
        const quint64 c2 = editops::overwriteClip(
            t2, {TrackType::Video, 0}, mediaClip(t2, again, 0, 50, 0, item.info.durationUs));
        project.endEdit();
        Q_UNUSED(clipId);
        selection.setClips({c2});
        QAction* cacheOutput = actions.action(QStringLiteral("clip.render_cache_output"));
        QVERIFY(!cacheOutput->isChecked());
        cacheOutput->trigger();
        QVERIFY(editops::clipById(project.timeline()->data(), c2)->cacheOutput);
        QVERIFY(cacheOutput->isChecked());
        media.renderCache()->refresh();
        QVERIFY(!media.renderCache()->chunks().empty());
        QVERIFY(media.renderCache()->chunks().front().wanted); // User mode + flagged clip
        project.undo();
        QVERIFY(!editops::clipById(project.timeline()->data(), c2)->cacheOutput);
        QVERIFY(!cacheOutput->isChecked());
    }

    void cleanupTestCase() { QSettings().clear(); }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestMediaManagement>(argc, argv);
}

#include "test_mediamanagement.moc"
