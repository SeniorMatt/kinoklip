// SPDX-License-Identifier: GPL-3.0-or-later
// test_folderimport — Import Media Folder (spec 33.21 - 33.29, offscreen): bins for nested folders,
// subfolders off, skipped files and the failure list, natural order, an image sequence as one
// clip, one undo step, cancel, duplicates, a symlink loop, a folder dropped on the Media Pool,
// the storage browser's folder commands and File > Import Media Folder through its action.
#include "PanelTestUtils.h"
#include "core/MediaPool.h"
#include "ui/common/FolderImport.h"
#include "ui/common/ProgressTask.h"
#include "ui/common/Resources.h"
#include "ui/mediapool/MediaBrowserPanel.h"
#include "ui/mediapool/MediaPoolPanel.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QListView>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressDialog>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUndoStack>
#include <QUrl>

using namespace kk;
using namespace kk::test;

namespace {

#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
using DragPos = QPointF;
#else
using DragPos = QPoint;
#endif

QString testMedia(const char* name)
{
    return QStringLiteral(KK_TEST_DATA_DIR "/generated/") + QLatin1String(name);
}

/// Copies the test fixture @p source to @p to (folders created).
void copyMedia(const QString& to, const char* source)
{
    QDir().mkpath(QFileInfo(to).absolutePath());
    QVERIFY2(QFile::copy(testMedia(source), to), qPrintable(to));
}

/// Writes a small PNG.
void writePng(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QImage image(32, 18, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(path));
}

void writeText(const QString& path, const QByteArray& bytes = "x")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}

/// Bin of @p pool called @p name inside @p parentId, or nullptr.
const Bin* findBin(const MediaPool& pool, const QString& name, const QString& parentId = {})
{
    for (const Bin& b : pool.bins()) {
        if (b.name == name && b.parentId == parentId)
            return &b;
    }
    return nullptr;
}

/// Names of the items in bin @p binId, in pool order.
QStringList namesIn(const MediaPool& pool, const QString& binId)
{
    QStringList out;
    for (const QString& id : pool.itemIdsInBin(binId)) {
        const MediaItem* m = pool.item(id);
        if (!m->isTimeline())
            out.append(m->name);
    }
    return out;
}

/// Closes the next modal message box (the folder import summary) and sets @p seen; retries
/// until one is shown.
void closeSummary(bool* seen)
{
    auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
    if (!box) {
        QTimer::singleShot(10, qApp, [seen] { closeSummary(seen); });
        return;
    }
    *seen = box->text().contains(QStringLiteral("imported"))
        && box->informativeText().contains(QStringLiteral("already in the media pool"));
    box->accept();
}

int fileItems(const MediaPool& pool)
{
    int n = 0;
    for (const MediaItem& m : pool.items())
        n += m.isTimeline() ? 0 : 1;
    return n;
}

} // namespace

class TestFolderImport : public QObject
{
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_shoot;

    QString path(const QString& name) const { return m_dir->filePath(name); }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_folderimport"));
        QSettings().clear();
        initResources();
    }

    void init()
    {
        if (!QFileInfo::exists(testMedia("h264_25fps.mp4")))
            QSKIP("test media missing (tests/data/generated)");
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        // Shoot/
        //   clip10.mp4 clip2.mp4 clip1.mp3   (natural order: clip1, clip2, clip10)
        //   Day1/still.jpg  Day1/Cam B/tone.mp3
        //   Empty/readme.txt                  (no bin: nothing importable)
        //   .hidden/x.mp3, .DS_Store, Thumbs.db, notes.txt, broken.mp4 (fails the probe)
        m_shoot = path(QStringLiteral("Shoot"));
        copyMedia(path(QStringLiteral("Shoot/clip10.mp4")), "h264_25fps.mp4");
        copyMedia(path(QStringLiteral("Shoot/clip2.mp4")), "h264_25fps.mp4");
        copyMedia(path(QStringLiteral("Shoot/clip1.mp3")), "sine.mp3");
        copyMedia(path(QStringLiteral("Shoot/Day1/still.jpg")), "still.jpg");
        copyMedia(path(QStringLiteral("Shoot/Day1/Cam B/tone.mp3")), "sine.mp3");
        writeText(path(QStringLiteral("Shoot/Empty/readme.txt")));
        copyMedia(path(QStringLiteral("Shoot/.hidden/x.mp3")), "sine.mp3");
        writeText(path(QStringLiteral("Shoot/.DS_Store")));
        writeText(path(QStringLiteral("Shoot/Thumbs.db")));
        writeText(path(QStringLiteral("Shoot/notes.txt")));
        writeText(path(QStringLiteral("Shoot/broken.mp4")), QByteArray(4096, 'z'));
    }

    void cleanup()
    {
        setProgressDialogHookForTests({});
        setFolderImportOptionsHandlerForTests({});
        m_dir.reset();
    }

    void nestedFoldersCreateBinsInNaturalOrder()
    {
        PanelFixture f;
        const MediaPool& pool = *f.project.mediaPool();
        const FolderImportResult r = importMediaFolders(f.ctx, {m_shoot}, {}, nullptr);
        QVERIFY(!r.cancelled);
        QCOMPARE(r.importedIds.size(), 5);
        const Bin* shoot = findBin(pool, QStringLiteral("Shoot"));
        QVERIFY(shoot);
        const Bin* day1 = findBin(pool, QStringLiteral("Day1"), shoot->id);
        QVERIFY(day1);
        const Bin* camB = findBin(pool, QStringLiteral("Cam B"), day1->id);
        QVERIFY(camB);
        QVERIFY(!findBin(pool, QStringLiteral("Empty"), shoot->id));
        QCOMPARE(int(pool.bins().size()), 3);
        QCOMPARE(r.createdBinIds.size(), 3);
        QCOMPARE(namesIn(pool, shoot->id),
            (QStringList {QStringLiteral("clip1.mp3"), QStringLiteral("clip2.mp4"),
                QStringLiteral("clip10.mp4")}));
        QCOMPARE(namesIn(pool, day1->id), QStringList {QStringLiteral("still.jpg")});
        QCOMPARE(namesIn(pool, camB->id), QStringList {QStringLiteral("tone.mp3")});

        // Skipped and failed files: counted, the failure listed once.
        QCOMPARE(r.skippedHidden, 3); // .hidden, .DS_Store, Thumbs.db
        QCOMPARE(r.skippedUnsupported, 2); // notes.txt, Empty/readme.txt
        QCOMPARE(r.failures.size(), 1);
        QVERIFY(r.failures.front().contains(QStringLiteral("broken.mp4")));

        // Into a chosen bin: the folder's bin goes inside it.
        PanelFixture g;
        const QString target = g.project.mediaPool()->addBin(QStringLiteral("Selects"));
        QFile::remove(path(QStringLiteral("Shoot/broken.mp4")));
        QCOMPARE(importMediaFolders(g.ctx, {m_shoot}, {}, nullptr, target).importedIds.size(), 5);
        QVERIFY(findBin(*g.project.mediaPool(), QStringLiteral("Shoot"), target));
    }

    void subfoldersOffAndNoBins()
    {
        PanelFixture f;
        const MediaPool& pool = *f.project.mediaPool();
        FolderImportOptions topOnly;
        topOnly.includeSubfolders = false;
        topOnly.createBins = false;
        const FolderImportResult r = importMediaFolders(f.ctx, {m_shoot}, topOnly, nullptr);
        QCOMPARE(r.importedIds.size(), 3);
        QVERIFY(pool.bins().isEmpty());
        QCOMPARE(namesIn(pool, {}),
            (QStringList {QStringLiteral("clip1.mp3"), QStringLiteral("clip2.mp4"),
                QStringLiteral("clip10.mp4")}));

        // Subfolders without bins: everything flat in the target bin.
        PanelFixture g;
        FolderImportOptions flat;
        flat.createBins = false;
        QCOMPARE(importMediaFolders(g.ctx, {m_shoot}, flat, nullptr).importedIds.size(), 5);
        QVERIFY(g.project.mediaPool()->bins().isEmpty());
    }

    void imageSequenceIsOneClip()
    {
        PanelFixture f;
        for (int i = 1; i <= 4; ++i)
            writePng(path(QStringLiteral("Render/frame_%1.png").arg(i, 4, 10, QLatin1Char('0'))));
        const FolderImportResult r
            = importMediaFolders(f.ctx, {path(QStringLiteral("Render"))}, {}, nullptr);
        QCOMPARE(r.importedIds.size(), 1);
        const MediaItem* item = f.project.mediaPool()->item(r.importedIds.front());
        QVERIFY(item->info.isImageSequence);
        QCOMPARE(item->info.frameRate, f.project.settings().fps); // the project's rate
        QVERIFY(item->info.durationUs > 0);
    }

    void oneUndoStepRemovesEverything()
    {
        PanelFixture f;
        const MediaPoolState before = f.project.mediaPool()->state();
        QUndoStack* stack = f.project.undoStack();
        const int steps = stack->count();
        const FolderImportResult r = importMediaFolders(f.ctx, {m_shoot}, {}, nullptr);
        QCOMPARE(r.importedIds.size(), 5);
        QCOMPARE(stack->count(), steps + 1);
        QCOMPARE(stack->undoText(), QStringLiteral("Import Media Folder"));
        stack->undo();
        QVERIFY(f.project.mediaPool()->state() == before);
        QVERIFY(f.project.mediaPool()->bins().isEmpty());
        stack->redo();
        QCOMPARE(fileItems(*f.project.mediaPool()), 5);
        QCOMPARE(int(f.project.mediaPool()->bins().size()), 3);
    }

    void cancelImportsNothing()
    {
        PanelFixture f;
        const MediaPoolState before = f.project.mediaPool()->state();
        const int steps = f.project.undoStack()->count();
        setProgressDialogHookForTests([](QProgressDialog* d) { d->cancel(); });
        const FolderImportResult r = importMediaFolders(f.ctx, {m_shoot}, {}, nullptr);
        setProgressDialogHookForTests({});
        QVERIFY(r.cancelled);
        QVERIFY(r.importedIds.isEmpty());
        QVERIFY(f.project.mediaPool()->state() == before); // rolled back: no bins, no items
        QCOMPARE(f.project.undoStack()->count(), steps);
        // The feedback variant shows nothing for a cancel and returns no ids.
        setProgressDialogHookForTests([](QProgressDialog* d) { d->cancel(); });
        QVERIFY(importMediaFoldersWithFeedback(f.ctx, {m_shoot}, {}, nullptr).isEmpty());
    }

    void duplicatesAreSkippedAndCounted()
    {
        PanelFixture f;
        // One file imported before with Import Media.
        f.project.importMedia({path(QStringLiteral("Shoot/clip2.mp4"))});
        FolderImportResult r = importMediaFolders(f.ctx, {m_shoot}, {}, nullptr);
        QCOMPARE(r.importedIds.size(), 4);
        QCOMPARE(r.duplicates, 1);
        // The same folder again: nothing new, no empty bins, no undo step.
        const int steps = f.project.undoStack()->count();
        const int bins = int(f.project.mediaPool()->bins().size());
        r = importMediaFolders(f.ctx, {m_shoot}, {}, nullptr);
        QVERIFY(r.importedIds.isEmpty());
        QCOMPARE(r.duplicates, 5);
        QCOMPARE(int(f.project.mediaPool()->bins().size()), bins);
        QCOMPARE(f.project.undoStack()->count(), steps);
    }

    void symlinkLoopDoesNotHang()
    {
        PanelFixture f;
        QVERIFY(QFile::link(m_shoot, path(QStringLiteral("Shoot/Day1/loop"))));
        const FolderImportResult r = importMediaFolders(f.ctx, {m_shoot}, {}, nullptr);
        QCOMPARE(r.importedIds.size(), 5);
        QCOMPARE(r.skippedLinks, 1);
    }

    void folderDroppedOnTheMediaPool()
    {
        QFile::remove(path(QStringLiteral("Shoot/broken.mp4"))); // no summary dialog
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        panel.resize(600, 400);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(m_shoot)});
        auto* grid = panel.findChild<QListView*>();
        QVERIFY(grid);
        const QPoint pos = grid->viewport()->mapTo(&panel, grid->viewport()->rect().center());
        QDragEnterEvent enter(DragPos(pos), Qt::CopyAction, &mime, Qt::LeftButton, {});
        QCoreApplication::sendEvent(&panel, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(DragPos(pos), Qt::CopyAction, &mime, Qt::LeftButton, {});
        drop.setAccepted(false);
        QCoreApplication::sendEvent(&panel, &drop);
        QVERIFY(drop.isAccepted());
        QTRY_COMPARE(fileItems(*f.project.mediaPool()), 5);
        const Bin* shoot = findBin(*f.project.mediaPool(), QStringLiteral("Shoot"));
        QVERIFY(shoot);
        QVERIFY(findBin(*f.project.mediaPool(), QStringLiteral("Day1"), shoot->id));
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Import Media Folder"));
    }

    void storageBrowserFolderCommands()
    {
        QFile::remove(path(QStringLiteral("Shoot/broken.mp4")));
        PanelFixture f;
        MediaBrowserPanel browser(f.ctx);
        // "Add Folder into Media Pool": only the folder's own files, straight into Master.
        QCOMPARE(browser.addFolderToMediaPool(m_shoot, false).size(), 3);
        QVERIFY(f.project.mediaPool()->bins().isEmpty());
        // "... and Subfolders (Create Bins)": the rest, in bins. The three above are already
        // in the pool: one summary says so (closed here).
        bool summary = false;
        QTimer::singleShot(0, qApp, [&summary] { closeSummary(&summary); });
        QCOMPARE(browser.addFolderToMediaPool(m_shoot, true).size(), 2);
        const Bin* shoot = findBin(*f.project.mediaPool(), QStringLiteral("Shoot"));
        QVERIFY(shoot);
        QVERIFY(findBin(*f.project.mediaPool(), QStringLiteral("Day1"), shoot->id));
        QVERIFY(summary);
    }

    void optionsHandlerAndTakeFolders()
    {
        QStringList paths {m_shoot, path(QStringLiteral("Shoot/clip2.mp4"))};
        QCOMPARE(takeFolders(paths), QStringList {m_shoot});
        QCOMPARE(paths, QStringList {path(QStringLiteral("Shoot/clip2.mp4"))});
        // The options dialog remembers the last choice.
        setFolderImportOptionsHandlerForTests([](FolderImportOptions* o) {
            o->createBins = false;
            return true;
        });
        FolderImportOptions options;
        QVERIFY(askFolderImportOptions(nullptr, m_shoot, &options));
        QVERIFY(options.includeSubfolders);
        QVERIFY(!options.createBins);
        bool sawCreateBins = true;
        setFolderImportOptionsHandlerForTests([&](FolderImportOptions* o) {
            sawCreateBins = o->createBins;
            return false; // cancelled
        });
        QVERIFY(!askFolderImportOptions(nullptr, m_shoot, &options));
        QVERIFY(!sawCreateBins);
        QSettings().clear();
    }
};

QTEST_MAIN(TestFolderImport)
#include "test_folderimport.moc"
