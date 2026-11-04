// SPDX-License-Identifier: GPL-3.0-or-later
// Fourth bug-hunting round, in the real main window where it matters: one test per fix, named
// after the spec row it covers.
#include "app/AutosaveController.h"
#include "app/MainWindow.h"
#include "app/ProjectController.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "app/pages/MediaPage.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "export/RenderQueue.h"
#include "platform/FileDialogs.h"
#include "projects/ProjectLibrary.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/deliver/DeliverPage.h"
#include "ui/deliver/RenderQueuePanel.h"
#include "ui/deliver/RenderSettingsPanel.h"
#include "ui/dialogs/KeyboardCustomizationDialog.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineDialogs.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>

#include <memory>

using namespace kk;
namespace ops = kk::editops;

namespace {

constexpr int64_t kSecondUs = 1'000'000;

/// Everything main() creates, for one test, with two fake 4 s clips with video and audio.
struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    QStringList ids;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        for (const char* name : {"alpha", "bravo"}) {
            MediaItem m;
            m.name = QLatin1String(name);
            m.path = QStringLiteral("/nonexistent/%1.mp4").arg(m.name);
            m.info.valid = true;
            m.info.hasVideo = true;
            m.info.hasAudio = true;
            m.info.durationUs = 4 * kSecondUs;
            m.info.frameRate = Rational {25, 1};
            m.info.width = 1920;
            m.info.height = 1080;
            ids.append(project.mediaPool()->addItem(m));
        }
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
        window->resize(1600, 1000);
        window->show();
        window->activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }

    TimelineController* controller() const { return window->editPage()->timeline()->controller(); }
};

/// A solid clip on V1 (one undo step).
void addSolid(Project& project, int64_t start = 0)
{
    project.beginEdit(QStringLiteral("add"));
    ClipData clip;
    clip.kind = ClipKind::Solid;
    clip.solidColor = Qt::red;
    clip.start = start;
    clip.duration = 25;
    ops::overwriteClip(project.timeline()->mutableData(), TrackRef {}, clip);
    project.endEdit();
}

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

/// Writes @p file with one clip, then leaves a crash autosave with a second clip.
QString crashAfterUnsavedChange(const QString& file)
{
    Project project;
    AutosaveController autosave(&project);
    addSolid(project);
    if (!project.save(file))
        return {};
    addSolid(project, 100);
    QTest::qWait(20); // the autosave is strictly newer than the project file
    if (!autosave.autosaveNow())
        return {};
    return autosave.autosavePath(); // "crash": no discard()
}

} // namespace

class TestBughunt4 : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_bughunt4"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }
    void init()
    {
        QSettings().clear();
        QDir(AutosaveController::autosaveDirectory()).removeRecursively();
    }

    // Spec 25.14: a new project's timeline and File > New Timeline start with V1 and A1 only
    // (Resolve); tracks are added when needed, and a project saved with more keeps them.
    void spec25_14_newTimelinesStartWithOneVideoAndOneAudioTrack()
    {
        Project project;
        const TimelineData& first = project.timeline()->data();
        QCOMPARE(first.videoTracks.size(), size_t(1));
        QCOMPARE(first.audioTracks.size(), size_t(1));
        const quint64 uid = project.createTimeline(QStringLiteral("Second"));
        QVERIFY(project.setActiveTimeline(uid));
        QCOMPARE(project.timeline()->data().videoTracks.size(), size_t(1));
        QCOMPARE(project.timeline()->data().audioTracks.size(), size_t(1));

        // A project with more tracks keeps them when it is saved and opened again.
        project.beginEdit(QStringLiteral("tracks"));
        ops::addTrack(project.timeline()->mutableData(), TrackType::Video);
        ops::addTrack(project.timeline()->mutableData(), TrackType::Audio);
        ops::addTrack(project.timeline()->mutableData(), TrackType::Audio);
        project.endEdit();
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("tracks.kkproj"));
        QVERIFY(project.save(file));
        Project reopened;
        QVERIFY(reopened.load(file));
        QVERIFY(reopened.setActiveTimeline(uid));
        QCOMPARE(reopened.timeline()->data().videoTracks.size(), size_t(2));
        QCOMPARE(reopened.timeline()->data().audioTracks.size(), size_t(3));
    }

    // Spec 25.15: Timeline Settings of a timeline with clips at its own frame rate does not
    // offer "Use project settings" when the project runs at another rate (ticking it was
    // silently ignored, spec 25.13).
    void spec25_15_useProjectSettingsLockedWithClipsAtAnotherRate()
    {
        Project project;
        const quint64 uid = project.activeTimelineUid();
        QVERIFY(project.setTimelineFormat(uid, false, 1920, 1080, Rational {30, 1}));
        addSolid(project);
        bool checked = false;
        bool enabled = true;
        QString tip;
        QTimer::singleShot(100, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            if (auto* follow
                = dialog->findChild<QCheckBox*>(QStringLiteral("UseProjectSettings"))) {
                checked = follow->isChecked();
                enabled = follow->isEnabled();
                tip = follow->toolTip();
            }
            dialog->reject();
        });
        timelinedialogs::editTimelineSettings(nullptr, project, uid);
        QVERIFY(!checked);
        QVERIFY(!enabled);
        QVERIFY(!tip.isEmpty());

        // Without clips it can still be ticked.
        Project empty;
        const quint64 uid2 = empty.activeTimelineUid();
        QVERIFY(empty.setTimelineFormat(uid2, false, 1920, 1080, Rational {30, 1}));
        enabled = false;
        QTimer::singleShot(100, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            if (auto* follow = dialog->findChild<QCheckBox*>(QStringLiteral("UseProjectSettings")))
                enabled = follow->isEnabled();
            dialog->reject();
        });
        timelinedialogs::editTimelineSettings(nullptr, empty, uid2);
        QVERIFY(enabled);
    }

    // Spec 25.16: File > New Timeline opens Resolve's Create New Timeline dialog (Start
    // Timecode, Timeline Name, No. of Video Tracks, No. of Audio Tracks), prefilled with
    // 01:00:00:00, "Timeline 2", 1 and 1; the timeline gets what was chosen in one undo step.
    void spec25_16_createNewTimelineDialog()
    {
        Fixture f;
        QString start;
        QString name;
        int video = 0;
        int audio = 0;
        QTimer::singleShot(100, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != QLatin1String("NewTimelineDialog")) {
                if (dialog)
                    dialog->reject();
                return;
            }
            auto* startEdit = dialog->findChild<QLineEdit*>(QStringLiteral("StartTimecode"));
            auto* nameEdit = dialog->findChild<QLineEdit*>(QStringLiteral("Name"));
            auto* videoSpin = dialog->findChild<QSpinBox*>(QStringLiteral("VideoTracks"));
            auto* audioSpin = dialog->findChild<QSpinBox*>(QStringLiteral("AudioTracks"));
            if (!startEdit || !nameEdit || !videoSpin || !audioSpin) {
                dialog->reject();
                return;
            }
            start = startEdit->text();
            name = nameEdit->text();
            video = videoSpin->value();
            audio = audioSpin->value();
            startEdit->setText(QStringLiteral("00:00:10:00"));
            nameEdit->setText(QStringLiteral("Interview"));
            videoSpin->setValue(3);
            audioSpin->setValue(2);
            dialog->accept();
        });
        const int before = f.project.undoStack()->count();
        f.actions.action(QStringLiteral("timeline.new"))->trigger();
        QCOMPARE(start, QStringLiteral("01:00:00:00"));
        QCOMPARE(name, QStringLiteral("Timeline 2"));
        QCOMPARE(video, 1);
        QCOMPARE(audio, 1);
        const TimelineData& t = f.project.timeline()->data();
        QCOMPARE(t.name, QStringLiteral("Interview"));
        QCOMPARE(t.videoTracks.size(), size_t(3));
        QCOMPARE(t.audioTracks.size(), size_t(2));
        QCOMPARE(t.startTimecode, int64_t(10 * 24));
        QCOMPARE(f.project.undoStack()->count(), before + 1);
    }

    // Spec 45.1: the pencil on a render job card loads the job's settings into Render
    // Settings, whose button reads "Update Job"; updating changes the job in place (no new
    // job) and Cancel leaves it as it was.
    void spec45_1_editRenderJob()
    {
        Fixture f;
        addSolid(f.project);
        f.window->setCurrentPage(QStringLiteral("deliver"));
        DeliverPage* deliver = f.window->deliverPage();
        QVERIFY(deliver);
        RenderSettingsPanel* settings = deliver->settingsPanel();
        RenderQueue* queue = deliver->renderQueue();
        QTemporaryDir dir;
        ExportSettings s = settings->settings();
        s.outputPath = dir.filePath(QStringLiteral("first.mp4"));
        s.video.width = 1280;
        s.video.height = 720;
        settings->setSettings(s);
        const quint64 id = deliver->addCurrentToQueue(false);
        QVERIFY(id != 0);
        QCOMPARE(queue->jobs().size(), 1);

        // Different settings in the panel, then the pencil.
        ExportSettings other = settings->settings();
        other.outputPath = dir.filePath(QStringLiteral("unrelated.mp4"));
        other.video.width = 3840;
        other.video.height = 2160;
        settings->setSettings(other);
        auto* add = settings->findChild<QPushButton*>(QStringLiteral("RenderAddToQueue"));
        QVERIFY(add);
        QCOMPARE(add->text(), QStringLiteral("Add to Render Queue"));
        auto* pencil = deliver->findChild<QToolButton*>(QStringLiteral("RenderJobEdit"));
        QVERIFY(pencil);
        QVERIFY(pencil->isEnabled());
        pencil->click();
        QCOMPARE(deliver->editedJob(), id);
        QCOMPARE(add->text(), QStringLiteral("Update Job"));
        QCOMPARE(settings->settings().video.width, 1280);
        QCOMPARE(
            QFileInfo(settings->settings().outputPath).fileName(), QStringLiteral("first.mp4"));

        // Change the job and update it (the button runs Update Job, not Add).
        ExportSettings changed = settings->settings();
        changed.video.width = 1920;
        changed.video.height = 1080;
        changed.outputPath = dir.filePath(QStringLiteral("second.mp4"));
        settings->setSettings(changed);
        add->click();
        QCOMPARE(queue->jobs().size(), 1);
        QCOMPARE(deliver->editedJob(), quint64(0));
        QCOMPARE(add->text(), QStringLiteral("Add to Render Queue"));
        const RenderQueue::Job* job = queue->job(id);
        QVERIFY(job);
        QCOMPARE(job->settings.video.width, 1920);
        QCOMPARE(QFileInfo(job->settings.outputPath).fileName(), QStringLiteral("second.mp4"));
        QVERIFY(job->name.endsWith(QStringLiteral("second.mp4")));
        QCOMPARE(job->status, RenderQueue::Status::Queued);

        // Cancel: the job stays, the settings shown before editing come back.
        ExportSettings before = settings->settings();
        before.video.width = 1280;
        before.video.height = 720;
        before.outputPath = dir.filePath(QStringLiteral("before.mp4"));
        settings->setSettings(before);
        deliver->editJob(id);
        QCOMPARE(settings->settings().video.width, 1920);
        auto* cancel = settings->findChild<QPushButton*>(QStringLiteral("RenderCancelEdit"));
        QVERIFY(cancel);
        QVERIFY(cancel->isVisibleTo(settings));
        cancel->click();
        QCOMPARE(deliver->editedJob(), quint64(0));
        QVERIFY(!cancel->isVisibleTo(settings));
        QCOMPARE(settings->settings().video.width, 1280);
        QCOMPARE(queue->job(id)->settings.video.width, 1920);

        // Removing the edited job leaves the mode.
        deliver->editJob(id);
        QVERIFY(queue->removeJob(id));
        QCOMPARE(deliver->editedJob(), quint64(0));
        QCOMPARE(add->text(), QStringLiteral("Add to Render Queue"));
    }

    // Spec 38.26: two never-saved projects with the same name (two windows or two running
    // copies) write separate autosave files, and both can be recovered after a crash.
    void spec38_26_untitledProjectsHaveTheirOwnAutosave()
    {
        QString first;
        QString second;
        {
            Project a;
            Project b;
            QCOMPARE(a.name(), b.name());
            AutosaveController autosaveA(&a);
            AutosaveController autosaveB(&b);
            addSolid(a);
            addSolid(b, 0);
            addSolid(b, 50);
            QVERIFY(autosaveA.autosaveNow());
            QVERIFY(autosaveB.autosaveNow());
            first = autosaveA.autosavePath();
            second = autosaveB.autosavePath();
            QVERIFY(first != second);
            QVERIFY(QFileInfo::exists(first));
            QVERIFY(QFileInfo::exists(second));
            // A new project in the same window is another project: another file.
            const QString before = autosaveA.autosavePath();
            a.newProject();
            QVERIFY(autosaveA.autosavePath() != before);
            QVERIFY(!QFileInfo::exists(first)); // replaced after the prompt: obsolete
            addSolid(a);
            QVERIFY(autosaveA.autosaveNow());
            first = autosaveA.autosavePath();
            // "crash": both controllers die without discard()
        }
        const QVector<RecoveryCandidate> found = AutosaveController::findRecoverable();
        QCOMPARE(found.size(), 2);
        QStringList paths {found[0].autosavePath, found[1].autosavePath};
        QVERIFY(paths.contains(QFileInfo(first).absoluteFilePath()));
        QVERIFY(paths.contains(QFileInfo(second).absoluteFilePath()));
        // The recovered copy holds that project's own clips.
        Project recovered;
        QVERIFY(recovered.load(second));
        QCOMPARE(recovered.timeline()->data().videoTracks[0].clips.size(), size_t(2));
    }

    // Spec 23.10: in Keyboard Customization, Enter in the search field goes to the first
    // matching command and Enter on a command starts recording; neither closes the dialog.
    void spec23_10_keyboardCustomizationEnterKeepsTheDialogOpen()
    {
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        KeyboardCustomizationDialog dialog(actions);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* search = dialog.findChild<QLineEdit*>(QStringLiteral("KeyboardSearch"));
        auto* tree = dialog.findChild<QTreeWidget*>(QStringLiteral("KeyboardCommands"));
        QVERIFY(search);
        QVERIFY(tree);
        search->setFocus();
        QTest::keyClicks(search, QStringLiteral("blade"));
        QVERIFY(!dialog.visibleIds().isEmpty());
        QTest::keyClick(search, Qt::Key_Return);
        QVERIFY(dialog.isVisible());
        QCOMPARE(dialog.result(), 0);
        QVERIFY(dialog.visibleIds().contains(dialog.currentId()));
        QVERIFY(tree->hasFocus());

        QTest::keyClick(tree, Qt::Key_Return);
        QVERIFY(dialog.isVisible());
        auto* recorder = dialog.findChild<ShortcutRecorder*>();
        QVERIFY(recorder);
        QVERIFY(recorder->isEnabled());
        QVERIFY(recorder->hasFocus());
        QTest::keyClick(recorder, Qt::Key_Escape); // cancels recording, not the dialog
        QVERIFY(dialog.isVisible());
        dialog.reject();
    }

    // Spec 38.27: opening a project (File > Open, Open Recent, the Project Manager, the
    // command line) whose crash autosave was put off with "Not Now" offers it again, instead
    // of opening the older file and autosaving over the copy.
    void spec38_27_openingAProjectOffersItsAutosave()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("trip.kkproj"));
        const QString autosaveFile = crashAfterUnsavedChange(file);
        QVERIFY(QFileInfo::exists(autosaveFile));
        Fixture f;
        ProjectController* projects = f.window->projectController();

        answerNextMessageBox(QStringLiteral("Not Now"));
        QVERIFY(projects->openFile(file, false));
        QCOMPARE(f.project.timeline()->data().videoTracks[0].clips.size(), size_t(1));
        QVERIFY(QFileInfo::exists(autosaveFile)); // still there for later

        f.project.newProject();
        answerNextMessageBox(QStringLiteral("Restore"));
        QVERIFY(projects->openFile(file, false));
        QCOMPARE(f.project.timeline()->data().videoTracks[0].clips.size(), size_t(2));
        QCOMPARE(f.project.filePath(), QFileInfo(file).absoluteFilePath());
        QVERIFY(f.project.isDirty());
        QCOMPARE(ProjectController::recentProjects().value(0), QFileInfo(file).absoluteFilePath());
    }

    // Spec 38.28: File > Open Recent shows a "&" in a project name as text (it was taken for
    // a mnemonic and vanished).
    void spec38_28_recentProjectNamesKeepAmpersands()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("Tom & Jerry.kkproj"));
        {
            Project project;
            QVERIFY(project.save(file));
        }
        ProjectController::addRecentProject(file);
        Fixture f;
        QMenu menu;
        f.window->projectController()->attachRecentMenu(&menu);
        QVERIFY(!menu.actions().isEmpty());
        QCOMPARE(menu.actions().first()->text(), QStringLiteral("&1 Tom && Jerry"));
    }

    // Spec 38.29: renaming or moving a project (or its folder) in the Project Manager keeps
    // its File > Open Recent entry (it showed "(missing)").
    void spec38_29_recentProjectsFollowRenameAndMove()
    {
        QTemporaryDir dir;
        prefs::setProjectLibraryDirectory(dir.path());
        const QString file = dir.filePath(QStringLiteral("Trip.kkproj"));
        {
            Project project;
            QVERIFY(project.save(file));
        }
        ProjectController::addRecentProject(file);
        Fixture f;
        ProjectController* projects = f.window->projectController();
        ProjectLibrary* library = projects->projectLibrary();
        QString error;
        const QString renamed = library->rename(file, QStringLiteral("Holiday"), &error);
        QVERIFY2(!renamed.isEmpty(), qPrintable(error));
        QCOMPARE(ProjectController::recentProjects().value(0), renamed);
        const QString folder = library->createFolder(dir.path(), QStringLiteral("2026"), &error);
        QVERIFY2(!folder.isEmpty(), qPrintable(error));
        const QString moved = library->move(renamed, folder, &error);
        QVERIFY2(!moved.isEmpty(), qPrintable(error));
        QCOMPARE(ProjectController::recentProjects().value(0), moved);
        const QString renamedFolder = library->rename(folder, QStringLiteral("Archive"), &error);
        QVERIFY2(!renamedFolder.isEmpty(), qPrintable(error));
        QCOMPARE(ProjectController::recentProjects().value(0),
            renamedFolder + QStringLiteral("/Holiday.kkproj"));
        QVERIFY(QFileInfo::exists(ProjectController::recentProjects().value(0)));
    }

    // Spec 38.30: Save As gives a new project the name it is saved under (the title kept
    // "Untitled Project"); a name that was the file's follows the new file; a name set in
    // Project Settings stays.
    void spec38_30_saveAsNamesTheProject()
    {
        QTemporaryDir dir;
        Fixture f;
        ProjectController* projects = f.window->projectController();
        const QString trip = dir.filePath(QStringLiteral("Trip.kkproj"));
        platform::setFileDialogHandlerForTests(
            [trip](const platform::FileDialogRequest&) { return QStringList {trip}; });
        QVERIFY(projects->saveAs());
        QCOMPARE(f.project.name(), QStringLiteral("Trip"));
        QVERIFY(f.window->windowTitle().startsWith(QStringLiteral("Trip")));
        QVERIFY(!f.project.isDirty());

        const QString copy = dir.filePath(QStringLiteral("Trip v2.kkproj"));
        platform::setFileDialogHandlerForTests(
            [copy](const platform::FileDialogRequest&) { return QStringList {copy}; });
        QVERIFY(projects->saveAs());
        QCOMPARE(f.project.name(), QStringLiteral("Trip v2"));

        f.project.setName(QStringLiteral("My Film"));
        const QString third = dir.filePath(QStringLiteral("export.kkproj"));
        platform::setFileDialogHandlerForTests(
            [third](const platform::FileDialogRequest&) { return QStringList {third}; });
        QVERIFY(projects->saveAs());
        QCOMPARE(f.project.name(), QStringLiteral("My Film"));
        platform::setFileDialogHandlerForTests({});
    }

    // Spec 33.33: on the Media page, double-clicking a clip (Open in Source Viewer) shows it
    // in the viewer and gives the viewer the keyboard, as on the Edit page; it did nothing.
    void spec33_33_mediaPageDoubleClickOpensTheClipInTheViewer()
    {
        Fixture f;
        QVERIFY(f.window->setCurrentPage(QStringLiteral("media")));
        MediaPage* page = f.window->mediaPage();
        QVERIFY(page);
        QCoreApplication::processEvents();
        page->mediaPool()->setFocus();
        emit page->mediaPool()->openInSourceViewer(f.ids[1]);
        QCOMPARE(page->viewer()->sourceMedia(), f.ids[1]);
        QWidget* focus = QApplication::focusWidget();
        QVERIFY(focus);
        QVERIFY(focus == page->viewer() || page->viewer()->isAncestorOf(focus));
    }

    // Spec 33.34: Replace Selected Clip… points an offline clip at any file (a renamed one
    // that Relink cannot find by name): probed again, one undo step, timeline clips follow.
    void spec33_34_replaceSelectedClip()
    {
        const QString source = QStringLiteral(KK_TEST_DATA_DIR "/generated/h264_25fps.mp4");
        if (!QFileInfo::exists(source))
            QSKIP("generated test media missing");
        Fixture f;
        QTemporaryDir dir;
        const QString renamed = dir.filePath(QStringLiteral("renamed take.mp4"));
        QVERIFY(QFile::copy(source, renamed));
        f.controller()->insertSource(f.ids[0], 0, 2 * kSecondUs, SourceEditMode::Overwrite);
        QVERIFY(f.project.mediaPool()->isOffline(f.ids[0]));
        MediaPoolPanel* pool = f.window->editPage()->mediaPool();

        const int before = f.project.undoStack()->count();
        QString error;
        QVERIFY(!pool->replaceClip(f.ids[0], dir.filePath(QStringLiteral("none.mp4")), &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(f.project.undoStack()->count(), before);

        QVERIFY2(pool->replaceClip(f.ids[0], renamed, &error), qPrintable(error));
        QCOMPARE(f.project.undoStack()->count(), before + 1);
        const MediaItem* item = f.project.mediaPool()->item(f.ids[0]);
        QCOMPARE(item->path, QFileInfo(renamed).absoluteFilePath());
        QCOMPARE(item->name, QStringLiteral("alpha")); // a name the user gave stays
        QVERIFY(item->info.valid);
        QVERIFY(!f.project.mediaPool()->isOffline(f.ids[0]));
        f.project.undoStack()->undo();
        QVERIFY(f.project.mediaPool()->isOffline(f.ids[0]));
    }
};

QTEST_MAIN(TestBughunt4)
#include "test_bughunt4.moc"
