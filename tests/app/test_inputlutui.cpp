// SPDX-License-Identifier: GPL-3.0-or-later
// Input LUT on the Color page clip thumbnails: right-click > 3D LUT sets the input LUT of the
// clip's media item (undoable), the same submenu as in the Media Pool.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/ColorPage.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/color/ColorBrowsers.h"
#include "ui/color/ColorSession.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace kk;

namespace {

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    QVector<quint64> clips;
    QString mediaId;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        MediaItem item;
        item.path = QStringLiteral("/nonexistent/lut-test.mp4");
        item.name = QStringLiteral("lut-test");
        item.info.valid = true;
        item.info.hasVideo = true;
        item.info.width = 320;
        item.info.height = 180;
        item.info.frameRate = {25, 1};
        item.info.durationUs = 20'000'000;
        project.beginEdit(QStringLiteral("Add"));
        mediaId = project.mediaPool()->addItem(item);
        TimelineData& tl = project.timeline()->mutableData();
        tl.width = 320;
        tl.height = 180;
        for (int i = 0; i < 2; ++i) {
            ClipData clip;
            clip.kind = ClipKind::Media;
            clip.mediaId = mediaId;
            clip.name = QStringLiteral("Clip %1").arg(i + 1);
            clip.start = i * 50;
            clip.duration = 50;
            clip.sourceDurationUs = 20'000'000;
            clips.append(editops::overwriteClip(tl, TrackRef {TrackType::Video, 0}, clip));
        }
        project.endEdit();
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }
    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
    void show()
    {
        window->resize(1600, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }
};

QAction* findAction(const QMenu* menu, const QString& text)
{
    for (QAction* a : menu->actions()) {
        if (a->text() == text)
            return a;
        if (a->menu()) {
            if (QAction* inner = findAction(a->menu(), text))
                return inner;
        }
    }
    return nullptr;
}

} // namespace

class TestInputLutUi : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_lut;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_inputlutui"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
        QVERIFY(m_dir.isValid());
        QDir().mkpath(m_dir.filePath(QStringLiteral("luts")));
        m_lut = m_dir.filePath(QStringLiteral("luts/Warm.cube"));
        QFile f(m_lut);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("LUT_1D_SIZE 2\n1 1 1\n0 0 0\n");
        f.close();
        QSettings().setValue(QStringLiteral("color/lutFolders"),
            QStringList {m_dir.filePath(QStringLiteral("luts"))});
    }

    void colorStripMenuSetsTheInputLutAndUndoes()
    {
        Fixture f;
        f.show();
        f.action("view.page_color")->trigger();
        QCoreApplication::processEvents();
        ColorClipStrip* strip = f.window->colorPage()->clipStrip();
        QCOMPARE(strip->count(), 2);
        const QPoint centre = strip->visualItemRect(strip->item(0)).center();
        bool hasSubmenu = false;
        QTimer::singleShot(0, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            QVERIFY(menu);
            QAction* sub = nullptr;
            for (QAction* a : menu->actions()) {
                if (a->text() == QStringLiteral("3D LUT"))
                    sub = a;
            }
            hasSubmenu = sub && sub->menu() && sub->isEnabled();
            if (hasSubmenu)
                findAction(sub->menu(), QStringLiteral("Warm"))->trigger();
            menu->close();
        });
        QContextMenuEvent menuEvent(
            QContextMenuEvent::Mouse, centre, strip->viewport()->mapToGlobal(centre));
        QApplication::sendEvent(strip->viewport(), &menuEvent);
        QVERIFY(hasSubmenu);
        QCOMPARE(f.project.mediaPool()->item(f.mediaId)->attributes.inputLut, m_lut);
        // The media item is shared: both clips of it carry the LUT.
        for (const quint64 id : f.clips)
            QCOMPARE(editops::clipById(f.project.timeline()->data(), id)->attributes.inputLut, m_lut);
        f.project.undo();
        QVERIFY(f.project.mediaPool()->item(f.mediaId)->attributes.inputLut.isEmpty());
    }
};

QTEST_MAIN(TestInputLutUi)
#include "test_inputlutui.moc"
