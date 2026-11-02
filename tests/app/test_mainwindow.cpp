// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen smoke test of the main window: pages, panel toggles, undo wiring, title, autosave.
#include "app/AutosaveController.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QApplication>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>

#include <memory>

using namespace kk;

namespace {

/// Everything main() creates, for one test.
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
        window = std::make_unique<MainWindow>(ctx, options);
    }

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
};

} // namespace

class TestMainWindow : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_mainwindow"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void pagesSwitch()
    {
        Fixture f;
        QCOMPARE(f.window->currentPageId(), QStringLiteral("edit"));
        f.action("view.page_media")->trigger();
        QCOMPARE(f.window->currentPageId(), QStringLiteral("media"));
        QVERIFY(f.window->setCurrentPage(QStringLiteral("deliver")));
        QVERIFY(f.action("view.page_deliver")->isChecked());
        QVERIFY(!f.window->setCurrentPage(QStringLiteral("color")));
        QVERIFY(!f.action("view.page_color")->isEnabled());
    }

    void panelTogglesFollowPage()
    {
        Fixture f;
        f.window->setCurrentPage(QStringLiteral("edit"));
        EditPage* edit = f.window->editPage();
        QAction* effects = f.action("view.toggle_effects");
        QVERIFY(effects->isEnabled());
        QVERIFY(!effects->isChecked());
        effects->trigger();
        QVERIFY(edit->isPanelVisible(PanelId::Effects));
        QVERIFY(effects->isChecked());

        f.action("view.toggle_inspector")->trigger(); // hide
        QVERIFY(!edit->isPanelVisible(PanelId::Inspector));

        f.window->setCurrentPage(QStringLiteral("deliver"));
        QVERIFY(!effects->isEnabled());
        f.window->setCurrentPage(QStringLiteral("edit"));
        QVERIFY(effects->isChecked());
        QVERIFY(!f.action("view.toggle_inspector")->isChecked());

        f.action("view.single_viewer")->trigger();
        QVERIFY(edit->isSingleViewer());
    }

    /// Alpha.2 feedback #5 (spec 24.4 - 24.6): a viewer-mode button at the right of both viewer
    /// headers, in step with View > Single Viewer Mode; single mode shows the timeline, or the
    /// source clip once one is opened; Q swaps; the choice persists.
    void spec24_4_singleViewerToggle()
    {
        {
            Fixture f;
            f.window->resize(1600, 900);
            f.window->show();
            QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
            EditPage* edit = f.window->editPage();
            QAction* single = f.action("view.single_viewer");
            QVERIFY(single->isCheckable());
            QCOMPARE(edit->viewerModeButtons().size(), qsizetype(2));
            QToolButton* button = edit->viewerModeButtons().at(1); // timeline viewer header
            QVERIFY(button->isVisible());
            QVERIFY(button->toolTip().contains(QLatin1String("Single Viewer")));
            const int viewerWidth = edit->sourceViewer()->width() + edit->timelineViewer()->width();
            button->click();
            QVERIFY(edit->isSingleViewer());
            QVERIFY(single->isChecked());
            QVERIFY(edit->sourceViewer()->isHidden());
            QVERIFY(!edit->timelineViewer()->isHidden());
            QVERIFY(button->toolTip().contains(QLatin1String("Dual Viewer")));
            QCoreApplication::processEvents();
            QVERIFY(edit->timelineViewer()->width() >= viewerWidth - 10);
            // The header button sits right of the timecode, inside the panel.
            const QPoint br = button->mapTo(edit->timelineViewer(), button->rect().topRight());
            QVERIFY(br.x() < edit->timelineViewer()->width());

            // Opening a clip shows the source viewer in place of the timeline viewer.
            MediaItem item;
            item.path = QStringLiteral("/nonexistent/clip.mp4");
            item.name = QStringLiteral("clip.mp4");
            item.info.valid = true;
            item.info.hasVideo = true;
            item.info.durationUs = 10'000'000;
            item.info.width = 640;
            item.info.height = 360;
            const QString id = f.project.mediaPool()->addItem(item);
            emit edit->mediaPool()->openInSourceViewer(id);
            QVERIFY(edit->isSingleViewer());
            QVERIFY(edit->singleViewerShowsSource());
            QVERIFY(edit->timelineViewer()->isHidden());
            // Q swaps back to the timeline.
            f.action("view.toggle_source_timeline")->trigger();
            QVERIFY(!edit->singleViewerShowsSource());
            QVERIFY(!edit->timelineViewer()->isHidden());
            // The menu entry leaves single mode again; both viewers come back.
            single->trigger();
            QVERIFY(!edit->isSingleViewer());
            QVERIFY(!edit->sourceViewer()->isHidden());
            QVERIFY(!edit->timelineViewer()->isHidden());
            single->trigger(); // single again, then persist
            f.window->close();
        }
        Fixture g;
        QVERIFY(g.window->editPage()->isSingleViewer());
        QVERIFY(g.action("view.single_viewer")->isChecked());
        QSettings().clear();
    }

    void layoutPersists()
    {
        {
            Fixture f;
            f.action("view.toggle_mixer")->trigger();
            f.window->close(); // not dirty → no prompt; saves layout
        }
        Fixture g;
        QVERIFY(g.window->editPage()->isPanelVisible(PanelId::Mixer));
        QSettings().clear();
    }

    void undoActionsAndTitle()
    {
        Fixture f;
        QAction* undo = f.action("edit.undo");
        QVERIFY(!undo->isEnabled());
        f.project.beginEdit(QStringLiteral("Add Clip"));
        ClipData clip;
        clip.duration = 10;
        editops::overwriteClip(f.project.timeline()->mutableData(), TrackRef {}, clip);
        f.project.endEdit();
        QVERIFY(undo->isEnabled());
        QVERIFY(undo->text().contains(QLatin1String("Add Clip")));
        QVERIFY(f.window->isWindowModified());
        undo->trigger();
        QCOMPARE(f.project.timeline()->data().videoTracks[0].clips.size(), size_t(0));
        QVERIFY(!f.window->isWindowModified());
        QVERIFY(f.action("edit.redo")->isEnabled());
        f.project.setName(QStringLiteral("Trip"));
        QVERIFY(f.window->windowTitle().startsWith(QLatin1String("Trip")));
    }

    void autosaveWritesCopy()
    {
        Project project;
        AutosaveController autosave(&project);
        QVERIFY(autosave.autosaveNow()); // nothing to do
        QVERIFY(!QFileInfo::exists(autosave.autosavePath()));
        project.setName(QStringLiteral("Auto/Save:Test"));
        project.beginEdit(QStringLiteral("x"));
        ClipData clip;
        clip.duration = 5;
        editops::overwriteClip(project.timeline()->mutableData(), TrackRef {}, clip);
        project.endEdit();
        QVERIFY(autosave.autosaveNow());
        QVERIFY(QFileInfo::exists(autosave.autosavePath()));
        QVERIFY(!autosave.autosavePath().contains(QLatin1Char(':')));
        QVERIFY(project.isDirty()); // autosave never clears the modified flag
        QFile::remove(autosave.autosavePath());
    }
};

QTEST_MAIN(TestMainWindow)
#include "test_mainwindow.moc"
