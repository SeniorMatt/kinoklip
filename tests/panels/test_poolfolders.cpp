// SPDX-License-Identifier: GPL-3.0-or-later
// Sub-bins as folders in the media pool's clip area (Resolve): folders before the clips in the
// grid and the list, double-click opens, drops on a folder move clips into it, rename and
// delete with undo, search hides folders that do not match, Sort by Type with folders first,
// and where imports go (Preferences > Editing > "Import new media into"). Writes a screenshot
// when KK_SCREENSHOT_DIR is set.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/MediaPool.h"
#include "core/PowerBins.h"
#include "core/Preferences.h"
#include "ui/common/DragMime.h"
#include "ui/common/MediaImport.h"
#include "ui/common/Resources.h"
#include "ui/mediapool/BinList.h"
#include "ui/mediapool/MediaPoolDelegate.h"
#include "ui/mediapool/MediaPoolPanel.h"

#include <QApplication>
#include <QDir>
#include <QDropEvent>
#include <QHeaderView>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUndoStack>
#include <QUrl>

using namespace kk;
using namespace kk::test;

namespace {

enum class Kind {
    Video,
    Audio,
    Still
};

QString addItem(
    Project& project, const QString& name, const QString& binId = {}, Kind kind = Kind::Video)
{
    MediaItem item;
    item.path = QCoreApplication::applicationFilePath(); // an existing file
    item.name = name;
    item.binId = binId;
    item.info.valid = true;
    item.info.hasVideo = kind != Kind::Audio;
    item.info.isStillImage = kind == Kind::Still;
    item.info.hasAudio = kind != Kind::Still;
    item.info.width = 1920;
    item.info.height = 1080;
    item.info.frameRate = kind == Kind::Video ? Rational {25, 1} : Rational {0, 1};
    item.info.durationUs = kind == Kind::Still ? 0 : 10'000'000;
    item.info.audioChannels = 2;
    item.info.audioSampleRate = 48000;
    project.beginEdit(QStringLiteral("add"));
    const QString id = project.mediaPool()->addItem(item);
    project.endEdit();
    return id;
}

QString addBin(Project& project, const QString& name, const QString& parent = {})
{
    project.beginEdit(QStringLiteral("New Bin"));
    const QString id = project.mediaPool()->addBin(name, parent);
    project.endEdit();
    return id;
}

QListView* grid(MediaPoolPanel& panel)
{
    for (QListView* v : panel.findChildren<QListView*>()) {
        if (v->viewMode() == QListView::IconMode)
            return v;
    }
    return nullptr;
}

QTreeView* list(MediaPoolPanel& panel)
{
    return panel.findChild<QTreeView*>(QStringLiteral("MediaPoolList"));
}

/// Names of the rows the views show, in order ("[Name]" for folders).
QStringList shown(MediaPoolPanel& panel)
{
    QStringList out;
    const QAbstractItemModel* m = list(panel)->model();
    for (int r = 0; r < m->rowCount(); ++r) {
        const QModelIndex i = m->index(r, MediaPoolModel::NameColumn);
        const QString name = i.data().toString();
        out.append(i.data(MediaPoolModel::FolderRole).toBool()
                ? QLatin1Char('[') + name + QLatin1Char(']')
                : name);
    }
    return out;
}

QModelIndex folderIndex(QAbstractItemView* view, const QString& binId)
{
    const QAbstractItemModel* m = view->model();
    for (int r = 0; r < m->rowCount(); ++r) {
        const QModelIndex i = m->index(r, MediaPoolModel::NameColumn);
        if (i.data(MediaPoolModel::FolderBinRole).toString() == binId)
            return i;
    }
    return {};
}

/// Drags @p data over @p panel and drops it at @p pos (panel coordinates). Returns whether
/// the drop was accepted (Qt delivers a drop only to the widget that accepted the enter).
bool dropAt(MediaPoolPanel& panel, const QPoint& pos, QMimeData* data)
{
    QDragEnterEvent enter(pos, Qt::CopyAction, data, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&panel, &enter);
    QDragMoveEvent move(pos, Qt::CopyAction, data, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&panel, &move);
    // Always end with a drop: QApplication keeps the accepting widget as the drag target
    // until then (test_droptargets).
    QDropEvent drop(QPointF(pos), Qt::CopyAction, data, Qt::LeftButton, Qt::NoModifier);
    drop.setAccepted(false);
    QCoreApplication::sendEvent(&panel, &drop);
    return enter.isAccepted() && move.isAccepted() && drop.isAccepted();
}

/// Drops @p data on the folder of @p binId in the shown view, as a drag from the pool would.
void dropOnFolder(
    MediaPoolPanel& panel, QAbstractItemView* view, const QString& binId, QMimeData* data)
{
    const QModelIndex index = folderIndex(view, binId);
    QVERIFY(index.isValid());
    const QPoint pos = view->viewport()->mapTo(&panel, view->visualRect(index).center());
    QDragEnterEvent enter(pos, Qt::CopyAction, data, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&panel, &enter);
    QVERIFY(enter.isAccepted());
    QDragMoveEvent move(pos, Qt::CopyAction, data, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&panel, &move);
    QVERIFY(move.isAccepted());
    QDropEvent drop(QPointF(pos), Qt::CopyAction, data, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&panel, &drop);
    QVERIFY(drop.isAccepted());
    QCoreApplication::processEvents(); // the move runs from the event loop
}

/// A double-click as a mouse makes it: press, release, double-click, release (Qt 6.4's
/// QTest::mouseDClick sends no release before the double-click, so item views ignore it).
void doubleClick(QWidget* w, const QPoint& at)
{
    const QPoint global = w->mapToGlobal(at);
    for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease,
             QEvent::MouseButtonDblClick, QEvent::MouseButtonRelease}) {
        QMouseEvent e(type, at, global, Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(w, &e);
    }
}

QString stillPath()
{
    return QStringLiteral(KK_TEST_DATA_DIR "/generated/still.jpg");
}

} // namespace

class TestPoolFolders : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_poolfolders"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void init() { QSettings().clear(); }

    // Folders of the current bin's sub-bins come first, in the list and in the grid; a bin's
    // own sub-bins show inside it; the list shows the item count and the type "Bin".
    void foldersInBothViews()
    {
        PanelFixture f;
        const QString shots = addBin(f.project, QStringLiteral("Shots"));
        const QString inner = addBin(f.project, QStringLiteral("Inner"), shots);
        addItem(f.project, QStringLiteral("a.mov"), shots);
        addItem(f.project, QStringLiteral("b.mov"));
        MediaPoolPanel panel(f.ctx);
        panel.setThumbnailProvider({});
        panel.resize(900, 500);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));

        QStringList names = shown(panel);
        QCOMPARE(names.value(0), QStringLiteral("[Shots]"));
        QVERIFY(names.contains(QStringLiteral("b.mov")));
        QVERIFY(!names.contains(QStringLiteral("[Inner]"))); // only direct sub-bins
        QVERIFY(!names.contains(QStringLiteral("a.mov")));
        QVERIFY(!panel.visibleMediaIds().contains(QString())); // folders are no media

        // List view: name, count, type.
        panel.setViewMode(MediaPoolPanel::ViewMode::List);
        panel.setColumnVisible(MediaPoolModel::TypeColumn, true);
        const QModelIndex row = folderIndex(list(panel), shots);
        QVERIFY(row.isValid());
        QVERIFY(list(panel)->visualRect(row).isValid());
        QCOMPARE(row.siblingAtColumn(MediaPoolModel::DurationColumn).data().toString(),
            QStringLiteral("2 items")); // a.mov + Inner
        QCOMPARE(row.siblingAtColumn(MediaPoolModel::TypeColumn).data().toString(),
            QStringLiteral("Bin"));
        QVERIFY(row.siblingAtColumn(MediaPoolModel::CodecColumn).data().toString().isEmpty());
        QVERIFY(!row.data(Qt::DecorationRole).value<QIcon>().isNull());

        // Grid view: a tile too.
        panel.setViewMode(MediaPoolPanel::ViewMode::Grid);
        QVERIFY(grid(panel)->isVisible());
        QVERIFY(grid(panel)->visualRect(folderIndex(grid(panel), shots)).isValid());

        // Inside Shots: its sub-bin and its clip.
        panel.setCurrentBin(shots);
        names = shown(panel);
        QCOMPARE(names.value(0), QStringLiteral("[Inner]"));
        QVERIFY(names.contains(QStringLiteral("a.mov")));
        QVERIFY(!names.contains(QStringLiteral("[Shots]")));
        Q_UNUSED(inner);

        // A new bin appears as a folder at once; a renamed one updates.
        panel.setCurrentBin({});
        addBin(f.project, QStringLiteral("Audio"));
        QVERIFY(shown(panel).contains(QStringLiteral("[Audio]")));

        if (const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR"); !dir.isEmpty()) {
            QDir().mkpath(dir);
            panel.grab().save(dir + QStringLiteral("/poolfolders-grid.png"));
            panel.setViewMode(MediaPoolPanel::ViewMode::List);
            panel.grab().save(dir + QStringLiteral("/poolfolders-list.png"));
        }
    }

    // Smart bins and power bins never show folders.
    void noFoldersInSmartOrPowerBins()
    {
        QTemporaryDir dir;
        PowerBins power(dir.filePath(QStringLiteral("p.json")));
        PanelFixture f;
        f.ctx.powerBins = &power;
        addBin(f.project, QStringLiteral("Shots"));
        addItem(f.project, QStringLiteral("a.mov"));
        MediaPoolPanel panel(f.ctx);
        panel.setThumbnailProvider({});
        QVERIFY(shown(panel).contains(QStringLiteral("[Shots]")));
        SmartBin smart;
        smart.name = QStringLiteral("All");
        smart.rules = {SmartRule {SmartField::Name, SmartOp::Contains, QStringLiteral("a")}};
        panel.setCurrentBinKey(panel.createSmartBin(smart));
        QVERIFY(!shown(panel).contains(QStringLiteral("[Shots]")));
        QVERIFY(shown(panel).contains(QStringLiteral("a.mov")));
        panel.setCurrentBinKey(QLatin1String(MediaPoolFilterModel::kPowerPrefix));
        QVERIFY(!shown(panel).contains(QStringLiteral("[Shots]")));
    }

    // Double-click on a folder opens its bin: the bin list selects it and the clip area shows
    // its contents. A double-click on a tile's name renames instead.
    void doubleClickOpensBin()
    {
        PanelFixture f;
        const QString shots = addBin(f.project, QStringLiteral("Shots"));
        addItem(f.project, QStringLiteral("a.mov"), shots);
        MediaPoolPanel panel(f.ctx);
        panel.setThumbnailProvider({});
        panel.resize(900, 500);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QListView* g = grid(panel);
        const QRect tile = g->visualRect(folderIndex(g, shots));
        doubleClick(g->viewport(), tile.topLeft() + QPoint(30, 30));
        QCOMPARE(panel.currentBinKey(), shots);
        QCOMPARE(panel.binList()->currentKey(), shots);
        QVERIFY(shown(panel).contains(QStringLiteral("a.mov")));

        // List view too.
        panel.setCurrentBin({});
        panel.setViewMode(MediaPoolPanel::ViewMode::List);
        QTreeView* l = list(panel);
        const QRect rowRect = l->visualRect(folderIndex(l, shots));
        doubleClick(l->viewport(), rowRect.center());
        QCOMPARE(panel.currentBinKey(), shots);

        // Grid: double-click on the name line edits the name.
        panel.setCurrentBin({});
        panel.setViewMode(MediaPoolPanel::ViewMode::Grid);
        const QRect tile2 = g->visualRect(folderIndex(g, shots));
        doubleClick(g->viewport(), MediaPoolDelegate::nameRect(tile2).center());
        QCOMPARE(panel.currentBinKey(), QString()); // not opened
        auto* editor = g->findChild<QLineEdit*>();
        QVERIFY(editor && editor->isVisible());
        editor->setText(QStringLiteral("Takes"));
        QTest::keyClick(editor, Qt::Key_Return);
        QTRY_COMPARE(f.project.mediaPool()->bin(shots)->name, QStringLiteral("Takes"));
    }

    // Dropping clips on a folder moves them into that bin (one undo step), as on the bin list;
    // a dragged folder moves into another; files dropped on a folder are imported there.
    void dropOnFolderMoves()
    {
        PanelFixture f;
        const QString shots = addBin(f.project, QStringLiteral("Shots"));
        const QString other = addBin(f.project, QStringLiteral("Other"));
        const QString a = addItem(f.project, QStringLiteral("a.mov"));
        const QString b = addItem(f.project, QStringLiteral("b.mov"));
        MediaPoolPanel panel(f.ctx);
        panel.setThumbnailProvider({});
        panel.resize(900, 500);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QListView* g = grid(panel);

        // From the view's own drag data (the model's mimeData of the two clips).
        panel.setSelectedMediaIds({a, b});
        QModelIndexList indexes;
        for (const QModelIndex& i : g->selectionModel()->selectedIndexes())
            indexes.append(i);
        std::unique_ptr<QMimeData> data(g->model()->mimeData(indexes));
        QVERIFY(data);
        dropOnFolder(panel, g, shots, data.get());
        QCOMPARE(f.project.mediaPool()->item(a)->binId, shots);
        QCOMPARE(f.project.mediaPool()->item(b)->binId, shots);
        QVERIFY(!shown(panel).contains(QStringLiteral("a.mov")));
        f.project.undoStack()->undo();
        QCOMPARE(f.project.mediaPool()->item(a)->binId, QString());
        QCOMPARE(f.project.mediaPool()->item(b)->binId, QString());

        // In the list view.
        panel.setViewMode(MediaPoolPanel::ViewMode::List);
        QMimeData one;
        one.setData(QLatin1String(mime::MediaItems), a.toUtf8());
        dropOnFolder(panel, list(panel), other, &one);
        QCOMPARE(f.project.mediaPool()->item(a)->binId, other);

        // A folder dragged onto another folder: the bin moves inside it.
        const QModelIndex otherIndex = folderIndex(list(panel), other);
        std::unique_ptr<QMimeData> binDrag(list(panel)->model()->mimeData({otherIndex}));
        QVERIFY(binDrag && binDrag->hasFormat(QLatin1String(BinList::kBinMime)));
        dropOnFolder(panel, list(panel), shots, binDrag.get());
        QCOMPARE(f.project.mediaPool()->bin(other)->parentId, shots);

        // Files from the file manager dropped on a folder go into that bin, whatever the
        // import preference.
        if (!QFileInfo::exists(stillPath()))
            QSKIP("generated test media missing");
        prefs::setImportIntoMasterBin(true);
        QMimeData files;
        files.setUrls({QUrl::fromLocalFile(stillPath())});
        dropOnFolder(panel, list(panel), shots, &files);
        QTRY_VERIFY(f.project.mediaPool()->itemByPath(stillPath()));
        QCOMPARE(f.project.mediaPool()->itemByPath(stillPath())->binId, shots);
    }

    // F2 renames a folder, Delete deletes the bin, both undoable like in the bin list.
    void renameAndDeleteWithUndo()
    {
        PanelFixture f;
        const QString shots = addBin(f.project, QStringLiteral("Shots"));
        MediaPoolPanel panel(f.ctx);
        panel.setThumbnailProvider({});
        panel.resize(900, 500);
        panel.setViewMode(MediaPoolPanel::ViewMode::List);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QTreeView* l = list(panel);
        const QModelIndex index = folderIndex(l, shots);
        l->setFocus();
        l->selectionModel()->setCurrentIndex(
            index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QCOMPARE(panel.selectedBinIds(), QStringList {shots});
        QVERIFY(panel.selectedMediaIds().isEmpty());
        QTest::keyClick(l, Qt::Key_F2);
        auto* editor = l->findChild<QLineEdit*>();
        QVERIFY(editor && editor->isVisible());
        editor->selectAll();
        QTest::keyClicks(editor, QStringLiteral("Cutaways"));
        QTest::keyClick(editor, Qt::Key_Return); // the delegate commits from the event loop
        QTRY_COMPARE(f.project.mediaPool()->bin(shots)->name, QStringLiteral("Cutaways"));
        QCOMPARE(panel.binList()->itemForKey(shots)->text(0), QStringLiteral("Cutaways"));
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Rename Bin"));
        f.project.undoStack()->undo();
        QCOMPARE(f.project.mediaPool()->bin(shots)->name, QStringLiteral("Shots"));
        QVERIFY(shown(panel).contains(QStringLiteral("[Shots]")));

        // Delete (an empty bin: no question).
        l->setFocus();
        l->selectionModel()->setCurrentIndex(
            folderIndex(l, shots), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QTest::keyClick(l, Qt::Key_Delete);
        QVERIFY(!f.project.mediaPool()->bin(shots));
        QVERIFY(!shown(panel).contains(QStringLiteral("[Shots]")));
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Delete Bin"));
        f.project.undoStack()->undo();
        QVERIFY(f.project.mediaPool()->bin(shots));
        QVERIFY(shown(panel).contains(QStringLiteral("[Shots]")));

        // A bin with clips asks first (same rule as the bin list): No keeps it.
        addItem(f.project, QStringLiteral("a.mov"), shots);
        QTimer::singleShot(0, [] {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (w->inherits("QMessageBox") && w->isVisible())
                    QTest::keyClick(w, Qt::Key_Escape);
            }
        });
        l->setFocus();
        l->selectionModel()->setCurrentIndex(
            folderIndex(l, shots), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QTest::keyClick(l, Qt::Key_Delete);
        QVERIFY(f.project.mediaPool()->bin(shots));
    }

    // The folder context menu has the bin commands; Open shows the bin.
    void folderContextMenu()
    {
        PanelFixture f;
        const QString shots = addBin(f.project, QStringLiteral("Shots"));
        MediaPoolPanel panel(f.ctx);
        panel.setThumbnailProvider({});
        panel.resize(900, 500);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QListView* g = grid(panel);
        QStringList texts;
        QTimer::singleShot(0, [&] {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                auto* menu = qobject_cast<QMenu*>(w);
                if (!menu || menu->objectName() != QLatin1String("FolderMenu"))
                    continue;
                for (QAction* a : menu->actions()) {
                    texts.append(a->text());
                    if (a->text() == QLatin1String("Open"))
                        a->trigger();
                }
                menu->close();
            }
        });
        const QPoint pos = g->visualRect(folderIndex(g, shots)).center();
        emit g->customContextMenuRequested(pos);
        for (const char* t : {"Open", "Rename Bin", "Delete Bin", "New Bin"})
            QVERIFY2(texts.contains(QLatin1String(t)), t);
        QCOMPARE(panel.currentBinKey(), shots);
    }

    // With a search, folders whose names do not match are hidden.
    void searchHidesFolders()
    {
        PanelFixture f;
        addBin(f.project, QStringLiteral("Shots"));
        addBin(f.project, QStringLiteral("Music"));
        addItem(f.project, QStringLiteral("shot1.mov"));
        MediaPoolPanel panel(f.ctx);
        panel.setThumbnailProvider({});
        panel.setSearchText(QStringLiteral("shot"));
        QStringList names = shown(panel);
        QVERIFY(names.contains(QStringLiteral("[Shots]")));
        QVERIFY(!names.contains(QStringLiteral("[Music]")));
        QVERIFY(names.contains(QStringLiteral("shot1.mov")));
        panel.setSearchText(QStringLiteral("zzz"));
        QVERIFY(shown(panel).isEmpty());
        panel.setSearchText({});
        names = shown(panel);
        QVERIFY(
            names.contains(QStringLiteral("[Shots]")) && names.contains(QStringLiteral("[Music]")));
        // A clip colour filter shows clips only.
        panel.setColorFilter(QStringLiteral("none"));
        QVERIFY(!shown(panel).contains(QStringLiteral("[Shots]")));
    }

    // Sort by Type: folders first, then timelines, video, audio, stills, compound clips;
    // Descending reverses the kinds but keeps the folders first. Every sort keeps folders
    // first; the Type column sorts on a header click too.
    void sortByType()
    {
        PanelFixture f;
        addBin(f.project, QStringLiteral("B bin"));
        addBin(f.project, QStringLiteral("A bin"));
        addItem(f.project, QStringLiteral("zeta.png"), {}, Kind::Still);
        addItem(f.project, QStringLiteral("beta.wav"), {}, Kind::Audio);
        addItem(f.project, QStringLiteral("alpha.mov"), {}, Kind::Video);
        addItem(f.project, QStringLiteral("aaa.mov"), {}, Kind::Video);
        f.project.createTimeline(QStringLiteral("Timeline X"));
        MediaPoolPanel panel(f.ctx);
        panel.setThumbnailProvider({});
        auto mine = [&] {
            QStringList out;
            for (const QString& n : shown(panel)) {
                if (n.startsWith(QLatin1Char('[')) || n.contains(QLatin1Char('.'))
                    || n == QLatin1String("Timeline X"))
                    out.append(n);
            }
            return out;
        };
        panel.setSort(MediaPoolModel::TypeColumn, Qt::AscendingOrder);
        QCOMPARE(mine(),
            (QStringList {QStringLiteral("[A bin]"), QStringLiteral("[B bin]"),
                QStringLiteral("Timeline X"), QStringLiteral("aaa.mov"),
                QStringLiteral("alpha.mov"), QStringLiteral("beta.wav"),
                QStringLiteral("zeta.png")}));
        panel.setSort(MediaPoolModel::TypeColumn, Qt::DescendingOrder);
        QCOMPARE(mine(),
            (QStringList {QStringLiteral("[B bin]"), QStringLiteral("[A bin]"),
                QStringLiteral("zeta.png"), QStringLiteral("beta.wav"), QStringLiteral("alpha.mov"),
                QStringLiteral("aaa.mov"), QStringLiteral("Timeline X")}));
        // Clip Name descending: folders still first.
        panel.setSort(MediaPoolModel::NameColumn, Qt::DescendingOrder);
        QCOMPARE(
            mine().mid(0, 2), (QStringList {QStringLiteral("[B bin]"), QStringLiteral("[A bin]")}));
        QCOMPARE(mine().value(2), QStringLiteral("zeta.png"));
        // Import order: folders first too.
        panel.setSort(-1, Qt::AscendingOrder);
        QVERIFY(shown(panel).value(0).startsWith(QLatin1Char('[')));
        QVERIFY(shown(panel).value(1).startsWith(QLatin1Char('[')));
        // Header click on the Type column.
        panel.setViewMode(MediaPoolPanel::ViewMode::List);
        panel.setColumnVisible(MediaPoolModel::TypeColumn, true);
        list(panel)->header()->setSortIndicator(MediaPoolModel::TypeColumn, Qt::AscendingOrder);
        QCOMPARE(mine().value(2), QStringLiteral("Timeline X"));
        // The sort menu offers Type.
        auto* sortButton = panel.findChild<QToolButton*>(QStringLiteral("MediaPoolSort"));
        QVERIFY(sortButton && sortButton->menu());
        bool hasType = false;
        for (QAction* a : sortButton->menu()->actions())
            hasType = hasType || a->text() == QLatin1String("Type");
        QVERIFY(hasType);
    }

    // Preferences > Editing > "Import new media into": the panel's own imports (drops on the
    // clip area, the Import button) go into the shown bin or Master; importTargetBin() answers
    // for the other paths from the shown panel.
    void importTarget()
    {
        PanelFixture f;
        const QString shots = addBin(f.project, QStringLiteral("Shots"));
        MediaPoolPanel panel(f.ctx);
        panel.setThumbnailProvider({});
        panel.setCurrentBin(shots);
        QCOMPARE(panel.importTargetKey(), shots); // default: the current bin
        QCOMPARE(importTargetBin(f.ctx), shots);
        prefs::setImportIntoMasterBin(true);
        QCOMPARE(panel.importTargetKey(), QString());
        QCOMPARE(importTargetBin(f.ctx), QString());
        prefs::setImportIntoMasterBin(false);
        QCOMPARE(importTargetBin(f.ctx), shots);

        if (!QFileInfo::exists(stillPath()))
            QSKIP("generated test media missing");
        // A drop on the clip area (not on a folder) follows the preference.
        prefs::setImportIntoMasterBin(true);
        panel.resize(900, 500);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QMimeData files;
        files.setUrls({QUrl::fromLocalFile(stillPath())});
        const QPoint empty = grid(panel)->viewport()->mapTo(&panel, QPoint(600, 300));
        QMimeData clip; // a clip from the pool is only taken by a folder
        clip.setData(QLatin1String(mime::MediaItems), QByteArrayLiteral("x"));
        QVERIFY(!dropAt(panel, empty, &clip));
        QVERIFY(dropAt(panel, empty, &files));
        QTRY_VERIFY(f.project.mediaPool()->itemByPath(stillPath()));
        QCOMPARE(f.project.mediaPool()->itemByPath(stillPath())->binId, QString());
        f.project.undoStack()->undo();
        QVERIFY(!f.project.mediaPool()->itemByPath(stillPath()));
        prefs::setImportIntoMasterBin(false);
        QVERIFY(dropAt(panel, empty, &files));
        QTRY_VERIFY(f.project.mediaPool()->itemByPath(stillPath()));
        QCOMPARE(f.project.mediaPool()->itemByPath(stillPath())->binId, shots);
    }

    // KK_SCREENSHOT_DIR set: writes poolfolders-grid.kkproj and poolfolders-list.kkproj
    // (Master with sub-bins, clips, a timeline; thumbnails / list sorted by Type) for
    // `kinoklip --screenshot mediapool`.
    void writeScreenshotProjects()
    {
        const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR");
        if (dir.isEmpty() || !QFileInfo::exists(stillPath()))
            QSKIP("KK_SCREENSHOT_DIR not set");
        QDir().mkpath(dir);
        for (const char* mode : {"grid", "list"}) {
            Project project;
            const QString footage = addBin(project, QStringLiteral("Footage"));
            addBin(project, QStringLiteral("Music"));
            addBin(project, QStringLiteral("Graphics"));
            addBin(project, QStringLiteral("Day 2"), footage);
            const QString media = QStringLiteral(KK_TEST_DATA_DIR "/generated/");
            project.importMedia({media + QStringLiteral("h264_25fps.mp4"),
                media + QStringLiteral("still.jpg"), media + QStringLiteral("sine.mp3"),
                media + QStringLiteral("rotated_portrait.mp4")});
            project.importMedia({media + QStringLiteral("hevc10.mp4")}, footage);
            BinViewState state;
            state.mode = QLatin1String(mode);
            if (state.mode == QLatin1String("list"))
                state.sortKey = QStringLiteral("type");
            project.setBinViewState({}, state);
            QString error;
            QVERIFY2(project.save(dir + QStringLiteral("/poolfolders-") + QLatin1String(mode)
                             + QStringLiteral(".kkproj"),
                         &error),
                qPrintable(error));
        }
    }
};

QTEST_MAIN(TestPoolFolders)
#include "test_poolfolders.moc"
