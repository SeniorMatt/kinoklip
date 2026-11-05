// SPDX-License-Identifier: GPL-3.0-or-later
// The View menu grouped into submenus (Switch to Page, Show Panel in Workspace, Viewer,
// Viewer Overlays) with every command keeping its id, key and check state; and where imports
// go in the real main window (Preferences > Editing > "Import new media into": Ctrl+I and the
// Media page's Add to Media Pool). Writes a screenshot when KK_SCREENSHOT_DIR is set.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "app/pages/MediaPage.h"
#include "core/MediaPool.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "platform/FileDialogs.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/mediapool/MediaBrowserPanel.h"
#include "ui/mediapool/MediaPoolPanel.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUndoStack>

#include <memory>

using namespace kk;

namespace {

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
    QMenu* menu(const char* id) const
    {
        return window->findChild<QMenu*>(QStringLiteral("menu:") + QLatin1String(id));
    }
    void show()
    {
        window->resize(1600, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }
};

QString stillPath()
{
    return QStringLiteral(KK_TEST_DATA_DIR "/generated/still.jpg");
}

} // namespace

class TestViewMenu : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_viewmenu"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }
    void init() { QSettings().clear(); }
    void cleanup() { platform::setFileDialogHandlerForTests({}); }

    // The View menu holds four submenus; each holds its commands (ids unchanged).
    void submenusHoldTheCommands()
    {
        Fixture f;
        QMenu* view = f.menu("view");
        QVERIFY(view);
        const std::pair<const char*, QStringList> groups[] = {
            {"view.pages",
                {"view.page_media", "view.page_cut", "view.page_edit", "view.page_compositing",
                    "view.page_color", "view.page_audio", "view.page_deliver"}},
            {"view.panels",
                {"view.toggle_media_pool", "view.toggle_effects", "view.toggle_edit_index",
                    "view.toggle_keyframes", "view.keyframes_panel_toggle_view",
                    "view.toggle_nodes", "view.toggle_gallery", "view.toggle_clips",
                    "view.toggle_color_timeline", "view.toggle_scopes", "view.stacked_timelines",
                    "view.toggle_mixer", "view.toggle_metadata", "view.toggle_inspector",
                    "view.show_bin_list", "view.video_scopes"}},
            {"view.viewer",
                {"view.single_viewer", "view.toggle_source_timeline", "view.multicam_viewer",
                    "view.fullscreen_viewer", "view.viewer_zoom_fit", "view.onscreen_transform",
                    "view.onscreen_masks", "view.onscreen_crop", "view.onscreen_dynamic_zoom"}},
            {"view.overlays", {"view.show_rulers", "view.grid_show"}},
        };
        for (const auto& [sub, ids] : groups) {
            QMenu* menu = f.menu(sub);
            QVERIFY2(menu, sub);
            QVERIFY2(view->actions().contains(menu->menuAction()), sub);
            for (const QString& id : ids) {
                QAction* a = f.actions.action(id);
                QVERIFY2(menu->actions().contains(a), qPrintable(id));
                QVERIFY2(!view->actions().contains(a), qPrintable(id)); // no longer flat
            }
        }
        QCOMPARE(f.menu("view.pages")->title().remove(QLatin1Char('&')),
            QStringLiteral("Switch to Page"));
        QCOMPARE(f.menu("view.panels")->title().remove(QLatin1Char('&')),
            QStringLiteral("Show Panel in Workspace"));
        // Guides, Safe Area and Grid Type stay submenus, now under Viewer Overlays.
        for (const char* sub : {"view.guides", "view.safe_area", "view.grid"})
            QVERIFY2(f.menu("view.overlays")->actions().contains(f.menu(sub)->menuAction()), sub);
        // The View menu itself is short now.
        QVERIFY(view->actions().size() <= 6);
    }

    // Keys, check states and the disabled panel toggles are unchanged: Shift+4 / Shift+5
    // still switch pages, the Color page's panels are disabled on Edit but in their submenu.
    void shortcutsAndStatesStay()
    {
        Fixture f;
        f.show();
        QCOMPARE(f.action("view.page_edit")->shortcut(), QKeySequence(QStringLiteral("Shift+4")));
        QCOMPARE(f.action("view.page_color")->shortcut(), QKeySequence(QStringLiteral("Shift+6")));
        f.window->setCurrentPage(QStringLiteral("edit"));
        QCoreApplication::processEvents();
        QTest::keyClick(f.window.get(), Qt::Key_6, Qt::ShiftModifier);
        QTRY_COMPARE(f.window->currentPageId(), QStringLiteral("color"));
        QVERIFY(f.action("view.page_color")->isChecked());
        QTest::keyClick(f.window.get(), Qt::Key_4, Qt::ShiftModifier);
        QTRY_COMPARE(f.window->currentPageId(), QStringLiteral("edit"));
        QVERIFY(f.action("view.page_edit")->isChecked());
        for (const char* id : {"view.toggle_nodes", "view.toggle_gallery", "view.toggle_scopes"}) {
            QVERIFY2(!f.action(id)->isEnabled(), id);
            QVERIFY2(f.menu("view.panels")->actions().contains(f.action(id)), id);
        }
        QVERIFY(f.action("view.toggle_inspector")->isEnabled());
        QVERIFY(f.action("view.toggle_inspector")->isCheckable());
        // The registry still files them by id (Keyboard Customization, profiles).
        QCOMPARE(
            f.actions.category(QStringLiteral("view.page_edit")), QStringLiteral("view.pages"));
        QVERIFY(f.actions.isSubmenu(QStringLiteral("view.panels")));

        if (const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR"); !dir.isEmpty()) {
            QDir().mkpath(dir);
            QMenu* view = f.menu("view");
            view->popup(f.window->mapToGlobal(QPoint(300, 30)));
            QTest::qWait(50);
            QMenu* panels = f.menu("view.panels");
            panels->popup(view->geometry().topRight());
            QTest::qWait(50);
            QPixmap shot = f.window->screen()->grabWindow(0);
            shot.save(dir + QStringLiteral("/view-menu.png"));
            panels->close();
            view->close();
        }
    }

    // Ctrl+I (File > Import Media) goes into the shown bin by default, into Master with
    // "Master bin".
    void ctrlIImportFollowsThePreference()
    {
        if (!QFileInfo::exists(stillPath()))
            QSKIP("generated test media missing");
        Fixture f;
        f.show();
        f.project.beginEdit(QStringLiteral("Add Bin"));
        const QString binId = f.project.mediaPool()->addBin(QStringLiteral("Footage"), {});
        f.project.endEdit();
        f.window->editPage()->mediaPool()->setCurrentBin(binId);
        platform::setFileDialogHandlerForTests(
            [](const platform::FileDialogRequest&) { return QStringList {stillPath()}; });
        const QString path = QFileInfo(stillPath()).absoluteFilePath();

        QTest::keyClick(f.window.get(), Qt::Key_I, Qt::ControlModifier);
        QTRY_VERIFY(f.project.mediaPool()->itemByPath(path));
        QCOMPARE(f.project.mediaPool()->itemByPath(path)->binId, binId);
        f.project.undoStack()->undo();
        QVERIFY(!f.project.mediaPool()->itemByPath(path));

        prefs::setImportIntoMasterBin(true);
        QTest::keyClick(f.window.get(), Qt::Key_I, Qt::ControlModifier);
        QTRY_VERIFY(f.project.mediaPool()->itemByPath(path));
        QCOMPARE(f.project.mediaPool()->itemByPath(path)->binId, QString());
    }

    // Media page > Media Storage > Add to Media Pool (files and folders) follows it too.
    void mediaStorageAddFollowsThePreference()
    {
        if (!QFileInfo::exists(stillPath()))
            QSKIP("generated test media missing");
        Fixture f;
        f.show();
        f.window->setCurrentPage(QStringLiteral("media"));
        QCoreApplication::processEvents();
        f.project.beginEdit(QStringLiteral("Add Bin"));
        const QString binId = f.project.mediaPool()->addBin(QStringLiteral("Footage"), {});
        f.project.endEdit();
        f.window->mediaPage()->mediaPool()->setCurrentBin(binId);
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("shot.jpg"));
        QVERIFY(QFile::copy(stillPath(), file));
        MediaBrowserPanel* browser = f.window->mediaPage()->browser();
        QVERIFY(browser);

        QVERIFY(!browser->addFolderToMediaPool(dir.path(), false).isEmpty());
        QCOMPARE(f.project.mediaPool()->itemByPath(file)->binId, binId);
        f.project.undoStack()->undo();
        QVERIFY(!f.project.mediaPool()->itemByPath(file));

        prefs::setImportIntoMasterBin(true);
        QVERIFY(!browser->addFolderToMediaPool(dir.path(), false).isEmpty());
        QCOMPARE(f.project.mediaPool()->itemByPath(file)->binId, QString());
        f.project.undoStack()->undo();

        // Selected files (Add to Media Pool button).
        prefs::setImportIntoMasterBin(false);
        browser->showFolder(dir.path());
        // The file list (not the folder tree): the view whose Select All selects files.
        QTRY_VERIFY([&] {
            for (QAbstractItemView* v : browser->findChildren<QAbstractItemView*>()) {
                v->selectAll();
                if (browser->selectedFiles().contains(file))
                    return true;
                v->clearSelection();
            }
            return false;
        }());
        QVERIFY(!browser->addSelectedToMediaPool().isEmpty());
        QCOMPARE(f.project.mediaPool()->itemByPath(file)->binId, binId);
    }
};

QTEST_MAIN(TestViewMenu)
#include "test_viewmenu.moc"
