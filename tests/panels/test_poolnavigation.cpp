// SPDX-License-Identifier: GPL-3.0-or-later
// Getting around the media pool's clip area: the mouse's Back / Forward buttons go through the
// bins opened before (skipping deleted ones), the thumbnail size follows Ctrl+wheel and the
// toolbar slider (remembered), and one wheel notch scrolls less than a row of thumbnails.
// Writes screenshots of the smallest and largest sizes when KK_SCREENSHOT_DIR is set.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/MediaPool.h"
#include "ui/common/Resources.h"
#include "ui/mediapool/BinList.h"
#include "ui/mediapool/MediaPoolPanel.h"

#include <QApplication>
#include <QDir>
#include <QListView>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QTest>
#include <QTreeView>
#include <QUndoStack>
#include <QWheelEvent>

using namespace kk;
using namespace kk::test;

namespace {

QString addItem(Project& project, const QString& name, const QString& binId = {})
{
    MediaItem item;
    item.path = QCoreApplication::applicationFilePath(); // an existing file
    item.name = name;
    item.binId = binId;
    item.info.valid = true;
    item.info.hasVideo = true;
    item.info.width = 1920;
    item.info.height = 1080;
    item.info.frameRate = Rational {25, 1};
    item.info.durationUs = 10'000'000;
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

/// A press and release of mouse button @p button (Qt::BackButton / Qt::ForwardButton) at the
/// centre of @p w, as a mouse with side buttons sends them.
void clickButton(QWidget* w, Qt::MouseButton button)
{
    const QPoint at = w->rect().center();
    QMouseEvent press(
        QEvent::MouseButtonPress, at, w->mapToGlobal(at), button, button, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &press);
    QMouseEvent release(
        QEvent::MouseButtonRelease, at, w->mapToGlobal(at), button, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &release);
}

/// A wheel event at the centre of @p w: @p angle in eighths of a degree (120 = one notch,
/// negative = towards the user, scrolls down) or @p pixels from a touchpad.
void wheel(QWidget* w, int angle, Qt::KeyboardModifiers mods = Qt::NoModifier, int pixels = 0)
{
    const QPointF at = QRectF(w->rect()).center();
    QWheelEvent e(at, w->mapToGlobal(at), QPoint(0, pixels), QPoint(0, angle), Qt::NoButton, mods,
        Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(w, &e);
}

/// Double-click on the folder of @p binId in the grid (press, release, double-click, release).
void openFolder(MediaPoolPanel& panel, const QString& binId)
{
    QListView* g = grid(panel);
    const QAbstractItemModel* m = g->model();
    for (int r = 0; r < m->rowCount(); ++r) {
        const QModelIndex i = m->index(r, 0);
        if (i.data(MediaPoolModel::FolderBinRole).toString() != binId)
            continue;
        const QPoint at = g->visualRect(i).topLeft() + QPoint(30, 30);
        for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease,
                 QEvent::MouseButtonDblClick, QEvent::MouseButtonRelease}) {
            QMouseEvent e(type, at, g->viewport()->mapToGlobal(at), Qt::LeftButton,
                type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(g->viewport(), &e);
        }
        return;
    }
    QFAIL("no folder");
}

} // namespace

class TestPoolNavigation : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_poolnavigation"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void init() { QSettings().clear(); }

    // Back / Forward mouse buttons over the clip area and the bin list go through the bins
    // opened (folder double-click, bin list click, setCurrentBinKey), like a file manager;
    // opening another bin after going back drops the forward entries.
    void backAndForwardButtons()
    {
        PanelFixture f;
        const QString shots = addBin(f.project, QStringLiteral("Shots"));
        const QString takes = addBin(f.project, QStringLiteral("Takes"), shots);
        const QString audio = addBin(f.project, QStringLiteral("Audio"));
        addItem(f.project, QStringLiteral("a.mov"), takes);
        MediaPoolPanel panel(f.ctx);
        panel.setThumbnailProvider({});
        panel.resize(900, 500);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QVERIFY(!panel.canGoBack());
        QVERIFY(!panel.canGoForward());

        openFolder(panel, shots);
        QCOMPARE(panel.currentBinKey(), shots);
        openFolder(panel, takes);
        QCOMPARE(panel.currentBinKey(), takes);

        QWidget* clips = grid(panel)->viewport();
        QWidget* bins = panel.binList()->viewport();
        clickButton(clips, Qt::BackButton);
        QCOMPARE(panel.currentBinKey(), shots);
        QCOMPARE(panel.binList()->currentKey(), shots); // the bin list follows
        clickButton(bins, Qt::BackButton);
        QCOMPARE(panel.currentBinKey(), QString()); // Master
        QVERIFY(!panel.canGoBack());
        clickButton(clips, Qt::BackButton); // nothing before Master: stays
        QCOMPARE(panel.currentBinKey(), QString());

        clickButton(clips, Qt::ForwardButton);
        QCOMPARE(panel.currentBinKey(), shots);
        clickButton(bins, Qt::ForwardButton);
        QCOMPARE(panel.currentBinKey(), takes);
        QVERIFY(!panel.canGoForward());

        // A click in the bin list opens a bin and is recorded; it drops the forward entries.
        clickButton(clips, Qt::BackButton);
        QCOMPARE(panel.currentBinKey(), shots);
        const QRect audioRow = panel.binList()->visualItemRect(panel.binList()->itemForKey(audio));
        QTest::mouseClick(bins, Qt::LeftButton, Qt::NoModifier, audioRow.center());
        QCOMPARE(panel.currentBinKey(), audio);
        QVERIFY(!panel.canGoForward());
        clickButton(clips, Qt::BackButton);
        QCOMPARE(panel.currentBinKey(), shots);
        clickButton(clips, Qt::BackButton);
        QCOMPARE(panel.currentBinKey(), QString());

        // The list view too.
        panel.setViewMode(MediaPoolPanel::ViewMode::List);
        auto* list = panel.findChild<QTreeView*>(QStringLiteral("MediaPoolList"));
        clickButton(list->viewport(), Qt::ForwardButton);
        QCOMPARE(panel.currentBinKey(), shots);
        // The selection is left alone by the side buttons.
        QVERIFY(panel.selectedBinIds().isEmpty());
    }

    // Bins deleted since are skipped; deleting the shown bin falls back to Master without
    // a history entry of its own.
    void historySkipsDeletedBins()
    {
        PanelFixture f;
        const QString a = addBin(f.project, QStringLiteral("A"));
        const QString b = addBin(f.project, QStringLiteral("B"));
        const QString c = addBin(f.project, QStringLiteral("C"));
        MediaPoolPanel panel(f.ctx);
        panel.setThumbnailProvider({});
        panel.setCurrentBinKey(a);
        panel.setCurrentBinKey(b);
        panel.setCurrentBinKey(c);

        QVERIFY(panel.deleteBin(b));
        QVERIFY(panel.goBack());
        QCOMPARE(panel.currentBinKey(), a); // B skipped
        QVERIFY(panel.goForward());
        QCOMPARE(panel.currentBinKey(), c);

        QVERIFY(panel.deleteBin(c)); // shown: back to Master
        QCOMPARE(panel.currentBinKey(), QString());
        QVERIFY(!panel.canGoForward());
        QVERIFY(panel.goBack());
        QCOMPARE(panel.currentBinKey(), a);
        QVERIFY(panel.goBack());
        QCOMPARE(panel.currentBinKey(), QString());
        QVERIFY(!panel.canGoBack());

        // Undo brings B back: it is in the history again.
        f.project.undoStack()->undo(); // C
        f.project.undoStack()->undo(); // B
        QVERIFY(panel.goForward());
        QCOMPARE(panel.currentBinKey(), a);
        QVERIFY(panel.goForward());
        QCOMPARE(panel.currentBinKey(), b);
    }

    // Ctrl+wheel over the grid changes the thumbnail size one step per notch, within
    // kThumbnailWidths; the slider follows (and sets it); the size is remembered.
    void ctrlWheelZoomsThumbnails()
    {
        PanelFixture f;
        for (int i = 0; i < 30; ++i)
            addItem(f.project, QStringLiteral("clip %1.mov").arg(i));
        MediaPoolPanel panel(f.ctx);
        panel.setThumbnailProvider({});
        panel.resize(900, 500);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QListView* g = grid(panel);
        auto* slider = panel.findChild<QSlider*>(QStringLiteral("MediaPoolThumbnailSize"));
        QVERIFY(slider && slider->isVisible());
        QCOMPARE(panel.thumbnailWidth(), MediaPoolModel::kPosterSize.width());
        const int defaultRow = g->gridSize().height();

        wheel(g->viewport(), -120, Qt::ControlModifier);
        QCOMPARE(panel.thumbnailWidth(), 128);
        QVERIFY(g->gridSize().height() < defaultRow);
        QCOMPARE(slider->value(), 4);
        QCOMPARE(QSettings().value(QStringLiteral("mediaPool/thumbnailWidth")).toInt(), 128);
        QCOMPARE(panel.model()->posterSize(), QSize(128, 72));

        // Half notches (high-resolution wheels, touchpads) add up.
        wheel(g->viewport(), -60, Qt::ControlModifier);
        QCOMPARE(panel.thumbnailWidth(), 128);
        wheel(g->viewport(), -60, Qt::ControlModifier);
        QCOMPARE(panel.thumbnailWidth(), 112);

        for (int i = 0; i < 20; ++i)
            wheel(g->viewport(), -120, Qt::ControlModifier);
        QCOMPARE(panel.thumbnailWidth(), MediaPoolPanel::kThumbnailWidths.front()); // 64
        QCOMPARE(slider->value(), 0);
        if (const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR"); !dir.isEmpty()) {
            QDir().mkpath(dir);
            panel.grab().save(dir + QStringLiteral("/poolnav-smallest.png"));
        }
        for (int i = 0; i < 20; ++i)
            wheel(g->viewport(), 120, Qt::ControlModifier);
        QCOMPARE(panel.thumbnailWidth(), MediaPoolPanel::kThumbnailWidths.back()); // 240
        if (const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR"); !dir.isEmpty())
            panel.grab().save(dir + QStringLiteral("/poolnav-largest.png"));

        slider->setValue(1);
        QCOMPARE(panel.thumbnailWidth(), 80);
        QCOMPARE(g->gridSize().width(), 80 + 12 + 4); // poster + padding + spacing

        // A new panel (the other page, the next start) shows the remembered size.
        MediaPoolPanel other(f.ctx);
        QCOMPARE(other.thumbnailWidth(), 80);
        panel.setThumbnailWidth(100); // snaps to the nearest step
        QCOMPARE(panel.thumbnailWidth(), 96);
        other.show(); // picks up the other panel's change when shown
        QCOMPARE(other.thumbnailWidth(), 96);

        // The slider belongs to the grid.
        panel.setViewMode(MediaPoolPanel::ViewMode::List);
        QVERIFY(!slider->isVisible());
    }

    // One wheel notch scrolls the grid by a third of a row (less than one row); a touchpad's
    // pixel delta scrolls by exactly those pixels.
    void wheelScrollsLessThanARow()
    {
        PanelFixture f;
        for (int i = 0; i < 200; ++i)
            addItem(f.project, QStringLiteral("clip %1.mov").arg(i));
        MediaPoolPanel panel(f.ctx);
        panel.setThumbnailProvider({});
        panel.resize(700, 400);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QListView* g = grid(panel);
        QScrollBar* bar = g->verticalScrollBar();
        QTRY_VERIFY(bar->maximum() > 2 * g->gridSize().height());
        const int row = g->gridSize().height();

        wheel(g->viewport(), -120);
        QVERIFY(bar->value() > 0);
        QVERIFY2(bar->value() < row,
            qPrintable(QStringLiteral("%1 px, row %2").arg(bar->value()).arg(row)));
        QCOMPARE(bar->value(), row / 3);
        wheel(g->viewport(), 120);
        QCOMPARE(bar->value(), 0);

        wheel(g->viewport(), -120, Qt::NoModifier, -10); // touchpad: pixels win
        QCOMPARE(bar->value(), 10);

        // Smaller thumbnails scroll by a smaller step.
        panel.setThumbnailWidth(64);
        bar->setValue(0);
        wheel(g->viewport(), -120);
        QCOMPARE(bar->value(), g->gridSize().height() / 3);
    }
};

QTEST_MAIN(TestPoolNavigation)
#include "test_poolnavigation.moc"
