// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen drag-and-drop tests of the drop targets outside the timeline (media pool item area,
// bins tree, inspector) and of the drag sources' advertised actions, plus the file list parsing
// of platform::droppedLocalFiles() for every format a Wayland source may offer. The timeline is
// covered by tests/timeline/test_timelinedrop.cpp, the compositor path by
// tests/app/test_wayland.cpp.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/MediaPool.h"
#include "platform/DroppedFiles.h"
#include "ui/common/DragMime.h"
#include "ui/common/Resources.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/mediapool/MediaPoolModel.h"
#include "ui/mediapool/MediaPoolPanel.h"

#include <QApplication>
#include <QImage>
#include <QListView>
#include <QMimeData>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QUrl>

using namespace kk;
using namespace kk::test;

namespace {

#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
using DragPos = QPointF; // Qt 6.12 deprecates the QPoint constructors of the drag events
#else
using DragPos = QPoint;
#endif

struct Answers
{
    bool enter = false;
    bool move = false;
    bool drop = false;
};

/// DragEnter + DragMove + Drop like Qt sends them for a real drag (each starts unaccepted).
Answers sendDrag(QWidget* target, const QMimeData* mime, QPoint pos)
{
    Answers a;
    QDragEnterEvent enter(DragPos(pos), Qt::CopyAction, mime, Qt::LeftButton, {});
    enter.setAccepted(false);
    QCoreApplication::sendEvent(target, &enter);
    a.enter = enter.isAccepted();
    QDragMoveEvent move(DragPos(pos), Qt::CopyAction, mime, Qt::LeftButton, {});
    move.setAccepted(false);
    QCoreApplication::sendEvent(target, &move);
    a.move = move.isAccepted();
    // Always end with a drop: QApplication remembers the accepting widget as the drag target
    // until a Drop, and a later test's events would reach the deleted widget otherwise. A
    // refused drop is ignored by every target.
    QDropEvent drop(DragPos(pos), Qt::CopyAction, mime, Qt::LeftButton, {});
    drop.setAccepted(false);
    QCoreApplication::sendEvent(target, &drop);
    a.drop = drop.isAccepted();
    return a;
}

QString writeStill(const QTemporaryDir& dir, const QString& name)
{
    QImage image(64, 36, QImage::Format_RGB32);
    image.fill(Qt::blue);
    const QString path = dir.filePath(name);
    image.save(path);
    return path;
}

QTreeWidgetItem* findBin(QTreeWidget* tree, const QString& text)
{
    const auto items = tree->findItems(text, Qt::MatchExactly | Qt::MatchRecursive);
    return items.isEmpty() ? nullptr : items.front();
}

} // namespace

class TestDropTargets : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_droptargets"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    // ---- platform::droppedLocalFiles ----

    void droppedFilesFormats()
    {
        QMimeData uris;
        uris.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/a.mp4")),
            QUrl(QStringLiteral("https://example.org/b.mp4")),
            QUrl::fromLocalFile(QStringLiteral("/run/user/1000/doc/1234abcd/c.mov"))});
        QVERIFY(platform::hasDroppedFiles(&uris));
        // Remote URLs are skipped; document-portal paths are ordinary files.
        QCOMPARE(platform::droppedLocalFiles(&uris),
            QStringList({QStringLiteral("/tmp/a.mp4"),
                QStringLiteral("/run/user/1000/doc/1234abcd/c.mov")}));

        QMimeData kde;
        kde.setData(QStringLiteral("application/x-kde4-urilist"),
            "# comment\r\nfile:///tmp/a%20b.mp4\r\nfile:///tmp/c.wav\r\n");
        QVERIFY(platform::hasDroppedFiles(&kde));
        QCOMPARE(platform::droppedLocalFiles(&kde),
            QStringList({QStringLiteral("/tmp/a b.mp4"), QStringLiteral("/tmp/c.wav")}));

        // text/x-moz-url: UTF-16 "url\ntitle" pairs.
        const QString moz = QStringLiteral("file:///tmp/m.mkv\nm.mkv");
        QMimeData mozData;
        mozData.setData(QStringLiteral("text/x-moz-url"),
            QByteArray(reinterpret_cast<const char*>(moz.utf16()), moz.size() * 2));
        QCOMPARE(platform::droppedLocalFiles(&mozData), QStringList {QStringLiteral("/tmp/m.mkv")});

        QMimeData text;
        text.setText(QStringLiteral("/tmp/a.mp4"));
        QVERIFY(!platform::hasDroppedFiles(&text));
        QVERIFY(platform::droppedLocalFiles(&text).isEmpty());
    }

    // ---- media pool ----

    void mediaPoolItemAreaImportsFiles()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        panel.resize(600, 400);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QTemporaryDir dir;
        const QString path = writeStill(dir, QStringLiteral("one.png"));
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path)});
        // Over the item grid (it does not accept drops itself: the event reaches the panel).
        auto* grid = panel.findChild<QListView*>();
        QVERIFY(grid);
        const QPoint pos = grid->viewport()->mapTo(&panel, grid->viewport()->rect().center());
        const Answers a = sendDrag(&panel, &mime, pos);
        QVERIFY(a.enter);
        QVERIFY(a.move);
        QVERIFY(a.drop);
        QTRY_COMPARE(f.project.mediaPool()->items().size(), 1);

        // Our own media pool payload is not a file drop.
        QMimeData items;
        items.setData(QLatin1String(mime::MediaItems), "x");
        QVERIFY(!sendDrag(&panel, &items, pos).enter);
    }

    void binTreeTakesItemsAndFiles()
    {
        PanelFixture f;
        MediaPoolPanel panel(f.ctx);
        panel.resize(600, 400);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        const QString bin = panel.createBin(QStringLiteral("Selects"), {});
        MediaItem item;
        item.path = QCoreApplication::applicationFilePath();
        item.name = QStringLiteral("Clip");
        const QString id = f.project.mediaPool()->addItem(item);

        QTreeWidget* tree = nullptr;
        for (auto* t : panel.findChildren<QTreeWidget*>()) {
            if (findBin(t, QStringLiteral("Selects")))
                tree = t;
        }
        QVERIFY(tree);
        QTreeWidgetItem* target = findBin(tree, QStringLiteral("Selects"));
        const QPoint pos = tree->viewport()->mapTo(tree, tree->visualItemRect(target).center());

        QMimeData items;
        items.setData(QLatin1String(mime::MediaItems), id.toUtf8());
        const Answers a = sendDrag(tree->viewport(), &items, tree->viewport()->mapFrom(tree, pos));
        QVERIFY(a.enter);
        QVERIFY(a.move);
        QVERIFY(a.drop);
        QCOMPARE(f.project.mediaPool()->item(id)->binId, bin);

        QTemporaryDir dir;
        QMimeData files;
        files.setUrls({QUrl::fromLocalFile(writeStill(dir, QStringLiteral("two.png")))});
        QVERIFY(sendDrag(tree->viewport(), &files, tree->viewport()->mapFrom(tree, pos)).drop);
        QTRY_COMPARE(f.project.mediaPool()->items().size(), 2);
        bool inBin = false;
        for (const MediaItem& m : f.project.mediaPool()->items())
            inBin = inBin || (m.name.startsWith(QLatin1String("two")) && m.binId == bin);
        QVERIFY(inBin);
    }

    // ---- inspector ----

    void inspectorTakesEffects()
    {
        PanelFixture f;
        const quint64 clip = f.addClip({TrackType::Video, 0}, 0, 50, ClipKind::Solid);
        InspectorPanel panel(f.ctx);
        panel.resize(400, 600);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QMimeData effect;
        effect.setData(QLatin1String(mime::Effect), "gaussian_blur");
        // Nothing inspected: refused.
        QVERIFY(!sendDrag(&panel, &effect, panel.rect().center()).enter);
        panel.inspect(clip, InspectorPanel::VideoTab);
        const Answers a = sendDrag(&panel, &effect, panel.rect().center());
        QVERIFY(a.enter);
        QVERIFY(a.move);
        QVERIFY(a.drop);
        QCOMPARE(f.clip(clip)->effects.size(), 1);
    }

    // ---- drag sources ----

    void dragSourcesOfferCopyOnly()
    {
        PanelFixture f;
        MediaPoolModel model(&f.project);
        QCOMPARE(model.supportedDragActions(), Qt::DropActions(Qt::CopyAction));

        EffectsLibraryPanel effects(f.ctx);
        QTreeWidget* tree = nullptr;
        for (auto* t : effects.findChildren<QTreeWidget*>()) {
            if (t->dragEnabled())
                tree = t;
        }
        QVERIFY(tree);
        QCOMPARE(tree->model()->supportedDragActions(), Qt::DropActions(Qt::CopyAction));
        QCOMPARE(tree->defaultDropAction(), Qt::CopyAction);
    }
};

QTEST_MAIN(TestDropTargets)
#include "test_droptargets.moc"
