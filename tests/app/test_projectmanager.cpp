// SPDX-License-Identifier: GPL-3.0-or-later
// The Project Manager window (spec section 38): cards and list, search and sort, folders,
// keys, rename / duplicate / delete, broken files, New Project kinds, the File menu entry and
// the "save changes?" prompt when switching projects, and 300 projects in the window.
#include "../projects/ProjectTestUtil.h"
#include "app/MainWindow.h"
#include "app/ProjectController.h"
#include "core/Preferences.h"
#include "core/SelectionModel.h"
#include "playback/PlaybackEngine.h"
#include "projects/ProjectKinds.h"
#include "projects/ProjectLibrary.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/ProgressTask.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/projects/NewProjectDialog.h"
#include "ui/projects/ProjectCardDelegate.h"
#include "ui/projects/ProjectLibraryModel.h"
#include "ui/projects/ProjectManagerWindow.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressDialog>
#include <QRadioButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeView>

#include <memory>

using namespace kk;

namespace {

QString g_xdgRoot;

/// Clicks the button whose text contains @p text in the next modal message box.
void answerNextMessageBox(const QString& text)
{
    QTimer::singleShot(0, qApp, [text] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) {
            answerNextMessageBox(text); // not shown yet
            return;
        }
        for (QAbstractButton* b : box->buttons()) {
            if (b->text().remove(QLatin1Char('&')).contains(text)) {
                b->click();
                return;
            }
        }
        qWarning("no button %s", qPrintable(text));
        box->reject();
    });
}

/// Names of the rows in view order.
QStringList shownNames(const QAbstractItemModel* m)
{
    QStringList out;
    for (int r = 0; r < m->rowCount(); ++r)
        out << m->index(r, 0).data(Qt::EditRole).toString();
    return out;
}

struct App
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    std::unique_ptr<PlaybackEngine> playback;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    App()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        PlaybackEngine::Options options;
        options.noAudio = true;
        playback = std::make_unique<PlaybackEngine>(std::move(options));
        playback->setMediaPool(project.mediaPool());
        playback->setTimeline(project.timeline());
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ctx.playback = playback.get();
        MainWindowOptions wo;
        wo.autosave = false;
        wo.restoreLayout = false;
        wo.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, wo);
    }
};

} // namespace

class TestProjectManager : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_root;
    std::unique_ptr<ProjectLibrary> m_library;
    std::unique_ptr<ProjectManagerWindow> m_window;

    QString project(
        const QString& name, const QString& folder = {}, const test::ProjectSpec& spec = {})
    {
        const QString path = (folder.isEmpty() ? m_root : folder) + QLatin1Char('/') + name
            + QStringLiteral(".kkproj");
        if (!test::writeProject(path, spec))
            qFatal("cannot write %s", qPrintable(path));
        return path;
    }
    void open()
    {
        m_library
            = std::make_unique<ProjectLibrary>(m_root, m_dir->path() + QStringLiteral("/cache"));
        m_window = std::make_unique<ProjectManagerWindow>(m_library.get());
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
    }

private slots:
    void init()
    {
        QSettings().clear();
        m_dir = std::make_unique<QTemporaryDir>(g_xdgRoot + QStringLiteral("/pm-XXXXXX"));
        m_root = m_dir->path() + QStringLiteral("/Kinoklip Projects");
        QDir().mkpath(m_root);
    }
    void cleanup()
    {
        m_window.reset();
        m_library.reset();
        m_dir.reset();
    }

    void cardsListSearchAndSort()
    {
        project(QStringLiteral("Wedding"));
        QTest::qWait(15);
        project(QStringLiteral("alpha trailer"));
        QTest::qWait(15);
        project(QStringLiteral("Music Video"));
        QDir(m_root).mkdir(QStringLiteral("Zeta Folder"));
        open();
        QAbstractItemModel* shown = m_window->proxy();
        QVERIFY(m_window->isCardView());
        // Default: newest first, folders always first.
        QCOMPARE(shownNames(shown),
            (QStringList {QStringLiteral("Zeta Folder"), QStringLiteral("Music Video"),
                QStringLiteral("alpha trailer"), QStringLiteral("Wedding")}));
        m_window->sortBy(ProjectLibraryModel::NameColumn, Qt::AscendingOrder);
        QCOMPARE(shownNames(shown),
            (QStringList {QStringLiteral("Zeta Folder"), QStringLiteral("alpha trailer"),
                QStringLiteral("Music Video"), QStringLiteral("Wedding")}));
        m_window->sortBy(ProjectLibraryModel::NameColumn, Qt::DescendingOrder);
        QCOMPARE(shownNames(shown).first(), QStringLiteral("Zeta Folder"));
        QCOMPARE(shownNames(shown).at(1), QStringLiteral("Wedding"));
        // Search: case-insensitive substring of the name.
        m_window->setSearchText(QStringLiteral("VID"));
        QCOMPARE(shownNames(shown), QStringList {QStringLiteral("Music Video")});
        m_window->setSearchText({});
        QCOMPARE(shown->rowCount(), 4);
        // List view: same rows, sortable columns, header follows sortBy().
        m_window->setCardView(false);
        QVERIFY(m_window->listView()->isVisible());
        QCOMPARE(m_window->listView()->header()->sortIndicatorSection(),
            int(ProjectLibraryModel::NameColumn));
        m_window->listView()->sortByColumn(ProjectLibraryModel::ModifiedColumn, Qt::AscendingOrder);
        QCOMPARE(shownNames(shown).at(1), QStringLiteral("Wedding"));
        // Card size slider.
        m_window->setCardView(true);
        m_window->cardSizeSlider()->setValue(300);
        QCOMPARE(m_window->cardDelegate()->cardWidth(), 300);
        QCOMPARE(m_window->cardView()->gridSize().width(), 310);
    }

    void detailsAndBrokenCards()
    {
        test::ProjectSpec spec;
        spec.width = 1280;
        spec.height = 720;
        spec.fps = {50, 1};
        spec.clipFrames = 100;
        project(QStringLiteral("Good"), {}, spec);
        QFile bad(m_root + QStringLiteral("/Broken.kkproj"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("{ this is not json");
        bad.close();
        QFile single(m_root + QStringLiteral("/Packed.kinoklip")); // empty: not a container
        QVERIFY(single.open(QIODevice::WriteOnly));
        single.close();
        QString error;
        QVERIFY(projectkinds::createKinoklip(
            m_root + QStringLiteral("/Real.kinoklip"), QStringLiteral("Real"), &error));
        open();
        ProjectLibraryModel* model = m_window->model();
        QCOMPARE(model->rowCount(), 4);
        m_window->grab(); // paints every card: schedules the metadata reads
        m_library->waitForIdle();
        const QModelIndex good
            = model->index(model->rowOf(m_root + QStringLiteral("/Good.kkproj")), 0);
        const QModelIndex broken
            = model->index(model->rowOf(m_root + QStringLiteral("/Broken.kkproj")), 0);
        const QModelIndex packed
            = model->index(model->rowOf(m_root + QStringLiteral("/Packed.kinoklip")), 0);
        QVERIFY(good.data(ProjectLibraryModel::ErrorRole).toString().isEmpty());
        QCOMPARE(good.data(ProjectLibraryModel::DetailsRole).toString(),
            QStringLiteral("00:00:02:00  ·  1280 x 720  ·  50 fps"));
        QCOMPARE(good.siblingAtColumn(ProjectLibraryModel::ResolutionColumn).data().toString(),
            QStringLiteral("1280 x 720"));
        QCOMPARE(
            good.data(ProjectLibraryModel::KindBadgeRole).toString(), QStringLiteral("Linked"));
        QVERIFY(!broken.data(ProjectLibraryModel::ErrorRole).toString().isEmpty());
        QCOMPARE(packed.data(ProjectLibraryModel::KindBadgeRole).toString(),
            QStringLiteral("Single file"));
        QVERIFY(!packed.data(ProjectLibraryModel::ErrorRole).toString().isEmpty());
        // A real single-file project gets a normal card with its details and badge.
        const QModelIndex real
            = model->index(model->rowOf(m_root + QStringLiteral("/Real.kinoklip")), 0);
        QVERIFY2(real.data(ProjectLibraryModel::ErrorRole).toString().isEmpty(),
            qPrintable(real.data(ProjectLibraryModel::ErrorRole).toString()));
        QCOMPARE(real.data(ProjectLibraryModel::KindBadgeRole).toString(),
            QStringLiteral("Single file"));
        QVERIFY(real.data(ProjectLibraryModel::DetailsRole)
                    .toString()
                    .contains(QStringLiteral("1920 x 1080")));
        QVERIFY(!m_window->grab().isNull()); // error cards paint
        // Opening the broken one fails in the controller, not here: the request is made.
        QSignalSpy requested(m_window.get(), &ProjectManagerWindow::openRequested);
        m_window->openPath(broken.data(ProjectLibraryModel::PathRole).toString());
        QCOMPARE(requested.size(), 1);
    }

    void missingMediaIsMarked()
    {
        const QString mediaFile = QStringLiteral(KK_TEST_DATA_DIR "/generated/h264_25fps.mp4");
        if (!QFileInfo::exists(mediaFile))
            QSKIP("test media missing (tests/data/generated)");
        test::ProjectSpec spec;
        spec.mediaFile = mediaFile;
        const QString p = project(QStringLiteral("Offline"), {}, spec);
        QVERIFY(test::redirectMedia(p, m_dir->path() + QStringLiteral("/gone.mp4")));
        open();
        m_window->grab();
        m_library->waitForIdle();
        QCOMPARE(
            m_window->model()->index(0, 0).data(ProjectLibraryModel::MissingMediaRole).toInt(), 1);
    }

    void keysOpenRenameDelete()
    {
        const QString a = project(QStringLiteral("Alpha"));
        const QString b = project(QStringLiteral("Beta"));
        open();
        QListView* view = m_window->cardView();
        view->setFocus();
        QSignalSpy requested(m_window.get(), &ProjectManagerWindow::openRequested);
        QVERIFY(m_window->selectPath(a));
        QTest::keyClick(view, Qt::Key_Return);
        QCOMPARE(requested.size(), 1);
        QCOMPARE(requested.last().first().toString(), a);
        // The switch guard (the editor's "save changes?") can stop it.
        m_window->setSwitchGuard([](QWidget*) { return false; });
        QTest::keyClick(view, Qt::Key_Enter);
        QCOMPARE(requested.size(), 1);
        m_window->setSwitchGuard({});
        // F2: inline editor on the name; typing a new name renames file and project.
        QVERIFY(m_window->selectPath(b));
        QTest::keyClick(view, Qt::Key_F2);
        auto* editor = view->findChild<QLineEdit*>();
        QVERIFY(editor);
        QTRY_VERIFY(editor->isVisible());
        editor->setText(QStringLiteral("Gamma"));
        QTest::keyClick(editor, Qt::Key_Return);
        const QString gamma = m_root + QStringLiteral("/Gamma.kkproj");
        QTRY_VERIFY(QFileInfo::exists(gamma));
        QVERIFY(!QFileInfo::exists(b));
        // Duplicate, cancelled in its progress dialog: nothing is left behind.
        QVERIFY(m_window->selectPath(gamma));
        setProgressDialogHookForTests([](QProgressDialog* d) { d->cancel(); });
        m_window->duplicateSelected();
        setProgressDialogHookForTests({});
        QVERIFY(!QFileInfo::exists(m_root + QStringLiteral("/Gamma copy.kkproj")));
        QVERIFY(!QFileInfo::exists(m_root + QStringLiteral("/Gamma copy.kkproj.part")));
        // Duplicate, with the card selected by a click like a user does (the card view selects
        // a single cell, not a whole row; owner report on 0.4.0).
        view->clearSelection();
        QModelIndex card;
        for (int r = 0; r < view->model()->rowCount(); ++r) {
            const QModelIndex i = view->model()->index(r, 0);
            if (i.data(ProjectLibraryModel::PathRole).toString() == gamma)
                card = i;
        }
        QVERIFY(card.isValid());
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, view->visualRect(card).center());
        QVERIFY(view->selectionModel()->isSelected(card));
        m_window->duplicateSelected();
        QVERIFY(QFileInfo::exists(m_root + QStringLiteral("/Gamma copy.kkproj")));
        // Delete: asks, then moves to the (isolated) trash; Cancel keeps it.
        QVERIFY(m_window->selectPath(gamma));
        view->setFocus();
        answerNextMessageBox(QStringLiteral("Cancel"));
        QTest::keyClick(view, Qt::Key_Delete);
        QVERIFY(QFileInfo::exists(gamma));
        answerNextMessageBox(QStringLiteral("Yes"));
        QTest::keyClick(view, Qt::Key_Delete);
        QVERIFY(!QFileInfo::exists(gamma));
        QVERIFY(QFileInfo::exists(
            qEnvironmentVariable("XDG_DATA_HOME") + QStringLiteral("/Trash/files/Gamma.kkproj")));
        QVERIFY(m_window->model()->rowOf(gamma) < 0);
    }

    void theOpenProjectCannotBeDeleted()
    {
        const QString a = project(QStringLiteral("Current"));
        open();
        m_window->setOpenProjectPath(a);
        const QModelIndex i = m_window->model()->index(0, 0);
        QVERIFY(i.data(ProjectLibraryModel::IsOpenRole).toBool());
        QVERIFY(!(i.flags() & Qt::ItemIsEditable));
        answerNextMessageBox(QStringLiteral("OK")); // the refusal
        QVERIFY(!m_window->trash({a}, false));
        QVERIFY(QFileInfo::exists(a));
    }

    void foldersAndDragging()
    {
        open();
        const QString folder = m_window->createFolder(QStringLiteral("Client"));
        QVERIFY(QFileInfo(folder).isDir());
        const QString p = project(QStringLiteral("Ad"));
        m_window->model()->refresh();
        // Drop the project on the folder card (the model's drop handler, like a drag).
        const QModelIndex target = m_window->model()->index(m_window->model()->rowOf(folder), 0);
        QMimeData* data = m_window->model()->mimeData(
            {m_window->model()->index(m_window->model()->rowOf(p), 0)});
        QVERIFY(m_window->model()->canDropMimeData(data, Qt::MoveAction, -1, -1, target));
        QVERIFY(m_window->model()->dropMimeData(data, Qt::MoveAction, -1, -1, target));
        delete data;
        QVERIFY(QFileInfo::exists(folder + QStringLiteral("/Ad.kkproj")));
        QCOMPARE(m_window->model()->rowCount(), 1); // only the folder is left here
        // Double-click / Enter on a folder enters it; Backspace goes up.
        m_window->openPath(folder);
        QCOMPARE(m_window->model()->folder(), folder);
        QCOMPARE(shownNames(m_window->proxy()), QStringList {QStringLiteral("Ad")});
        m_window->cardView()->setFocus();
        QTest::keyClick(m_window->cardView(), Qt::Key_Backspace);
        QCOMPARE(m_window->model()->folder(), m_root);
        QCOMPARE(m_window->selectedPaths(), QStringList {folder});
        // A project file dropped from outside is imported (copied) into the shown folder.
        const QString outside = m_dir->path() + QStringLiteral("/Outside.kkproj");
        QVERIFY(test::writeProject(outside));
        QCOMPARE(m_window->model()->dropPaths({outside}, m_root), 1);
        QVERIFY(QFileInfo::exists(m_root + QStringLiteral("/Outside.kkproj")));
        QVERIFY(QFileInfo::exists(outside));
        // Recent Projects: projects anywhere, a missing one marked.
        m_window->setRecentProjects({outside, m_dir->path() + QStringLiteral("/gone.kkproj")});
        m_window->showRecent();
        QCOMPARE(m_window->model()->rowCount(), 2);
        QVERIFY(!m_window->model()->index(1, 0).data(ProjectLibraryModel::ExistsRole).toBool());
    }

    void newProjectKinds()
    {
        // Single file is the owner's default (nothing remembered yet); both are enabled.
        QSettings().remove(QStringLiteral("projectManager/newProjectKind"));
        {
            NewProjectDialog dialog(QStringLiteral("Untitled Project"));
            QVERIFY(dialog.singleFileButton()->isEnabled());
            QVERIFY(dialog.linkedButton()->isEnabled());
            QVERIFY(dialog.singleFileButton()->isChecked());
            QCOMPARE(dialog.kindId(), QStringLiteral("kinoklip"));
            QCOMPARE(NewProjectDialog::preferredKindId(), QStringLiteral("kinoklip"));
            // The choice is remembered when the dialog is accepted.
            dialog.linkedButton()->setChecked(true);
            dialog.accept();
            QCOMPARE(NewProjectDialog::preferredKindId(), QStringLiteral("kkproj"));
        }
        {
            NewProjectDialog dialog(QStringLiteral("Untitled Project"));
            QVERIFY(dialog.linkedButton()->isChecked());
        }
        NewProjectDialog::rememberKind(QStringLiteral("kinoklip"));
        QCOMPARE(NewProjectDialog::preferredKindId(), QStringLiteral("kinoklip"));

        open();
        QSignalSpy requested(m_window.get(), &ProjectManagerWindow::openRequested);
        const QString single = m_window->createProject(QStringLiteral("Packed"));
        QCOMPARE(single, m_root + QStringLiteral("/Packed.kinoklip"));
        QCOMPARE(projectkinds::readKinoklipMetadata(single).name, QStringLiteral("Packed"));
        NewProjectDialog::rememberKind(QStringLiteral("kkproj"));
        const QString p = m_window->createProject(QStringLiteral("Fresh"));
        QCOMPARE(p, m_root + QStringLiteral("/Fresh.kkproj"));
        QCOMPARE(requested.size(), 2);
        // A cancelled "save changes?" creates nothing.
        m_window->setSwitchGuard([](QWidget*) { return false; });
        QVERIFY(m_window->createProject(QStringLiteral("Never")).isEmpty());
        QVERIFY(!QFileInfo::exists(m_root + QStringLiteral("/Never.kkproj")));
    }

    void fileMenuAndSwitchingProjects()
    {
        prefs::setProjectLibraryDirectory(m_root);
        const QString a = project(QStringLiteral("First"));
        const QString b = project(QStringLiteral("Second"));
        App app;
        QVERIFY(app.actions.action(QStringLiteral("file.project_manager"))
                    ->shortcuts()
                    .contains(QKeySequence(QStringLiteral("Shift+1"))));
        QVERIFY(!app.actions.action(QStringLiteral("file.open"))
                     ->shortcuts()
                     .contains(QKeySequence(QStringLiteral("Shift+1"))));
        app.window->show();
        app.actions.action(QStringLiteral("file.project_manager"))->trigger();
        ProjectManagerWindow* pm = app.window->projectController()->projectManager();
        QVERIFY(pm && pm->isVisible());
        QCOMPARE(pm->library()->root(), m_root);
        QCOMPARE(pm->windowModality(), Qt::WindowModal);
        // Open the first project.
        pm->openPath(a);
        QCOMPARE(app.project.filePath(), a);
        QVERIFY(!pm->isVisible());
        QCOMPARE(ProjectController::recentProjects().first(), a);
        // Modify it, open the manager again: it is marked open; switching asks to save.
        app.project.setName(QStringLiteral("changed"));
        QVERIFY(app.project.isDirty());
        app.window->projectController()->showProjectManager();
        QVERIFY(pm->isVisible());
        QCOMPARE(pm->library()->openProjectPath(), a);
        answerNextMessageBox(QStringLiteral("Cancel"));
        pm->openPath(b);
        QCOMPARE(app.project.filePath(), a); // stayed
        QVERIFY(pm->isVisible());
        answerNextMessageBox(QStringLiteral("Discard"));
        pm->openPath(b);
        QCOMPARE(app.project.filePath(), b);
        QVERIFY(!pm->isVisible());
        // Opening the open project just closes the manager.
        app.window->projectController()->showProjectManager();
        pm->openPath(b);
        QVERIFY(!pm->isVisible());
        // Start-up "reopen the last project".
        app.project.newProject();
        QVERIFY(app.window->projectController()->openLastProject());
        QCOMPARE(app.project.filePath(), b);
        prefs::setProjectLibraryDirectory({});
    }

    /// Spec 38.20: the window over 300 projects. The second opening answers from the caches.
    void threeHundredProjectsInTheWindow()
    {
        const QString first = project(QStringLiteral("Project 000"));
        QFile src(first);
        QVERIFY(src.open(QIODevice::ReadOnly));
        const QByteArray bytes = src.readAll();
        for (int i = 1; i < 300; ++i) {
            QFile f(m_root + QStringLiteral("/Project %1.kkproj").arg(i, 3, 10, QLatin1Char('0')));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(bytes);
        }
        QElapsedTimer t;
        t.start();
        open();
        m_window->grab();
        const qint64 coldMs = t.elapsed();
        QCOMPARE(m_window->model()->rowCount(), 300);
        // Painting asks only for the visible cards' metadata, not all 300.
        QVERIFY(m_library->metadataReads() + 2 < 300);
        m_library->waitForIdle();
        // Load everything once (as scrolling would), then reopen from the cache.
        for (int r = 0; r < 300; ++r)
            m_library->metadata(m_window->model()->entry(r));
        m_library->waitForIdle();
        m_window.reset();
        m_library.reset();
        t.restart();
        open();
        m_window->sortBy(
            ProjectLibraryModel::DurationColumn, Qt::AscendingOrder); // needs all metadata
        m_window->grab();
        const qint64 warmMs = t.elapsed();
        QCOMPARE(m_library->metadataReads(), 0);
        qInfo("Project Manager over 300 projects: first show %lld ms, cached show + sort %lld ms",
            coldMs, warmMs);
        QVERIFY2(warmMs < 1000, "a cached library must open instantly");
    }
};

int main(int argc, char** argv)
{
    QTemporaryDir xdg(QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath()
        + QStringLiteral("/projectmanager-XXXXXX"));
    g_xdgRoot = xdg.path();
    test::isolateXdg(g_xdgRoot);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-test"));
    QCoreApplication::setApplicationName(QStringLiteral("projectmanager"));
    kk::initResources();
    TestProjectManager test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_projectmanager.moc"
