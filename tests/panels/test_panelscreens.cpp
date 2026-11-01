// SPDX-License-Identifier: GPL-3.0-or-later
// Visual check: the full main window with real imported media, a selected animated clip,
// the effects library and the media page, saved as PNGs for review.
// Runs only when KK_DEMO_MEDIA_DIR (folder with media files, e.g. the output of
// scripts/make_test_media.sh) and KK_SCREENSHOT_DIR are set; skipped otherwise.
#include "PanelTestUtils.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/MediaPage.h"
#include "core/ParamKeys.h"
#include "ui/common/Resources.h"
#include "ui/effects/EffectActions.h"
#include "ui/mediapool/MediaBrowserPanel.h"
#include "ui/mediapool/MediaPoolPanel.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

using namespace kk;
using namespace kk::test;

class TestPanelScreens : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_panelscreens"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void mainWindowWithMedia()
    {
        const QString mediaDir = qEnvironmentVariable("KK_DEMO_MEDIA_DIR");
        const QString shotDir = qEnvironmentVariable("KK_SCREENSHOT_DIR");
        if (mediaDir.isEmpty() || shotDir.isEmpty())
            QSKIP("set KK_DEMO_MEDIA_DIR and KK_SCREENSHOT_DIR to render the screenshots");

        PanelFixture f;
        MainWindowOptions options;
        options.restoreLayout = false;
        options.saveLayout = false;
        options.autosave = false;
        MainWindow window(f.ctx, options);

        QStringList paths;
        for (const char* name : {"Harbour sunrise.mp4", "Drone pass.mp4", "broll.mp4",
                 "interview.mp4", "Bars.png", "still.jpg", "music.wav", "texture.mp4"}) {
            const QString path = QDir(mediaDir).filePath(QLatin1String(name));
            if (QFileInfo::exists(path))
                paths.append(path);
        }
        const ImportResult imported = f.project.importMedia(paths);
        QVERIFY(!imported.importedIds.isEmpty());
        f.project.beginEdit(QStringLiteral("bin"));
        f.project.mediaPool()->addBin(QStringLiteral("Interviews"));
        f.project.mediaPool()->addBin(QStringLiteral("B-Roll"));
        f.project.endEdit();

        // A clip with animated opacity and an effect, plus a title above it.
        ClipData clip;
        clip.mediaId = imported.importedIds.front();
        clip.name = f.project.mediaPool()->item(clip.mediaId)->name;
        clip.duration = 125;
        f.project.beginEdit(QStringLiteral("clip"));
        TimelineData& data = f.project.timeline()->mutableData();
        clip.transform.animated(keys::Opacity, 100).setKey(0, 0);
        clip.transform.animated(keys::Opacity, 100).setKey(25, 100);
        clip.transform.setBase(keys::ZoomX, 1.2);
        clip.transform.setBase(keys::ZoomY, 1.2);
        const quint64 id = editops::overwriteClip(data, {TrackType::Video, 0}, clip);
        f.project.endEdit();
        effectactions::applyEffect(f.project, {id}, QStringLiteral("vignette"));
        effectactions::addGeneratorClip(f.project, QStringLiteral("text_title"), 50);
        f.project.timeline()->setPlayhead(25);
        f.selection.selectClip(id);

        QVERIFY(window.setCurrentPage(QStringLiteral("edit")));
        f.actions.action(QStringLiteral("view.toggle_effects"))->trigger();
        window.resize(1920, 1080);
        window.show();
        QTest::qWait(3000); // poster frames are decoded asynchronously
        QDir().mkpath(shotDir);
        window.grab().save(QDir(shotDir).filePath(QStringLiteral("window_edit.png")));
        QVERIFY(window.setCurrentPage(QStringLiteral("media")));
        auto* browser = window.findChild<MediaBrowserPanel*>();
        QVERIFY(browser);
        browser->showFolder(mediaDir);
        auto* pool = window.mediaPage()->findChild<MediaPoolPanel*>();
        QVERIFY(pool && pool->selectMedia(imported.importedIds.front()));
        QTest::qWait(1500);
        window.grab().save(QDir(shotDir).filePath(QStringLiteral("window_media.png")));
    }
};

QTEST_MAIN(TestPanelScreens)
#include "test_panelscreens.moc"
