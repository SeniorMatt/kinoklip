// SPDX-License-Identifier: GPL-3.0-or-later
// Edit Index panel (spec 18.8, 18.9): event and marker rows, jump and select on click, search
// and colour filters, in-place marker editing as one undo step, live updates.
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/MediaPool.h"
#include "core/Timeline.h"
#include "ui/common/Resources.h"
#include "ui/editindex/EditIndexModel.h"
#include "ui/editindex/EditIndexPanel.h"
#include "ui/editindex/EditIndexRows.h"
#include "ui/timeline/TimelineController.h"

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QStandardPaths>
#include <QTableView>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;

namespace {

const TrackRef kV1 {TrackType::Video, 0};
const TrackRef kV2 {TrackType::Video, 1};
const TrackRef kA1 {TrackType::Audio, 0};
const QColor kRed(0xe0, 0x4a, 0x4a); // "Red" of the marker palette
const QColor kBlue(0x4c, 0x9b, 0xe8); // "Blue"

/// Text of proxy cell (@p row, @p column) of the panel's table.
QString cell(const EditIndexPanel& panel, int row, int column)
{
    return panel.table()->model()->index(row, column).data().toString();
}

/// Clicks proxy row @p row of the table (column 1, so not on the number).
void clickRow(EditIndexPanel& panel, int row)
{
    QTableView* t = panel.table();
    const QRect r = t->visualRect(t->model()->index(row, 1));
    QTest::mouseClick(t->viewport(), Qt::LeftButton, {}, r.center());
}

} // namespace

class TestEditIndex : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        initResources();
        theme::apply(*qApp);
    }

    // Events in record order, video before audio on the same frame; EDL-style columns.
    void eventRowsFollowTheTimeline()
    {
        PanelFixture f;
        MediaItem item;
        item.id = QStringLiteral("m1");
        item.path = QStringLiteral("/footage/A001_C002.mov");
        item.name = QStringLiteral("A001_C002.mov");
        item.info.hasVideo = true;
        item.info.frameRate = {25, 1};
        item.info.durationUs = 100'000'000;
        item.info.startTimecode = QStringLiteral("01:00:00:00");
        f.project.mediaPool()->addItem(item);
        const quint64 late = f.addClip(kV1, 50, 25);
        const quint64 audio = f.addClip(kA1, 0, 50);
        const quint64 video = f.addClip(kV2, 0, 50);
        {
            f.project.beginEdit(QStringLiteral("Set Source"));
            ClipData* c = editops::clipById(f.project.timeline()->mutableData(), late);
            c->sourceInUs = 2'000'000; // 2 s into the media
            c->name = QStringLiteral("Second shot");
            f.project.endEdit();
        }
        const auto rows = editindex::events(f.project.timeline()->data(), f.project.mediaPool());
        QCOMPARE(rows.size(), size_t(3));
        QCOMPARE(rows[0].clipId, video);
        QCOMPARE(rows[1].clipId, audio);
        QCOMPARE(rows[2].clipId, late);
        QCOMPARE(rows[2].number, 3);
        QCOMPARE(rows[2].reel, QStringLiteral("A001_C002"));
        QCOMPARE(rows[2].trackName, QStringLiteral("V1"));
        QCOMPARE(rows[2].sourceIn, QStringLiteral("01:00:02:00"));
        QCOMPARE(rows[2].sourceOut, QStringLiteral("01:00:03:00"));

        EditIndexPanel panel(f.ctx);
        panel.resize(900, 300);
        QCOMPARE(panel.table()->model()->rowCount(), 3);
        QCOMPARE(cell(panel, 2, EditIndexModel::EditNumber), QStringLiteral("003"));
        QCOMPARE(cell(panel, 2, EditIndexModel::EditName), QStringLiteral("Second shot"));
        QCOMPARE(cell(panel, 2, EditIndexModel::EditRecordIn), QStringLiteral("00:00:02:00"));
        QCOMPARE(cell(panel, 2, EditIndexModel::EditRecordOut), QStringLiteral("00:00:03:00"));
        QCOMPARE(cell(panel, 2, EditIndexModel::EditDuration), QStringLiteral("00:00:01:00"));
        QCOMPARE(cell(panel, 1, EditIndexModel::EditTrack), QStringLiteral("A1"));
        saveScreenshot(panel, QStringLiteral("editindex-edits"));
    }

    // A click moves the playhead to the record in and selects the clip; the timeline
    // selection is mirrored back into the table.
    void clickJumpsAndSelects()
    {
        PanelFixture f;
        f.addClip(kV1, 0, 30);
        const quint64 second = f.addClip(kV1, 30, 40);
        EditIndexPanel panel(f.ctx);
        panel.resize(900, 300);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        clickRow(panel, 1);
        QCOMPARE(f.project.timeline()->playhead(), int64_t(30));
        QCOMPARE(f.selection.currentClip(), second);
        // Selecting the first clip elsewhere makes its row current, without moving the playhead.
        f.selection.selectClip(f.project.timeline()->data().videoTracks[0].clips[0].id);
        QCOMPARE(panel.table()->currentIndex().row(), 0);
        QCOMPARE(f.project.timeline()->playhead(), int64_t(30));
    }

    // Timeline and clip markers in one list; colour and text filters; clicking a clip marker
    // selects its clip.
    void markersModeListsAndFilters()
    {
        PanelFixture f;
        const quint64 clip = f.addClip(kV1, 100, 50, ClipKind::Media, QStringLiteral("Interview"));
        f.project.beginEdit(QStringLiteral("Add Markers"));
        TimelineData& d = f.project.timeline()->mutableData();
        d.markers.push_back({10, QStringLiteral("Intro"), kBlue, QStringLiteral("music starts")});
        d.markers.push_back({200, QStringLiteral("Outro"), kRed, {}});
        editops::clipById(d, clip)->markers.push_back(
            {20, QStringLiteral("Good take"), kRed, QStringLiteral("use this")});
        f.project.endEdit();

        EditIndexPanel panel(f.ctx);
        panel.resize(900, 300);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        panel.setMode(EditIndexModel::Mode::Markers);
        QVERIFY(panel.colorFilter()->isVisible());
        QCOMPARE(panel.table()->model()->rowCount(), 3);
        QCOMPARE(cell(panel, 0, EditIndexModel::MarkerName), QStringLiteral("Intro"));
        QCOMPARE(cell(panel, 1, EditIndexModel::MarkerName), QStringLiteral("Good take"));
        QCOMPARE(cell(panel, 1, EditIndexModel::MarkerTimecode), QStringLiteral("00:00:04:20"));
        QCOMPARE(cell(panel, 1, EditIndexModel::MarkerDuration), QStringLiteral("00:00:00:01"));
        QCOMPARE(cell(panel, 1, EditIndexModel::MarkerLocation), QStringLiteral("Interview"));
        QCOMPARE(cell(panel, 2, EditIndexModel::MarkerNotes), QString());
        saveScreenshot(panel, QStringLiteral("editindex-markers"));

        clickRow(panel, 1);
        QCOMPARE(f.project.timeline()->playhead(), int64_t(120));
        QCOMPARE(f.selection.currentClip(), clip);

        panel.colorFilter()->setCurrentIndex(panel.colorFilter()->findData(kRed));
        QCOMPARE(panel.table()->model()->rowCount(), 2);
        panel.searchField()->setText(QStringLiteral("TAKE"));
        QCOMPARE(panel.table()->model()->rowCount(), 1);
        QCOMPARE(cell(panel, 0, EditIndexModel::MarkerName), QStringLiteral("Good take"));
        panel.colorFilter()->setCurrentIndex(0);
        panel.searchField()->setText(QStringLiteral("music"));
        QCOMPARE(panel.table()->model()->rowCount(), 1);
        QCOMPARE(cell(panel, 0, EditIndexModel::MarkerName), QStringLiteral("Intro"));
    }

    // Editing a marker's name or notes in the table is one named undo step.
    void markerEditIsOneUndoStep()
    {
        PanelFixture f;
        const quint64 clip = f.addClip(kV1, 0, 50);
        f.project.beginEdit(QStringLiteral("Add Markers"));
        TimelineData& d = f.project.timeline()->mutableData();
        d.markers.push_back({5, QStringLiteral("Old"), kBlue, {}});
        editops::clipById(d, clip)->markers.push_back({7, QStringLiteral("Clip"), kRed, {}});
        f.project.endEdit();
        EditIndexPanel panel(f.ctx);
        panel.show(); // a hidden panel does not rebuild
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        panel.setMode(EditIndexModel::Mode::Markers);
        QAbstractItemModel* m = panel.table()->model();
        QUndoStack* undo = f.project.undoStack();
        const int before = undo->count();

        QVERIFY(m->flags(m->index(0, EditIndexModel::MarkerName)) & Qt::ItemIsEditable);
        QVERIFY(!(m->flags(m->index(0, EditIndexModel::MarkerTimecode)) & Qt::ItemIsEditable));
        QVERIFY(m->setData(m->index(0, EditIndexModel::MarkerName), QStringLiteral("New")));
        QCOMPARE(undo->count(), before + 1);
        QCOMPARE(undo->undoText(), QStringLiteral("Rename Marker"));
        QCOMPARE(f.project.timeline()->data().markers[0].name, QStringLiteral("New"));
        QCoreApplication::processEvents(); // the coalesced rebuild
        QCOMPARE(cell(panel, 0, EditIndexModel::MarkerName), QStringLiteral("New"));

        QVERIFY(m->setData(m->index(1, EditIndexModel::MarkerNotes), QStringLiteral("a note")));
        QCOMPARE(undo->count(), before + 2);
        QCOMPARE(undo->undoText(), QStringLiteral("Modify Marker"));
        QCOMPARE(f.clip(clip)->markers[0].note, QStringLiteral("a note"));
        // Unchanged text is no step.
        QCoreApplication::processEvents();
        QVERIFY(!m->setData(m->index(1, EditIndexModel::MarkerNotes), QStringLiteral("a note")));
        QCOMPARE(undo->count(), before + 2);

        undo->undo();
        undo->undo();
        QCoreApplication::processEvents();
        QCOMPARE(cell(panel, 0, EditIndexModel::MarkerName), QStringLiteral("Old"));
        QCOMPARE(f.clip(clip)->markers[0].note, QString());

        // A locked track makes its clip markers read-only.
        f.project.beginEdit(QStringLiteral("Lock"));
        f.project.timeline()->mutableData().videoTracks[0].locked = true;
        f.project.endEdit();
        panel.rebuildNow();
        QVERIFY(!(m->flags(m->index(1, EditIndexModel::MarkerName)) & Qt::ItemIsEditable));
    }

    // Round 3 audit A1: Delete on a marker row deletes the marker (one undo step), not the
    // clip the row selected through the window's Delete commands.
    void deleteKeyDeletesTheMarker()
    {
        PanelFixture f;
        const quint64 clip = f.addClip(kV1, 0, 50);
        f.project.beginEdit(QStringLiteral("Add Markers"));
        editops::clipById(f.project.timeline()->mutableData(), clip)
            ->markers.push_back({7, QStringLiteral("Clip"), kRed, {}});
        f.project.endEdit();
        TimelineController controller(f.ctx); // Delete would ripple delete the clip
        controller.connectActions();
        QWidget window; // the window's shortcuts, as in the main window
        window.addActions(f.actions.actions());
        EditIndexPanel panel(f.ctx);
        panel.setParent(&window);
        window.resize(900, 300);
        panel.resize(900, 300);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        panel.setMode(EditIndexModel::Mode::Markers);
        clickRow(panel, 0);
        QCOMPARE(f.selection.currentClip(), clip);
        panel.table()->setFocus();
        const int before = f.project.undoStack()->count();
        QTest::keyClick(panel.table(), Qt::Key_Delete);
        QVERIFY(f.clip(clip)); // still there
        QVERIFY(f.clip(clip)->markers.isEmpty());
        QCOMPARE(f.project.undoStack()->count(), before + 1);
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Delete Marker"));
    }

    // Edits, undo and hidden panels: the table follows the timeline.
    void updatesLive()
    {
        PanelFixture f;
        EditIndexPanel panel(f.ctx);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QCOMPARE(panel.table()->model()->rowCount(), 0);
        f.addClip(kV1, 0, 30);
        QTRY_COMPARE(panel.table()->model()->rowCount(), 1);
        f.project.undoStack()->undo();
        QTRY_COMPARE(panel.table()->model()->rowCount(), 0);
        panel.hide();
        f.addClip(kV1, 0, 30);
        f.addClip(kV1, 30, 30);
        QCoreApplication::processEvents();
        QCOMPARE(panel.model()->rowCount(), 0); // no work while hidden
        panel.show();
        QCOMPARE(panel.model()->rowCount(), 2);
    }
};

QTEST_MAIN(TestEditIndex)
#include "test_editindex.moc"
