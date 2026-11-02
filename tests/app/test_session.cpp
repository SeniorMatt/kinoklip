// SPDX-License-Identifier: GPL-3.0-or-later
// Session safety: autosave life cycle, crash recovery, unsaved-changes prompts on New / Open /
// Quit, File > Open Recent.
#include "app/AutosaveController.h"
#include "app/MainWindow.h"
#include "app/ProjectController.h"
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <memory>

using namespace kk;

namespace {

struct App
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    std::unique_ptr<PlaybackEngine> playback;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    explicit App(bool autosave = true)
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
        wo.autosave = autosave;
        wo.restoreLayout = false;
        wo.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, wo);
    }
    AutosaveController* autosave() const { return window->findChild<AutosaveController*>(); }
};

void addClip(Project& project, int64_t start = 0)
{
    project.beginEdit(QStringLiteral("add"));
    ClipData clip;
    clip.kind = ClipKind::Solid;
    clip.solidColor = Qt::red;
    clip.start = start;
    clip.duration = 25;
    editops::overwriteClip(project.timeline()->mutableData(), TrackRef {}, clip);
    project.endEdit();
}

size_t clipCount(const Project& project)
{
    return project.timeline()->data().videoTracks[0].clips.size();
}

/// Clicks the button with text containing @p text in the next modal message box.
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

void clearAutosaves()
{
    QDir(AutosaveController::autosaveDirectory()).removeRecursively();
}

} // namespace

class TestSession : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_session"));
        QSettings().clear();
        initResources();
    }
    void init() { clearAutosaves(); }

    void autosaveRemovedOnSaveAndClose()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("a.kkproj"));
        App app;
        AutosaveController* autosave = app.autosave();
        QVERIFY(autosave);
        addClip(app.project);
        QVERIFY(autosave->autosaveNow());
        const QString unsavedCopy = autosave->autosavePath();
        QVERIFY(QFileInfo::exists(unsavedCopy));
        QVERIFY(app.project.save(file));
        QVERIFY(!QFileInfo::exists(unsavedCopy)); // saved: obsolete

        addClip(app.project, 50);
        QVERIFY(autosave->autosaveNow());
        QVERIFY(QFileInfo::exists(autosave->autosavePath()));
        answerNextMessageBox(QStringLiteral("Discard"));
        QVERIFY(app.window->close());
        QVERIFY(!QFileInfo::exists(autosave->autosavePath())); // normal shutdown
        QVERIFY(AutosaveController::findRecoverable().isEmpty());
    }

    void crashLeavesRecoverableAutosave()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("trip.kkproj"));
        QString autosaveFile;
        {
            Project project;
            AutosaveController autosave(&project);
            addClip(project);
            QVERIFY(project.save(file));
            addClip(project, 100); // unsaved change
            QTest::qWait(20); // autosave strictly newer than the project file
            QVERIFY(autosave.autosaveNow());
            autosaveFile = autosave.autosavePath();
            // "crash": controller and project die without discard()
        }
        QVERIFY(QFileInfo::exists(autosaveFile));

        const QVector<RecoveryCandidate> found = AutosaveController::findRecoverable();
        QCOMPARE(found.size(), 1);
        QCOMPARE(found[0].documentPath, QFileInfo(file).absoluteFilePath());
        QCOMPARE(found[0].name, QStringLiteral("Untitled Project"));

        // Restore through the prompt in a fresh window.
        App app;
        answerNextMessageBox(QStringLiteral("Restore"));
        QVERIFY(app.window->projectController()->offerRecovery());
        QCOMPARE(clipCount(app.project), size_t(2));
        QCOMPARE(app.project.filePath(), QFileInfo(file).absoluteFilePath());
        QVERIFY(app.project.isDirty()); // the user decides whether to save over the file
        // Saving makes the adopted autosave obsolete.
        QVERIFY(app.project.save(app.project.filePath()));
        QVERIFY(!QFileInfo::exists(autosaveFile));
        QVERIFY(AutosaveController::findRecoverable().isEmpty());
    }

    void obsoleteAndForeignAutosavesAreNotOffered()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("p.kkproj"));
        Project project;
        addClip(project);
        QVERIFY(project.save(file));
        QDir().mkpath(AutosaveController::autosaveDirectory());
        auto write = [&](const QString& name, const QJsonObject& info) {
            const QString path = QDir(AutosaveController::autosaveDirectory()).filePath(name);
            if (!project.saveCopy(path, nullptr, info))
                qFatal("cannot write %s", qPrintable(path));
            return path;
        };
        // Older than the project file (project saved after it): deleted while scanning.
        const QString old = write(QStringLiteral("old.kkproj"),
            {{QStringLiteral("documentPath"), file}, {QStringLiteral("pid"), 0}});
        QFile f(old);
        QVERIFY(f.open(QIODevice::ReadWrite));
        QVERIFY(f.setFileTime(
            QDateTime::currentDateTime().addSecs(-3600), QFileDevice::FileModificationTime));
        f.close();
        // Written by a process that is still running (pid 1): left alone.
        const QString foreign = write(QStringLiteral("foreign.kkproj"),
            {{QStringLiteral("documentPath"), QString()}, {QStringLiteral("pid"), 1}});
        // 0.1.0-alpha.1 autosave without the info object: ignored.
        const QString legacy = write(QStringLiteral("legacy.kkproj"), {});
        QVERIFY(AutosaveController::findRecoverable().isEmpty());
        QVERIFY(!QFileInfo::exists(old));
        QVERIFY(QFileInfo::exists(foreign));
        QVERIFY(QFileInfo::exists(legacy));

        // Discard deletes the file; Not Now keeps it.
        const QString unsaved = write(QStringLiteral("unsaved.kkproj"),
            {{QStringLiteral("documentPath"), QString()}, {QStringLiteral("pid"), 0}});
        App app;
        answerNextMessageBox(QStringLiteral("Not Now"));
        QVERIFY(!app.window->projectController()->offerRecovery());
        QVERIFY(QFileInfo::exists(unsaved));
        answerNextMessageBox(QStringLiteral("Discard"));
        QVERIFY(!app.window->projectController()->offerRecovery());
        QVERIFY(!QFileInfo::exists(unsaved));
        // Only the command-line project's autosave is offered when one is given.
        write(QStringLiteral("other.kkproj"),
            {{QStringLiteral("documentPath"), QString()}, {QStringLiteral("pid"), 0}});
        QVERIFY(!app.window->projectController()->offerRecovery(file)); // no prompt
    }

    void unsavedChangesPrompts()
    {
        App app(false);
        addClip(app.project);
        QVERIFY(app.project.isDirty());
        // New: Cancel keeps everything.
        answerNextMessageBox(QStringLiteral("Cancel"));
        app.actions.action(QStringLiteral("file.new"))->trigger();
        QCOMPARE(clipCount(app.project), size_t(1));
        // Quit: Cancel keeps the window open.
        answerNextMessageBox(QStringLiteral("Cancel"));
        QVERIFY(!app.window->close());
        QCOMPARE(clipCount(app.project), size_t(1));
        // New: Discard resets.
        answerNextMessageBox(QStringLiteral("Discard"));
        app.actions.action(QStringLiteral("file.new"))->trigger();
        QCOMPARE(clipCount(app.project), size_t(0));
        QVERIFY(!app.project.isDirty());
        // Open (recent) with changes: Cancel keeps the current project.
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("x.kkproj"));
        QVERIFY(app.project.save(file));
        addClip(app.project);
        answerNextMessageBox(QStringLiteral("Cancel"));
        QVERIFY(!app.window->projectController()->openFile(file, true));
        QCOMPARE(clipCount(app.project), size_t(1));
    }

    void openRecentMenu()
    {
        ProjectController::clearRecentProjects();
        QTemporaryDir dir;
        App app(false);
        QStringList files;
        for (int i = 0; i < ProjectController::kMaxRecentProjects + 2; ++i) {
            files.append(dir.filePath(QStringLiteral("p%1.kkproj").arg(i)));
            QVERIFY(app.project.save(files.last()));
            ProjectController::addRecentProject(files.last());
        }
        QStringList recent = ProjectController::recentProjects();
        QCOMPARE(recent.size(), ProjectController::kMaxRecentProjects);
        QCOMPARE(recent.first(), QFileInfo(files.last()).absoluteFilePath());
        ProjectController::addRecentProject(files[5]); // re-opened: moves to the top, no copy
        recent = ProjectController::recentProjects();
        QCOMPARE(recent.first(), QFileInfo(files[5]).absoluteFilePath());
        QCOMPARE(recent.count(QFileInfo(files[5]).absoluteFilePath()), 1);

        auto* menu = app.window->menuBar()->findChild<QMenu*>(QStringLiteral("menu:file.recent"));
        QVERIFY(menu);
        QFile::remove(files.last()); // a missing project is shown disabled
        emit menu->aboutToShow();
        QList<QAction*> entries = menu->actions();
        QCOMPARE(entries.size(), ProjectController::kMaxRecentProjects + 2); // + separator, clear
        QVERIFY(entries[0]->text().contains(QStringLiteral("p5")));
        QVERIFY(entries[1]->text().contains(QStringLiteral("missing")));
        QVERIFY(!entries[1]->isEnabled());
        // Opening from the menu loads the project.
        addClip(app.project);
        QVERIFY(app.project.save(files[5]));
        app.project.newProject();
        emit menu->aboutToShow();
        menu->actions()[0]->trigger();
        QCOMPARE(app.project.filePath(), QFileInfo(files[5]).absoluteFilePath());
        QCOMPARE(clipCount(app.project), size_t(1));
        // Clear Menu.
        emit menu->aboutToShow();
        menu->actions().last()->trigger();
        QVERIFY(ProjectController::recentProjects().isEmpty());
        emit menu->aboutToShow();
        QCOMPARE(menu->actions().size(), 1);
        QVERIFY(!menu->actions()[0]->isEnabled());
    }
};

QTEST_MAIN(TestSession)
#include "test_session.moc"
