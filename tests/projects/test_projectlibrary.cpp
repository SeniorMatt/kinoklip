// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectLibrary (spec section 38): listing, metadata and its cache, broken files, folders,
// rename / duplicate / move / trash / import / export, the open project's protection, and a
// library of 300 generated projects.
#include "ProjectTestUtil.h"
#include "core/MediaPool.h"
#include "core/Preferences.h"
#include "projects/ProjectKinds.h"
#include "projects/ProjectLibrary.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace kk;

namespace {
QString g_xdgRoot; ///< isolated XDG dirs next to the test binary (see main())

QString testMedia(const char* name)
{
    return QStringLiteral(KK_TEST_DATA_DIR "/generated/") + QLatin1String(name);
}
} // namespace

class TestProjectLibrary : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_root;
    QString m_cache;

    std::unique_ptr<ProjectLibrary> makeLibrary()
    {
        return std::make_unique<ProjectLibrary>(m_root, m_cache);
    }
    QString project(const QString& name, const test::ProjectSpec& spec = {})
    {
        const QString path = m_root + QLatin1Char('/') + name + QStringLiteral(".kkproj");
        if (!test::writeProject(path, spec))
            qFatal("cannot write %s", qPrintable(path));
        return path;
    }
    QStringList names(const QVector<ProjectEntry>& entries)
    {
        QStringList out;
        for (const ProjectEntry& e : entries)
            out << (e.isFolder ? QStringLiteral("[%1]").arg(e.name) : e.name);
        out.sort();
        return out;
    }

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>(g_xdgRoot + QStringLiteral("/lib-XXXXXX"));
        QVERIFY(m_dir->isValid());
        m_root = m_dir->path() + QStringLiteral("/Kinoklip Projects");
        m_cache = m_dir->path() + QStringLiteral("/cache");
        QDir().mkpath(m_root);
    }
    void cleanup() { m_dir.reset(); }

    void defaultFolderIsVideosKinoklipProjects()
    {
        QVERIFY(
            prefs::projectLibraryDefaultDirectory().endsWith(QStringLiteral("/Kinoklip Projects")));
        QCOMPARE(prefs::projectLibraryDirectory(), prefs::projectLibraryDefaultDirectory());
        prefs::setProjectLibraryDirectory(m_root);
        QCOMPARE(prefs::projectLibraryDirectory(), m_root);
        prefs::setProjectLibraryDirectory({});
        QCOMPARE(prefs::projectLibraryDirectory(), prefs::projectLibraryDefaultDirectory());
        QVERIFY(prefs::startupShowsProjectManager());
        prefs::setStartupShowsProjectManager(false);
        QVERIFY(!prefs::startupShowsProjectManager());
        prefs::setStartupShowsProjectManager(true);
    }

    void listsFoldersAndProjectsOnly()
    {
        QDir(m_root).removeRecursively();
        auto lib = makeLibrary();
        QVERIFY(QFileInfo(m_root).isDir()); // created
        project(QStringLiteral("Alpha"));
        project(QStringLiteral("Beta"));
        QDir(m_root).mkdir(QStringLiteral("Client A"));
        QFile notes(m_root + QStringLiteral("/notes.txt"));
        QVERIFY(notes.open(QIODevice::WriteOnly));
        notes.close();
        QDir(m_root).mkdir(QStringLiteral(".hidden"));
        const QVector<ProjectEntry> entries = lib->list();
        QCOMPARE(names(entries),
            (QStringList {
                QStringLiteral("Alpha"), QStringLiteral("Beta"), QStringLiteral("[Client A]")}));
        QVERIFY(entries.first().isFolder); // folders first
        for (const ProjectEntry& e : entries) {
            if (!e.isFolder) {
                QCOMPARE(e.kindId, QStringLiteral("kkproj"));
                QVERIFY(e.modifiedMs > 0);
                QVERIFY(e.size > 0);
            }
        }
    }

    void metadataIsReadOnAWorkerAndCached()
    {
        test::ProjectSpec spec;
        spec.width = 3840;
        spec.height = 2160;
        spec.fps = {30000, 1001};
        spec.clipFrames = 300;
        const QString path = project(QStringLiteral("UHD"), spec);
        {
            auto lib = makeLibrary();
            const ProjectEntry e = ProjectLibrary::entryFor(path);
            QSignalSpy ready(lib.get(), &ProjectLibrary::metadataReady);
            QVERIFY(!lib->metadata(e).has_value()); // scheduled, not read on this thread
            QVERIFY(ready.wait(5000));
            QCOMPARE(ready.first().first().toString(), e.path);
            const std::optional<ProjectMetadata> m = lib->metadata(e);
            QVERIFY(m.has_value());
            QVERIFY(m->isValid());
            QCOMPARE(m->name, QStringLiteral("UHD"));
            QCOMPARE(m->width, 3840);
            QCOMPARE(m->height, 2160);
            QCOMPARE(m->fps, (Rational {30000, 1001}));
            QCOMPARE(m->durationFrames, 300);
            QCOMPARE(m->timelineCount, 1);
            QCOMPARE(lib->metadataReads(), 1);
        }
        // A new library (next start) answers from the sidecar cache without opening the file.
        auto lib = makeLibrary();
        const std::optional<ProjectMetadata> m = lib->metadata(ProjectLibrary::entryFor(path));
        QVERIFY(m.has_value());
        QCOMPARE(m->width, 3840);
        QCOMPARE(lib->metadataReads(), 0);
        QVERIFY(QFileInfo::exists(m_cache + QStringLiteral("/metadata.json")));
        // A changed file (new modification time / size) is read again.
        QTest::qWait(20);
        spec.width = 1280;
        spec.height = 720;
        QVERIFY(test::writeProject(path, spec));
        QVERIFY(!lib->metadata(ProjectLibrary::entryFor(path)).has_value());
        lib->waitForIdle();
        QCOMPARE(lib->metadata(ProjectLibrary::entryFor(path))->width, 1280);
    }

    void brokenFilesGiveAnErrorAndDoNotStopTheOthers()
    {
        auto lib = makeLibrary();
        const QString good = project(QStringLiteral("Good"));
        const auto write = [&](const QString& name, const QByteArray& bytes) {
            QFile f(m_root + QLatin1Char('/') + name);
            if (!f.open(QIODevice::WriteOnly))
                qFatal("cannot write %s", qPrintable(f.fileName()));
            f.write(bytes);
            return f.fileName();
        };
        const QString garbage = write(QStringLiteral("Garbage.kkproj"), "\x00\x01not json at all");
        const QString notOurs = write(QStringLiteral("Other.kkproj"), R"({"hello": 1})");
        const QString newer = write(QStringLiteral("Newer.kkproj"),
            QByteArray(R"({"formatVersion": 9999, "timelines": []})"));
        const QString empty = write(QStringLiteral("Empty.kkproj"), QByteArray());
        QCOMPARE(lib->list().size(), 5);
        for (const ProjectEntry& e : lib->list())
            lib->metadata(e);
        lib->waitForIdle();
        QVERIFY(lib->metadata(ProjectLibrary::entryFor(good))->isValid());
        for (const QString& bad : {garbage, notOurs, newer, empty}) {
            const std::optional<ProjectMetadata> m = lib->metadata(ProjectLibrary::entryFor(bad));
            QVERIFY2(m.has_value(), qPrintable(bad));
            QVERIFY2(!m->isValid(), qPrintable(bad));
        }
        QVERIFY(lib->metadata(ProjectLibrary::entryFor(newer))
                    ->error.contains(QStringLiteral("newer")));
    }

    void missingMediaAndPosterSource()
    {
        const QString mediaFile = testMedia("h264_25fps.mp4");
        if (!QFileInfo::exists(mediaFile))
            QSKIP("test media missing (tests/data/generated)");
        test::ProjectSpec spec;
        spec.mediaFile = mediaFile;
        const QString path = project(QStringLiteral("WithMedia"), spec);
        auto lib = makeLibrary();
        ProjectMetadata m = lib->metadataNow(ProjectLibrary::entryFor(path));
        QVERIFY(m.isValid());
        QCOMPARE(m.mediaCount, 1);
        QCOMPARE(m.missingMediaCount, 0);
        QCOMPARE(m.posterMediaPath, mediaFile); // the clip on V2 is on top at the playhead 0
        QCOMPARE(m.posterTimeUs, 500'000);
        // The media goes offline: marked, the poster source stays (the cache may still have it).
        QVERIFY(test::redirectMedia(path, m_dir->path() + QStringLiteral("/gone/clip.mp4")));
        m = lib->metadataNow(ProjectLibrary::entryFor(path));
        QCOMPARE(m.missingMediaCount, 1);
    }

    void postersAreCachedPerVersion()
    {
        const QString path = project(QStringLiteral("Poster"));
        const ProjectEntry e = ProjectLibrary::entryFor(path);
        QImage image(160, 90, QImage::Format_RGB32);
        image.fill(Qt::red);
        {
            auto lib = makeLibrary();
            ProjectLibrary::PosterState state;
            QVERIFY(lib->poster(e, &state).isNull());
            QCOMPARE(state, ProjectLibrary::PosterState::Loading);
            lib->waitForIdle();
            lib->poster(e, &state);
            QCOMPARE(state, ProjectLibrary::PosterState::Absent);
            lib->storePoster(e, image);
            QCOMPARE(lib->poster(e, &state).size(), image.size());
            QCOMPARE(state, ProjectLibrary::PosterState::Ready);
            lib->waitForIdle();
        }
        auto lib = makeLibrary();
        QSignalSpy ready(lib.get(), &ProjectLibrary::posterReady);
        ProjectLibrary::PosterState state;
        lib->poster(e, &state);
        QVERIFY(ready.wait(5000));
        const QImage cached = lib->poster(e, &state);
        QCOMPARE(state, ProjectLibrary::PosterState::Ready);
        QCOMPARE(cached.size(), image.size());
        // A saved project (new modification time) has no poster until one is made again.
        QTest::qWait(20);
        QVERIFY(test::writeProject(path));
        lib->poster(ProjectLibrary::entryFor(path), &state);
        lib->waitForIdle();
        lib->poster(ProjectLibrary::entryFor(path), &state);
        QCOMPARE(state, ProjectLibrary::PosterState::Absent);
    }

    void createRenameDuplicate()
    {
        auto lib = makeLibrary();
        QString error;
        const QString created
            = lib->createProject({}, QStringLiteral("My Film"), QStringLiteral("kkproj"), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        QCOMPARE(QFileInfo(created).fileName(), QStringLiteral("My Film.kkproj"));
        Project loaded;
        QVERIFY(loaded.load(created));
        QCOMPARE(loaded.name(), QStringLiteral("My Film"));
        QVERIFY(lib->createProject({}, QStringLiteral("My Film"), QStringLiteral("kkproj"), &error)
                    .isEmpty()); // taken
        QVERIFY(lib->createProject({}, QStringLiteral("a/b"), QStringLiteral("kkproj"), &error)
                    .isEmpty());
        QVERIFY(lib->createProject({}, QStringLiteral("  "), QStringLiteral("kkproj"), &error)
                    .isEmpty());

        const QString renamed = lib->rename(created, QStringLiteral("Feature"), &error);
        QCOMPARE(QFileInfo(renamed).fileName(), QStringLiteral("Feature.kkproj"));
        QVERIFY(!QFileInfo::exists(created));
        QVERIFY(loaded.load(renamed));
        QCOMPARE(loaded.name(), QStringLiteral("Feature")); // the stored name follows

        const QString copy = lib->duplicate(renamed, &error);
        QCOMPARE(QFileInfo(copy).fileName(), QStringLiteral("Feature copy.kkproj"));
        QVERIFY(loaded.load(copy));
        QCOMPARE(loaded.name(), QStringLiteral("Feature copy"));
        const QString copy2 = lib->duplicate(renamed, &error);
        QCOMPARE(QFileInfo(copy2).fileName(), QStringLiteral("Feature copy 2.kkproj"));
        QVERIFY(lib->rename(copy2, QStringLiteral("Feature"), &error).isEmpty()); // taken
    }

    void foldersAndMoving()
    {
        auto lib = makeLibrary();
        QString error;
        const QString clientA = lib->createFolder({}, QStringLiteral("Client A"), &error);
        const QString sub = lib->createFolder(clientA, QStringLiteral("2026"), &error);
        QVERIFY(QFileInfo(sub).isDir());
        QVERIFY(lib->createFolder({}, QStringLiteral("Client A"), &error).isEmpty());
        const QString p = project(QStringLiteral("Spot"));
        lib->metadataNow(ProjectLibrary::entryFor(p));
        const int reads = lib->metadataReads();
        const QString moved = lib->move(p, sub, &error);
        QCOMPARE(moved, sub + QStringLiteral("/Spot.kkproj"));
        QCOMPARE(names(lib->list(sub)), QStringList {QStringLiteral("Spot")});
        // The metadata moved with it (a rename keeps the modification time).
        QVERIFY(lib->metadata(ProjectLibrary::entryFor(moved)).has_value());
        QCOMPARE(lib->metadataReads(), reads);
        // Moving into the folder it is in is a no-op; a folder never into itself.
        QCOMPARE(lib->move(moved, sub, &error), moved);
        QVERIFY(lib->move(clientA, sub, &error).isEmpty());
        // Back to the root, then rename and move a whole folder.
        QCOMPARE(lib->move(moved, {}, &error), m_root + QStringLiteral("/Spot.kkproj"));
        const QString renamedFolder = lib->rename(clientA, QStringLiteral("Client B"), &error);
        QVERIFY(QFileInfo(renamedFolder + QStringLiteral("/2026")).isDir());
        QVERIFY(lib->contains(renamedFolder));
        QVERIFY(!lib->contains(m_dir->path()));
    }

    void deleteGoesToTheTrash()
    {
        auto lib = makeLibrary();
        const QString p = project(QStringLiteral("Old Cut"));
        QString error;
        QVERIFY2(lib->moveToTrash(p, &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(p));
        // The freedesktop trash of the (isolated) XDG data dir has it, with its .trashinfo.
        const QString trash = qEnvironmentVariable("XDG_DATA_HOME") + QStringLiteral("/Trash");
        QVERIFY2(QFileInfo::exists(trash + QStringLiteral("/files/Old Cut.kkproj"))
                || !QDir(trash + QStringLiteral("/files"))
                        .entryList({QStringLiteral("Old Cut*")})
                        .isEmpty(),
            qPrintable(trash));
        QVERIFY(!QDir(trash + QStringLiteral("/info"))
                     .entryList({QStringLiteral("Old Cut*.trashinfo")})
                     .isEmpty());
        // A folder goes as a whole.
        const QString folder = lib->createFolder({}, QStringLiteral("Archive"), &error);
        project(QStringLiteral("x"));
        QVERIFY(lib->move(m_root + QStringLiteral("/x.kkproj"), folder, &error).size() > 0);
        QVERIFY(lib->moveToTrash(folder, &error));
        QVERIFY(!QFileInfo::exists(folder));
    }

    void theOpenProjectIsProtected()
    {
        auto lib = makeLibrary();
        QString error;
        const QString folder = lib->createFolder({}, QStringLiteral("Work"), &error);
        const QString p = lib->move(project(QStringLiteral("Current")), folder, &error);
        lib->setOpenProjectPath(p);
        QVERIFY(lib->isOpenOrContainsOpen(p));
        QVERIFY(lib->isOpenOrContainsOpen(folder));
        QVERIFY(!lib->moveToTrash(p, &error));
        QVERIFY(error.contains(QStringLiteral("open")));
        QVERIFY(!lib->moveToTrash(folder, &error));
        QVERIFY(lib->rename(p, QStringLiteral("Other"), &error).isEmpty());
        QVERIFY(lib->move(p, {}, &error).isEmpty());
        QVERIFY(lib->move(folder, lib->createFolder({}, QStringLiteral("Elsewhere")), &error)
                    .isEmpty());
        QVERIFY(QFileInfo::exists(p));
        QVERIFY(!lib->duplicate(p, &error).isEmpty()); // copying is fine
        lib->setOpenProjectPath({});
        QVERIFY(lib->moveToTrash(p, &error));
    }

    void importAndExport()
    {
        auto lib = makeLibrary();
        QString error;
        const QString outside = m_dir->path() + QStringLiteral("/elsewhere/Outside.kkproj");
        QDir().mkpath(QFileInfo(outside).absolutePath());
        QVERIFY(test::writeProject(outside));
        const QString folder = lib->createFolder({}, QStringLiteral("Imported"), &error);
        const QString imported = lib->importProject(outside, folder, &error);
        QCOMPARE(imported, folder + QStringLiteral("/Outside.kkproj"));
        QVERIFY(QFileInfo::exists(outside)); // a copy
        QCOMPARE(QFileInfo(lib->importProject(outside, folder, &error)).fileName(),
            QStringLiteral("Outside 2.kkproj"));
        QVERIFY(lib->importProject(m_dir->path() + QStringLiteral("/nothing.txt"), {}, &error)
                    .isEmpty());

        const QString target = m_dir->path() + QStringLiteral("/elsewhere/Backup.kkproj");
        QVERIFY(lib->exportProject(imported, target, &error));
        QVERIFY(lib->exportProject(imported, target, &error)); // overwrite
        Project loaded;
        QVERIFY(loaded.load(target));
    }

    void singleFileProjectsAreCreatedAndRead()
    {
        // Owner decision: two kinds, linked (.kkproj) and single file (.kinoklip); both are
        // created, listed with their details, renamed and duplicated by the library.
        const ProjectKind* single = projectkinds::byStorage(ProjectStorage::SingleFile);
        QVERIFY(single);
        QCOMPARE(single->extensions, QStringList {QStringLiteral("kinoklip")});
        QVERIFY(single->canCreate());
        QVERIFY(projectkinds::byStorage(ProjectStorage::Linked)->canCreate());
        auto lib = makeLibrary();
        QString error;
        const QString packed = lib->createProject({}, QStringLiteral("Packed"), single->id, &error);
        QCOMPARE(packed, m_root + QStringLiteral("/Packed.kinoklip"));
        QFile f(m_root + QStringLiteral("/Empty.kinoklip")); // not a container: an error card
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        project(QStringLiteral("Linked One"));
        const QVector<ProjectEntry> entries = lib->list();
        QCOMPARE(entries.size(), 3);
        for (const ProjectEntry& e : entries) {
            const ProjectMetadata m = lib->metadataNow(e);
            if (e.name == QLatin1String("Empty")) {
                QVERIFY(!m.isValid());
                continue;
            }
            QVERIFY2(m.isValid(), qPrintable(m.error));
            if (e.kindId == QLatin1String("kinoklip")) {
                QCOMPARE(m.name, QStringLiteral("Packed"));
                QCOMPARE(m.timelineCount, 1);
                QCOMPARE(m.width, 1920);
            }
        }
        // Rename changes the name stored inside the container; Duplicate gets its own name.
        const QString renamed = lib->rename(packed, QStringLiteral("Trip"), &error);
        QCOMPARE(renamed, m_root + QStringLiteral("/Trip.kinoklip"));
        QCOMPARE(projectkinds::readKinoklipMetadata(renamed).name, QStringLiteral("Trip"));
        const QString copy = lib->duplicate(renamed, &error);
        QCOMPARE(copy, m_root + QStringLiteral("/Trip copy.kinoklip"));
        QCOMPARE(projectkinds::readKinoklipMetadata(copy).name, QStringLiteral("Trip copy"));
        QVERIFY(lib->moveToTrash(copy, &error));
        QVERIFY(!QFileInfo::exists(copy));
    }

    void largeCopiesReportProgressAndCancelCleanly()
    {
        // Duplicate / Import copy in chunks with progress (a single-file project holds its
        // media); a cancelled copy leaves neither the copy nor its ".part" file.
        auto lib = makeLibrary();
        QString error;
        const QString packed
            = lib->createProject({}, QStringLiteral("Big"), QStringLiteral("kinoklip"), &error);
        QVERIFY2(!packed.isEmpty(), qPrintable(error));
        {
            QFile f(packed); // grow it past one chunk (bytes after the end record are ignored)
            QVERIFY(f.open(QIODevice::Append));
            QVERIFY(f.resize(f.size() + (20 << 20)));
        }
        const QString copyPath = m_root + QStringLiteral("/Big copy.kinoklip");
        bool cancelled = false;
        int calls = 0;
        QVERIFY(lib->duplicate(
                       packed, &error,
                       [&](qint64 done, qint64) {
                           ++calls;
                           return done < (8 << 20); // cancel after the first chunk
                       },
                       &cancelled)
                    .isEmpty());
        QVERIFY(cancelled);
        QVERIFY(calls >= 2);
        QVERIFY(!QFileInfo::exists(copyPath));
        QVERIFY(!QFileInfo::exists(copyPath + QStringLiteral(".part")));
        qint64 last = 0;
        QCOMPARE(lib->duplicate(
                     packed, &error,
                     [&](qint64 done, qint64 total) {
                         last = done;
                         return total > 0;
                     },
                     &cancelled),
            copyPath);
        QVERIFY(!cancelled);
        QCOMPARE(last, QFileInfo(packed).size());
        // (the stored name is then saved anew, which also cuts the padding off the copy)
        QCOMPARE(projectkinds::readKinoklipMetadata(copyPath).name, QStringLiteral("Big copy"));
        // Import from outside, cancelled at once: nothing in the library.
        const QString outside = m_dir->path() + QStringLiteral("/Outside.kinoklip");
        QVERIFY(QFile::copy(packed, outside));
        QDir().mkpath(m_root + QStringLiteral("/In"));
        QVERIFY(lib->importProject(
                       outside, m_root + QStringLiteral("/In"), &error,
                       [](qint64, qint64) { return false; }, &cancelled)
                    .isEmpty());
        QVERIFY(cancelled);
        QVERIFY(QDir(m_root + QStringLiteral("/In")).isEmpty());
        QCOMPARE(lib->importProject(outside, m_root + QStringLiteral("/In"), &error),
            m_root + QStringLiteral("/In/Outside.kinoklip"));
    }

    void exportConvertsBetweenKinds()
    {
        // Export as a single file packs a linked project with its media; exporting a single
        // file as .kkproj unpacks it (media next to the exported project).
        const QString media = testMedia("h264_25fps.mp4");
        if (!QFileInfo::exists(media))
            QSKIP("test media missing (tests/data/generated)");
        test::ProjectSpec spec;
        spec.mediaFile = media;
        const QString linked = project(QStringLiteral("Holiday"), spec);
        auto lib = makeLibrary();
        QString error;
        const QString single = m_dir->path() + QStringLiteral("/out/Holiday.kinoklip");
        QDir().mkpath(QFileInfo(single).absolutePath());
        int calls = 0;
        bool cancelled = false;
        QVERIFY2(lib->exportProject(
                     linked, single, &error,
                     [&](qint64, qint64) {
                         ++calls;
                         return true;
                     },
                     &cancelled),
            qPrintable(error));
        QVERIFY(calls > 0);
        QVERIFY(!cancelled);
        const ProjectMetadata m = projectkinds::readKinoklipMetadata(single);
        QVERIFY2(m.isValid(), qPrintable(m.error));
        QCOMPARE(m.mediaCount, 1);
        QCOMPARE(m.missingMediaCount, 0);
        QVERIFY(m.posterMediaPath.startsWith(single + QLatin1Char('/')));
        // Cancelled: nothing is left behind.
        const QString never = m_dir->path() + QStringLiteral("/out/Never.kinoklip");
        QVERIFY(!lib->exportProject(
            linked, never, &error, [](qint64, qint64) { return false; }, &cancelled));
        QVERIFY(cancelled);
        QVERIFY(!QFileInfo::exists(never));
        // Back to a linked project in another folder.
        const QString back = m_dir->path() + QStringLiteral("/back/Holiday linked.kkproj");
        QDir().mkpath(QFileInfo(back).absolutePath());
        QVERIFY2(lib->exportProject(single, back, &error), qPrintable(error));
        Project loaded;
        QVERIFY(loaded.load(back));
        QCOMPARE(loaded.name(), QStringLiteral("Holiday linked"));
        QString path;
        for (const MediaItem& item : loaded.mediaPool()->state().items) {
            if (!item.isTimeline())
                path = item.path;
        }
        QVERIFY2(path.startsWith(m_dir->path() + QStringLiteral("/back/media/")), qPrintable(path));
        QVERIFY(QFileInfo(path).isFile());
    }

    void projectKindsAreOpen()
    {
        // A second kind (the single-file project of docs/archive/plan-v4 part 2 will be one) is listed and
        // read through its own reader.
        ProjectKind kind;
        kind.id = QStringLiteral("testkind");
        kind.displayName = QStringLiteral("Test");
        kind.extensions = {QStringLiteral("kktest")};
        kind.readMetadata = [](const QString&) {
            ProjectMetadata m;
            m.width = 640;
            m.height = 480;
            return m;
        };
        projectkinds::registerKind(kind);
        QVERIFY(projectkinds::nameFilters().contains(QStringLiteral("*.kktest")));
        auto lib = makeLibrary();
        QFile f(m_root + QStringLiteral("/Packed.kktest"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        const QVector<ProjectEntry> entries = lib->list();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().kindId, QStringLiteral("testkind"));
        QCOMPARE(lib->metadataNow(entries.first()).width, 640);
    }

    /// Spec 38.20: hundreds of projects. Cold: the listing is immediate and the metadata of all
    /// 300 arrives from the workers; warm (next start): everything comes from the cache.
    void threeHundredProjects()
    {
        // Generate one project and copy it (Project::save() 300 times would test the saver).
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
        qint64 listMs = 0;
        qint64 coldMs = 0;
        {
            auto lib = makeLibrary();
            t.start();
            const QVector<ProjectEntry> entries = lib->list();
            listMs = t.elapsed();
            QCOMPARE(entries.size(), 300);
            for (const ProjectEntry& e : entries)
                QVERIFY(!lib->metadata(e).has_value()); // all scheduled, none read here
            lib->waitForIdle();
            coldMs = t.elapsed();
            QCOMPARE(lib->metadataReads(), 300);
        }
        t.restart();
        auto lib = makeLibrary(); // loads the cache
        const QVector<ProjectEntry> entries = lib->list();
        int known = 0;
        for (const ProjectEntry& e : entries)
            known += lib->metadata(e).has_value() ? 1 : 0;
        const qint64 warmMs = t.elapsed();
        QCOMPARE(known, 300);
        QCOMPARE(lib->metadataReads(), 0);
        qInfo("300 projects: list %lld ms, cold metadata %lld ms, warm (cache) %lld ms", listMs,
            coldMs, warmMs);
        QVERIFY2(listMs < 200, "listing must not open the projects");
        QVERIFY2(warmMs < 300, "a cached library must open instantly");
    }
};

int main(int argc, char** argv)
{
    // Isolated XDG dirs (trash, cache, settings) next to the binary: on the home partition, so
    // the trash is $XDG_DATA_HOME/Trash and never the user's.
    QTemporaryDir xdg(QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath()
        + QStringLiteral("/projectlibrary-XXXXXX"));
    g_xdgRoot = xdg.path();
    test::isolateXdg(g_xdgRoot);
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-test"));
    QCoreApplication::setApplicationName(QStringLiteral("projectlibrary"));
    TestProjectLibrary test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_projectlibrary.moc"
