// SPDX-License-Identifier: GPL-3.0-or-later
// Single-file projects in the real main window (spec section 39, offscreen): the owner's
// "drag a file in, save, delete the original, the clip is still there and editable" end to
// end (decode, play, render), import with Cancel and the duplicate notice, File > Pack /
// Unpack / Compact / Verify through their actions (with the cancel paths), crash recovery of
// a single-file project from its autosave, read-only opening, and the Media Pool's location
// indicator.
#include "../export/ExportTestUtil.h"
#include "../projects/ProjectTestUtil.h"
#include "app/AutosaveController.h"
#include "app/MainWindow.h"
#include "app/ProjectController.h"
#include "app/pages/EditPage.h"
#include "core/MediaPool.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "export/ExportJob.h"
#include "media/MediaInfo.h"
#include "media/MediaSource.h"
#include "pack/EmbeddedMedia.h"
#include "pack/ProjectPackage.h"
#include "platform/FileDialogs.h"
#include "playback/PlaybackEngine.h"
#include "projects/ProjectKinds.h"
#include "projects/ProjectLock.h"
#include "render/Compositor.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/MediaImport.h"
#include "ui/common/ProgressTask.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/mediapool/MediaPoolModel.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/projects/ProjectManagerWindow.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>
#include <memory>

using namespace kk;

namespace {

QString testMedia(const char* name)
{
    return QStringLiteral(KK_TEST_DATA_DIR "/generated/") + QLatin1String(name);
}

/// Everything main() creates (program engine without a sound device).
struct App
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    std::unique_ptr<PlaybackEngine> playback;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    explicit App(bool autosave = false)
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
    ProjectController* controller() const { return window->projectController(); }
    QAction* action(const char* id) { return actions.action(QLatin1String(id)); }
    void trigger(const char* id) { action(id)->trigger(); }
    AutosaveController* autosave() const { return window->findChild<AutosaveController*>(); }
    MediaPoolModel* poolModel() const { return window->editPage()->mediaPool()->model(); }
    /// Appends media pool item @p id to the end of the timeline (the Edit page's command).
    void append(const QString& id)
    {
        window->editPage()->timeline()->controller()->editSources(
            {SourceSelection {id, 0, 0}}, SourceEditMode::AppendAtEnd);
    }
    size_t clipCount() const
    {
        size_t n = 0;
        for (const auto& t : project.timeline()->data().videoTracks)
            n += t.clips.size();
        return n;
    }
};

/// Clicks the button whose text contains @p text in the next modal message box (notices,
/// which are not modal, are skipped).
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
        qWarning("no button %s in “%s”", qPrintable(text), qPrintable(box->text()));
        box->reject();
    });
}

/// Calls @p fn with the next modal message box (after progress dialogs are gone).
void onNextMessageBox(std::function<void(QMessageBox*)> fn)
{
    QTimer::singleShot(10, qApp, [fn] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) {
            onNextMessageBox(fn);
            return;
        }
        fn(box);
    });
}

/// The non-blocking notices currently shown (ProjectController::notify()).
QList<QMessageBox*> notices()
{
    QList<QMessageBox*> out;
    for (QWidget* w : QApplication::topLevelWidgets()) {
        auto* box = qobject_cast<QMessageBox*>(w);
        if (box && box->objectName() == QLatin1String("ProjectNotice") && box->isVisible())
            out.append(box);
    }
    return out;
}

void closeNotices()
{
    for (QMessageBox* box : notices())
        box->close();
    QCoreApplication::processEvents();
}

/// Answers every file dialog with @p paths (empty = cancelled) until reset.
void answerFileDialogs(const QStringList& paths)
{
    platform::setFileDialogHandlerForTests(
        [paths](const platform::FileDialogRequest&) { return paths; });
}

/// Presses Cancel in every progress dialog, before the work starts.
void cancelProgress(bool on)
{
    if (on)
        setProgressDialogHookForTests([](QProgressDialog* d) { d->cancel(); });
    else
        setProgressDialogHookForTests({});
}

/// The first file item of the media pool (timelines are items too).
MediaItem firstFile(const Project& project)
{
    for (const MediaItem& m : project.mediaPool()->state().items) {
        if (!m.isTimeline())
            return m;
    }
    return {};
}

/// Raw bytes of @p path.
QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class TestSingleFile : public QObject
{
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_media;

    QString path(const QString& name) const { return m_dir->filePath(name); }

    /// A copy of the test clip at @p name (so the test can delete the "original").
    QString copyMedia(const QString& name, const char* source = "h264_25fps.mp4")
    {
        const QString to = path(name);
        QDir().mkpath(QFileInfo(to).absolutePath());
        QFile::remove(to);
        if (!QFile::copy(testMedia(source), to))
            qFatal("cannot copy %s", source);
        return to;
    }

    /// A new single-file project opened in @p app.
    QString newSingleFile(App& app, const QString& name)
    {
        const QString file = path(name + QStringLiteral(".kinoklip"));
        QString error;
        if (!projectkinds::createKinoklip(file, name, &error))
            qFatal("createKinoklip: %s", qPrintable(error));
        if (!app.controller()->openFile(file, false))
            qFatal("cannot open %s", qPrintable(file));
        return file;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_singlefile"));
        QSettings().clear();
        initResources();
        m_media = testMedia("h264_25fps.mp4");
    }

    void init()
    {
        if (!QFileInfo::exists(m_media))
            QSKIP("test media missing (tests/data/generated)");
        // A folder with a distinctive name: the privacy check looks for it in the file.
        m_dir = std::make_unique<QTemporaryDir>(
            QDir::tempPath() + QStringLiteral("/kk-singlefile-originals-XXXXXX"));
        QVERIFY(m_dir->isValid());
        QDir(AutosaveController::autosaveDirectory()).removeRecursively();
    }

    void cleanup()
    {
        platform::setFileDialogHandlerForTests({});
        setProgressDialogHookForTests({});
        closeNotices();
        m_dir.reset();
    }

    void deleteTheOriginalAndTheClipStaysEditable()
    {
        // Owner: "if I drag a file into the project, save it, and then delete the original
        // file, the clip is still there and editable".
        prefs::setProjectLibraryDirectory(path(QStringLiteral("library")));
        QSettings().remove(QStringLiteral("projectManager/newProjectKind")); // the default
        const QString original = copyMedia(QStringLiteral("originals/beach.mp4"));
        QString file;
        {
            App app;
            // New Project in the Project Manager: Single file is the default kind.
            app.controller()->showProjectManager();
            ProjectManagerWindow* manager = app.controller()->projectManager();
            QVERIFY(manager);
            file = manager->createProject(QStringLiteral("Film"));
            QCOMPARE(file, path(QStringLiteral("library/Film.kinoklip")));
            QCOMPARE(app.project.filePath(), file);
            QVERIFY(app.controller()->isSingleFile());
            QVERIFY(app.controller()->package());
            QVERIFY(!app.controller()->isReadOnly());
            QVERIFY(app.action("file.compact_project")->isEnabled());
            QVERIFY(!app.action("file.pack_project")->isEnabled());
            QVERIFY(app.window->windowTitle().startsWith(QStringLiteral("Film[*]")));

            // Drop from a file manager onto the timeline = import + edit; both go through
            // importMediaWithFeedback(), which copies the file in first.
            const QStringList ids = importMediaWithFeedback(app.ctx, {original}, app.window.get());
            QCOMPARE(ids.size(), 1);
            const MediaItem* item = app.project.mediaPool()->item(ids.front());
            QVERIFY(item);
            QVERIFY2(item->path.startsWith(file + QLatin1Char('/')), qPrintable(item->path));
            QCOMPARE(QFileInfo(item->path).fileName(), QStringLiteral("beach.mp4"));
            app.append(ids.front());
            QCOMPARE(app.clipCount(), size_t(1));
            app.trigger("file.save");
            QVERIFY(!app.project.isDirty());
            QCOMPARE(int(app.controller()->package()->media().size()), 1);
        }
        QVERIFY(QFile::remove(original));
        QVERIFY(!QFileInfo::exists(original));
        // Privacy: the file does not tell where the media came from.
        const QByteArray raw = readAll(file);
        QVERIFY(!raw.contains(QFileInfo(original).absolutePath().toUtf8()));
        QVERIFY(!raw.contains(QByteArrayLiteral("kk-singlefile-originals")));

        App app;
        QVERIFY(app.controller()->openFile(file, false));
        QCOMPARE(app.clipCount(), size_t(1));
        const MediaItem item = firstFile(app.project);
        QVERIFY(!app.project.mediaPool()->isOffline(item.id));
        QVERIFY(media::mediaExists(item.path));
        const QModelIndex row = app.poolModel()->index(app.poolModel()->rowOf(item.id), 0);
        QVERIFY(!row.data(MediaPoolModel::OfflineRole).toBool());
        QCOMPARE(
            row.data(MediaPoolModel::LocationRole).toInt(), int(MediaPoolModel::StoredLocation));

        // Decodes ...
        const QImage frame = test::decodeAt(item.path, 500'000);
        QVERIFY(!frame.isNull());
        QVERIFY(test::meanLuma(frame) > 5);
        // ... plays ...
        app.project.timeline()->setPlayhead(0);
        QSignalSpy frames(app.playback.get(), &PlaybackEngine::frameReady);
        app.trigger("transport.play_pause");
        QTRY_VERIFY_WITH_TIMEOUT(
            app.project.timeline()->playhead() >= 10 && frames.count() >= 3, 15000);
        app.trigger("transport.play_pause");
        // ... is editable (trim the clip, undo) ...
        const ClipData clip = app.project.timeline()->data().videoTracks[0].clips.front();
        app.project.beginEdit(QStringLiteral("trim"));
        app.project.timeline()->mutableData().videoTracks[0].clips.front().duration
            = clip.duration / 2;
        app.project.endEdit();
        QCOMPARE(app.project.timeline()->data().videoTracks[0].clips.front().duration,
            clip.duration / 2);
        app.trigger("file.save");
        QVERIFY(!app.project.isDirty());
        // ... and renders.
        Compositor compositor;
        ExportSettings settings;
        settings.outputPath = path(QStringLiteral("render.mp4"));
        ExportJob job(settings, app.project.timeline()->snapshot(),
            makeMediaPathMap(app.project.mediaPool()->state()));
        job.setCompositor(&compositor);
        const ExportResult result = job.run({});
        QVERIFY2(result.ok, qPrintable(result.error));
        const media::MediaInfo info = media::probe(settings.outputPath);
        QVERIFY(info.valid && info.hasVideo);
        QVERIFY(test::meanLuma(test::decodeAt(settings.outputPath, 200'000)) > 5);
        prefs::setProjectLibraryDirectory({});
    }

    void importCancelAndDuplicateNotice()
    {
        App app;
        const QString file = newSingleFile(app, QStringLiteral("Imports"));
        const QString a = copyMedia(QStringLiteral("in/a.mp4"));
        // Cancel: nothing imported, nothing stored.
        cancelProgress(true);
        QVERIFY(importMediaWithFeedback(app.ctx, {a}, app.window.get()).isEmpty());
        cancelProgress(false);
        QVERIFY(firstFile(app.project).id.isEmpty()); // only the timeline item
        QCOMPARE(int(app.controller()->package()->media().size()), 0);
        // Import, then the same content again under another name: stored once, with a notice.
        QCOMPARE(importMediaWithFeedback(app.ctx, {a}, app.window.get()).size(), 1);
        const QString b = copyMedia(QStringLiteral("in/b.mp4"));
        answerNextMessageBox(QStringLiteral("OK")); // "already in the media pool"
        QVERIFY(importMediaWithFeedback(app.ctx, {b}, app.window.get()).isEmpty());
        QCOMPARE(int(app.controller()->package()->media().size()), 1);
        QTRY_COMPARE(int(notices().size()), 1);
        QVERIFY(notices().front()->text().contains(QStringLiteral("b.mp4")));
        closeNotices();
        // A subtitle file is stored too and its captions are read from inside the file.
        const QString srt = path(QStringLiteral("in/captions.srt"));
        {
            QFile f(srt);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("1\n00:00:00,000 --> 00:00:01,000\nHello\n");
        }
        const QStringList subs = importMediaWithFeedback(app.ctx, {srt}, app.window.get());
        QCOMPARE(subs.size(), 1);
        const MediaItem* sub = app.project.mediaPool()->item(subs.front());
        QVERIFY(sub->path.startsWith(file + QLatin1Char('/')));
        QCOMPARE(int(sub->cues.size()), 1);
        // Relink in a single-file project stores the found file.
        app.project.beginEdit(QStringLiteral("linked"));
        const ImportResult linked = app.project.importMedia({copyMedia(
            QStringLiteral("in/still.jpg"), "still.jpg")}); // bypasses the store: a linked item
        app.project.endEdit();
        QCOMPARE(linked.importedIds.size(), 1);
        QVERIFY(isLinkedFile(app.project.mediaPool()->item(linked.importedIds.front())->path));
        QCOMPARE(storeItemsInProject(app.ctx, linked.importedIds, app.window.get()), 1);
        QVERIFY(isStoredInProject(app.project.mediaPool()->item(linked.importedIds.front())->path));
    }

    void mediaPoolShowsWhereEachClipIs()
    {
        App app;
        newSingleFile(app, QStringLiteral("Indicator"));
        const QStringList stored = importMediaWithFeedback(
            app.ctx, {copyMedia(QStringLiteral("m/a.mp4"))}, app.window.get());
        QCOMPARE(stored.size(), 1);
        app.project.beginEdit(QStringLiteral("linked"));
        const QString jpg = copyMedia(QStringLiteral("m/still.jpg"), "still.jpg");
        const QString linkedId = app.project.importMedia({jpg}).importedIds.value(0);
        app.project.endEdit();
        QVERIFY(!linkedId.isEmpty());
        MediaPoolModel* model = app.poolModel();
        const QModelIndex s = model->index(model->rowOf(stored.front()), 0);
        const QModelIndex l = model->index(model->rowOf(linkedId), 0);
        QCOMPARE(s.data(MediaPoolModel::LocationRole).toInt(), int(MediaPoolModel::StoredLocation));
        QCOMPARE(l.data(MediaPoolModel::LocationRole).toInt(), int(MediaPoolModel::LinkedLocation));
        QCOMPARE(
            s.data(MediaPoolModel::LocationBadgeRole).toInt(), int(MediaPoolModel::StoredLocation));
        QCOMPARE(
            l.data(MediaPoolModel::LocationBadgeRole).toInt(), int(MediaPoolModel::LinkedLocation));
        QCOMPARE(s.siblingAtColumn(MediaPoolModel::LocationColumn).data().toString(),
            QStringLiteral("Project file"));
        QCOMPARE(l.siblingAtColumn(MediaPoolModel::LocationColumn).data().toString(),
            QStringLiteral("Linked"));
        QCOMPARE(model->headerData(MediaPoolModel::LocationColumn, Qt::Horizontal, Qt::DisplayRole)
                     .toString(),
            QStringLiteral("Location"));
        QVERIFY(s.data(Qt::ToolTipRole)
                    .toString()
                    .contains(QStringLiteral("Stored in the project file")));
        QVERIFY(l.data(Qt::ToolTipRole).toString().contains(QStringLiteral("Linked")));
        QVERIFY(mediaLocationText(app.project.mediaPool()->item(stored.front())->path)
                    .contains(QStringLiteral("Indicator.kinoklip")));
        // A linked project shows no badges (everything is linked there).
        App linkedApp;
        const QString kkproj = path(QStringLiteral("plain.kkproj"));
        test::ProjectSpec spec;
        spec.mediaFile = jpg;
        QVERIFY(test::writeProject(kkproj, spec));
        QVERIFY(linkedApp.controller()->openFile(kkproj, false));
        const QModelIndex p = linkedApp.poolModel()->index(
            linkedApp.poolModel()->rowOf(firstFile(linkedApp.project).id), 0);
        QCOMPARE(p.data(MediaPoolModel::LocationRole).toInt(), int(MediaPoolModel::LinkedLocation));
        QCOMPARE(
            p.data(MediaPoolModel::LocationBadgeRole).toInt(), int(MediaPoolModel::NoLocation));
        // The thumbnails paint with the badges (smoke test; the screenshot shows them).
        app.window->resize(1600, 900);
        QVERIFY(!app.window->editPage()->mediaPool()->grab().isNull());
    }

    void packCompactVerifyUnpackThroughTheFileMenu()
    {
        App app;
        const QString linked = path(QStringLiteral("trip/Trip.kkproj"));
        QDir().mkpath(QFileInfo(linked).absolutePath());
        test::ProjectSpec spec;
        spec.mediaFile = copyMedia(QStringLiteral("trip/clip.mp4"));
        QVERIFY(test::writeProject(linked, spec));
        QVERIFY(app.controller()->openFile(linked, false));
        QVERIFY(app.action("file.pack_project")->isEnabled());
        QVERIFY(!app.action("file.unpack_project")->isEnabled());
        QVERIFY(!app.action("file.compact_project")->isEnabled());
        QVERIFY(!app.action("file.verify_project")->isEnabled());

        // Pack, cancelled: no file.
        const QString packed = path(QStringLiteral("Trip.kinoklip"));
        answerFileDialogs({packed});
        cancelProgress(true);
        app.trigger("file.pack_project");
        cancelProgress(false);
        QVERIFY(!QFileInfo::exists(packed));
        QCOMPARE(app.project.filePath(), linked);
        // Pack and open the packed file.
        answerNextMessageBox(QStringLiteral("Open Packed Project"));
        app.trigger("file.pack_project");
        QCOMPARE(app.project.filePath(), packed);
        QVERIFY(app.controller()->isSingleFile());
        QVERIFY(!app.action("file.pack_project")->isEnabled());
        QVERIFY(app.action("file.compact_project")->isEnabled());
        const MediaItem clip = firstFile(app.project);
        QVERIFY(clip.path.startsWith(packed + QLatin1Char('/')));

        // An unused file and old saves: Compact shows the sizes and removes them.
        const QStringList extra = importMediaWithFeedback(
            app.ctx, {copyMedia(QStringLiteral("trip/extra.jpg"), "still.jpg")}, app.window.get());
        QCOMPARE(extra.size(), 1);
        app.trigger("file.save");
        app.project.beginEdit(QStringLiteral("remove"));
        app.project.mediaPool()->removeItem(extra.front());
        app.project.endEdit();
        app.trigger("file.save");
        QCOMPARE(int(app.controller()->package()->unusedMedia().size()), 1);
        const qint64 before = QFileInfo(packed).size();
        answerNextMessageBox(QStringLiteral("Cancel")); // the question: nothing happens
        app.trigger("file.compact_project");
        QCOMPARE(QFileInfo(packed).size(), before);
        QString question;
        onNextMessageBox([&question](QMessageBox* box) {
            question = box->objectName() + QLatin1Char('|') + box->text() + QLatin1Char('|')
                + box->informativeText();
            for (QAbstractButton* b : box->buttons()) {
                if (b->text().remove(QLatin1Char('&')) == QLatin1String("Compact"))
                    b->click();
            }
        });
        app.trigger("file.compact_project");
        QVERIFY(question.startsWith(QStringLiteral("CompactQuestion|")));
        QVERIFY(question.contains(QStringLiteral("Current size")));
        QVERIFY(question.contains(QStringLiteral("Estimated size")));
        QVERIFY(question.contains(QStringLiteral("extra.jpg")));
        QVERIFY(QFileInfo(packed).size() < before);
        QVERIFY(app.controller()->package());
        QVERIFY(app.controller()->package()->unusedMedia().isEmpty());
        QVERIFY(!app.project.mediaPool()->isOffline(clip.id));
        QVERIFY(!test::decodeAt(clip.path, 400'000).isNull()); // read from the new file
        app.trigger("file.save"); // the container is writable again after the rewrite
        closeNotices();

        // Verify: a clean file gives a notice; a damaged byte the problem list.
        app.trigger("file.verify_project");
        QTRY_COMPARE(int(notices().size()), 1);
        QVERIFY(notices().front()->text().contains(QStringLiteral("No problems")));
        closeNotices();

        // Unpack, cancelled: still the single-file project.
        const QString folder = path(QStringLiteral("unpacked"));
        QDir().mkpath(folder);
        answerFileDialogs({folder});
        cancelProgress(true);
        app.trigger("file.unpack_project");
        cancelProgress(false);
        QCOMPARE(app.project.filePath(), packed);
        QVERIFY(!QFileInfo::exists(folder + QStringLiteral("/Trip.kkproj")));
        // Unpack: a linked project in the folder, media next to it.
        app.trigger("file.unpack_project");
        QCOMPARE(app.project.filePath(), folder + QStringLiteral("/Trip.kkproj"));
        QVERIFY(!app.controller()->isSingleFile());
        QVERIFY(!app.controller()->package());
        const MediaItem out = firstFile(app.project);
        QVERIFY2(out.path.startsWith(folder + QStringLiteral("/media/")), qPrintable(out.path));
        QVERIFY(QFileInfo(out.path).isFile());
        QVERIFY(!app.project.mediaPool()->isOffline(out.id));
        QVERIFY(app.action("file.pack_project")->isEnabled());
        closeNotices();

        // Damage a media byte inside the packed file: Verify lists the problem.
        App other;
        QVERIFY(other.controller()->openFile(packed, false));
        pack::EntryLocation where;
        QVERIFY(pack::locateEmbedded(firstFile(other.project).path, &where));
        {
            QFile f(packed);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.seek(where.dataOffset + where.size / 2));
            const char x = 0x5a;
            f.write(&x, 1);
        }
        QString shown;
        onNextMessageBox([&shown](QMessageBox* box) {
            shown = box->objectName();
            box->reject();
        });
        other.trigger("file.verify_project");
        QCOMPARE(shown, QStringLiteral("VerifyProblems"));
    }

    void saveAsBetweenTheKinds()
    {
        App app;
        const QString linked = path(QStringLiteral("a/Story.kkproj"));
        QDir().mkpath(QFileInfo(linked).absolutePath());
        test::ProjectSpec spec;
        spec.mediaFile = copyMedia(QStringLiteral("a/clip.mp4"));
        QVERIFY(test::writeProject(linked, spec));
        QVERIFY(app.controller()->openFile(linked, false));
        // Linked -> single file (unsaved change included).
        app.project.setName(QStringLiteral("Story changed"));
        const QString single = path(QStringLiteral("b/Story.kinoklip"));
        QDir().mkpath(QFileInfo(single).absolutePath());
        answerFileDialogs({single});
        QVERIFY(app.controller()->saveAs());
        QCOMPARE(app.project.filePath(), single);
        QCOMPARE(app.project.name(), QStringLiteral("Story changed"));
        QVERIFY(!app.project.isDirty());
        QVERIFY(firstFile(app.project).path.startsWith(single + QLatin1Char('/')));
        // Single file -> single file elsewhere.
        const QString copy = path(QStringLiteral("c/Story copy.kinoklip"));
        QDir().mkpath(QFileInfo(copy).absolutePath());
        answerFileDialogs({copy});
        QVERIFY(app.controller()->saveAs());
        QCOMPARE(app.project.filePath(), copy);
        QVERIFY(firstFile(app.project).path.startsWith(copy + QLatin1Char('/')));
        // Single file -> linked = Unpack next to the chosen file.
        const QString back = path(QStringLiteral("d/Story linked.kkproj"));
        QDir().mkpath(QFileInfo(back).absolutePath());
        answerFileDialogs({back});
        QVERIFY(app.controller()->saveAs());
        closeNotices();
        QCOMPARE(app.project.filePath(), back);
        QVERIFY(!app.controller()->isSingleFile());
        const QString media = firstFile(app.project).path;
        QVERIFY2(media.startsWith(path(QStringLiteral("d/media/"))), qPrintable(media));
        QVERIFY(!QFileInfo::exists(path(QStringLiteral("d/Story copy.kkproj")))); // unpack's own
    }

    void recoveryOfASingleFileProject()
    {
        const QString media = copyMedia(QStringLiteral("r/clip.mp4"));
        QString file;
        QString autosaveCopy;
        QString autosaveFile;
        {
            App app(true);
            file = newSingleFile(app, QStringLiteral("Crashy"));
            const QStringList ids = importMediaWithFeedback(app.ctx, {media}, app.window.get());
            QCOMPARE(ids.size(), 1);
            app.append(ids.front());
            app.trigger("file.save");
            app.append(ids.front()); // unsaved: two clips
            QTest::qWait(20); // the autosave strictly newer than the project file
            QVERIFY(app.autosave()->autosaveNow());
            autosaveFile = app.autosave()->autosavePath();
            QVERIFY(QFileInfo::exists(autosaveFile));
            // Outside the container, never inside it.
            QVERIFY(!autosaveFile.startsWith(QFileInfo(file).absolutePath()));
            // "crash": keep the autosave the normal shutdown would delete.
            autosaveCopy = path(QStringLiteral("autosave-copy.kkproj"));
            QVERIFY(QFile::copy(autosaveFile, autosaveCopy));
        }
        if (!QFileInfo::exists(autosaveFile))
            QVERIFY(QFile::copy(autosaveCopy, autosaveFile));
        QFile(autosaveFile)
            .setFileTime(
                QDateTime::currentDateTime().addSecs(5), QFileDevice::FileModificationTime);
        QVERIFY(QFile::remove(media)); // the original is gone: the clip lives in the file

        App app(true);
        answerNextMessageBox(QStringLiteral("Restore"));
        QVERIFY(app.controller()->offerRecovery());
        QCOMPARE(app.project.filePath(), file);
        QCOMPARE(app.clipCount(), size_t(2));
        QVERIFY(app.project.isDirty());
        QVERIFY(app.controller()->package());
        QVERIFY(!app.controller()->isReadOnly());
        const MediaItem item = firstFile(app.project);
        QVERIFY(!app.project.mediaPool()->isOffline(item.id));
        app.trigger("file.save"); // into the container again
        QVERIFY(!app.project.isDirty());
        QVERIFY(!QFileInfo::exists(autosaveFile));

        App again;
        QVERIFY(again.controller()->openFile(file, false));
        QCOMPARE(again.clipCount(), size_t(2));
    }

    void lockedOrReadOnlyFilesOpenReadOnly()
    {
        App first;
        const QString file = newSingleFile(first, QStringLiteral("Shared"));
        QVERIFY(ProjectLock::isLocked(file));
        // A second window: read-only with a notice, and in the title.
        App second;
        QVERIFY(second.controller()->openFile(file, false));
        QVERIFY(second.controller()->isReadOnly());
        QVERIFY(second.window->windowTitle().contains(QStringLiteral("(Read-Only)")));
        QVERIFY(!second.action("file.compact_project")->isEnabled());
        QTRY_VERIFY(!notices().isEmpty());
        QVERIFY(notices().front()->text().contains(QStringLiteral("read-only")));
        closeNotices();
        // Importing into it is refused (nothing can be stored).
        answerNextMessageBox(QStringLiteral("OK"));
        QVERIFY(importMediaWithFeedback(
            second.ctx, {copyMedia(QStringLiteral("x.mp4"))}, second.window.get())
                    .isEmpty());
        // Save goes to Save As (cancelled here).
        second.project.setName(QStringLiteral("Changed"));
        answerFileDialogs({});
        answerNextMessageBox(QStringLiteral("OK"));
        QVERIFY(!second.controller()->save());
        // The first window still writes.
        first.project.setName(QStringLiteral("Shared 2"));
        QVERIFY(first.controller()->save());
        // A file without write permission opens read-only too.
        first.trigger("file.new");
        QVERIFY(!ProjectLock::isLocked(file));
        QFile::setPermissions(file, QFileDevice::ReadOwner);
        App third;
        QVERIFY(third.controller()->openFile(file, false));
        QVERIFY(third.controller()->isReadOnly());
        closeNotices();
        QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
};

QTEST_MAIN(TestSingleFile)
#include "test_singlefile.moc"
