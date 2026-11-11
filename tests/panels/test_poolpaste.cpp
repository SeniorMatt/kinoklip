// SPDX-License-Identifier: GPL-3.0-or-later
// test_poolpaste — Ctrl+V / Edit > Paste in the Media Pool (spec 79, offscreen): files copied in a
// file manager, raw image data saved as a PNG, files preferred over image data, nothing usable,
// undo, and the timeline's paste.
#include "PanelTestUtils.h"
#include "core/MediaPool.h"
#include "ui/common/Resources.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/timeline/TimelineController.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QImage>
#include <QKeyEvent>
#include <QListView>
#include <QMainWindow>
#include <QMimeData>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <QUrl>

using namespace kk;
using namespace kk::test;

namespace {

QImage sampleImage()
{
    QImage image(32, 18, QImage::Format_RGB32);
    image.fill(Qt::green);
    return image;
}

int fileItems(const MediaPool& pool)
{
    int n = 0;
    for (const MediaItem& m : pool.items())
        n += m.isTimeline() ? 0 : 1;
    return n;
}

} // namespace

class TestPoolPaste : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_poolpaste"));
        QSettings().clear();
        initResources();
    }

    void imageDataIsSavedNextToTheProjectAndImported()
    {
        QTemporaryDir dir;
        PanelFixture f;
        f.project.markSaved(dir.filePath(QStringLiteral("show.kinoklip")));
        MediaPoolPanel panel(f.ctx);
        QApplication::clipboard()->setImage(sampleImage());
        const QStringList ids = panel.pasteFromClipboard();
        QCOMPARE(ids.size(), 1);
        QCOMPARE(fileItems(*f.project.mediaPool()), 1);
        const QDir folder(dir.filePath(QStringLiteral("Pasted Media")));
        QCOMPARE(panel.pastedMediaFolder(), folder.absolutePath());
        const QStringList files = folder.entryList({QStringLiteral("Pasted image *.png")});
        QCOMPARE(files.size(), 1);
        QVERIFY(QImage(folder.filePath(files.first())).size() == QSize(32, 18));
        // A second paste in the same second never overwrites.
        QApplication::clipboard()->setImage(sampleImage());
        panel.pasteFromClipboard();
        QCOMPARE(folder.entryList({QStringLiteral("Pasted image *.png")}).size(), 2);
        QCOMPARE(fileItems(*f.project.mediaPool()), 2);
    }

    void unsavedProjectUsesTheAppDataFolder()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        QCOMPARE(panel.pastedMediaFolder(),
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                + QStringLiteral("/Pasted Media"));
        QApplication::clipboard()->setImage(sampleImage());
        QCOMPARE(panel.pasteFromClipboard().size(), 1);
        QVERIFY(
            QDir(panel.pastedMediaFolder()).entryList({QStringLiteral("Pasted image *.png")}).size()
            >= 1);
        QDir(panel.pastedMediaFolder()).removeRecursively();
    }

    void fileUrlsImportIntoTheCurrentBinAndBeatImageData()
    {
        QTemporaryDir dir;
        const QString png = dir.filePath(QStringLiteral("copied.png"));
        QVERIFY(sampleImage().save(png));
        PanelFixture f;
        f.project.markSaved(dir.filePath(QStringLiteral("show.kinoklip")));
        MediaPoolPanel panel(f.ctx);
        const QString bin = f.project.mediaPool()->addBin(QStringLiteral("Stills"), {});
        panel.setCurrentBin(bin);
        auto* mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile(png)});
        mime->setImageData(sampleImage());
        QApplication::clipboard()->setMimeData(mime);
        const QStringList ids = panel.pasteFromClipboard();
        QCOMPARE(ids.size(), 1);
        const MediaItem* item = f.project.mediaPool()->item(ids.first());
        QVERIFY(item);
        QCOMPARE(item->binId, bin);
        QCOMPARE(item->path, png);
        QVERIFY(!QDir(dir.filePath(QStringLiteral("Pasted Media"))).exists()); // no extra copy
        // Undo removes it.
        f.project.undoStack()->undo();
        QCOMPARE(fileItems(*f.project.mediaPool()), 0);
    }

    void ctrlVOnTheViewImports()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        panel.resize(600, 400);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* grid = panel.findChild<QListView*>();
        QVERIFY(grid);
        grid->setFocus();
        QApplication::clipboard()->setImage(sampleImage());
        QTest::keyClick(grid, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(fileItems(*f.project.mediaPool()), 1);
        QDir(panel.pastedMediaFolder()).removeRecursively();
    }

    void nothingUsableGivesAStatusMessageOnly()
    {
        PanelFixture f;
        QMainWindow win;
        auto* panel = new MediaPoolPanel(f.ctx);
        win.setCentralWidget(panel);
        win.show();
        QApplication::clipboard()->setText(QStringLiteral("just words"));
        QVERIFY(panel->pasteFromClipboard().isEmpty());
        QVERIFY(!win.statusBar()->currentMessage().isEmpty());
        QCOMPARE(fileItems(*f.project.mediaPool()), 0);
    }

    void timelinePasteStillWorks()
    {
        PanelFixture f;
        TimelineController c(f.ctx);
        c.connectActions();
        MediaPoolPanel panel(f.ctx);
        const TrackRef v1 {TrackType::Video, 0};
        const quint64 id = f.addClip(v1, 0, 10, ClipKind::Solid);
        f.selection.selectClip(id);
        f.actions.action(QStringLiteral("edit.copy"))->trigger();
        f.project.timeline()->setPlayhead(50);
        // Focus outside the pool: Edit > Paste pastes the clips and imports nothing.
        QApplication::clipboard()->setImage(sampleImage());
        f.actions.action(QStringLiteral("edit.paste"))->trigger();
        QCOMPARE(fileItems(*f.project.mediaPool()), 0);
        QCOMPARE(f.project.timeline()->playhead(), int64_t(60));
        // Focus in the pool: the same command imports the clipboard picture instead.
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* grid = panel.findChild<QListView*>();
        grid->setFocus();
        QTRY_VERIFY(panel.hasFocusInside());
        f.project.timeline()->setPlayhead(100);
        f.actions.action(QStringLiteral("edit.paste"))->trigger();
        QCOMPARE(f.project.timeline()->playhead(), int64_t(100));
        QCOMPARE(fileItems(*f.project.mediaPool()), 1);
        QDir(panel.pastedMediaFolder()).removeRecursively();
    }
};

QTEST_MAIN(TestPoolPaste)
#include "test_poolpaste.moc"
