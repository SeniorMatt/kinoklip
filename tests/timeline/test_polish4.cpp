// SPDX-License-Identifier: GPL-3.0-or-later
// Fourth polish round of the Edit page (docs/polish-v3.md, Round 4): ranged markers (spec
// 18.11), moving markers and clip marker editing (18.14, 18.15), the Edit Index column chooser
// (18.10) and the History window (17.6).
#include "TimelineTestUtils.h"
#include "core/Preferences.h"
#include "core/Serialization.h"
#include "ui/dialogs/HistoryWindow.h"
#include "ui/editindex/EditIndexModel.h"
#include "ui/editindex/EditIndexPanel.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <QUndoView>

using namespace kk;
using namespace kk::test;
using namespace kk::timelinemetrics;
namespace ops = kk::editops;

namespace {

struct View
{
    TimelineFixture f;
    TimelineController c {f.ctx};
    TimelineWidget w {f.ctx, &c};
    View()
    {
        c.connectActions();
        w.resize(1200, 400);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.setPixelsPerFrame(10.0, kHeaderWidth);
        w.scrollToFrame(0);
    }
    QWidget* vp() { return w.viewport(); }
    /// A point on the ruler's marker band at @p frame.
    QPoint ruler(double frame) const
    {
        return QPoint(int(std::lround(w.layout().frameToX(frame))), kRulerHeight - 8);
    }
    void drag(QPoint from, QPoint to, Qt::KeyboardModifiers mods)
    {
        QTest::mousePress(vp(), Qt::LeftButton, mods, from);
        for (int i = 1; i <= 4; ++i)
            QTest::mouseMove(vp(), from + (to - from) * i / 4);
        QTest::mouseRelease(vp(), Qt::LeftButton, mods, to);
    }
    void addMarker(int64_t frame)
    {
        f.project.timeline()->setPlayhead(frame);
        c.addMarker();
    }
    int undoCount() const { return f.project.undoStack()->index(); }
};

} // namespace

class TestPolish4 : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_polish4"); }

    // ---- Ranged markers (spec 18.11) ----

    void spec18_11_altDragStretchesAMarker()
    {
        View v;
        v.f.project.beginEdit(QStringLiteral("setup"));
        ops::overwriteClip(v.f.data(), {TrackType::Video, 0}, solidClip(0, 200));
        v.f.project.endEdit();
        v.addMarker(20);
        const int before = v.undoCount();
        v.drag(v.ruler(20), v.ruler(45), Qt::AltModifier);
        QCOMPARE(v.f.view().markers.size(), 1);
        QCOMPARE(v.f.view().markers[0].frame, int64_t(20));
        QCOMPARE(v.f.view().markers[0].duration, int64_t(25));
        QCOMPARE(v.undoCount(), before + 1);
        QCOMPARE(v.f.project.undoStack()->undoText(), QStringLiteral("Change Marker Duration"));
        // Its end can be dragged without Alt; never below one frame.
        v.drag(v.ruler(45), v.ruler(10), {});
        QCOMPARE(v.f.view().markers[0].duration, int64_t(1));
        v.f.project.undo();
        QCOMPARE(v.f.view().markers[0].duration, int64_t(25));
        // A plain drag on a single-frame marker moves it (spec 18.14), one step.
        v.f.project.undo();
        const int beforeMove = v.undoCount();
        v.drag(v.ruler(20), v.ruler(60), {});
        QCOMPARE(v.f.view().markers[0].frame, int64_t(60));
        QCOMPARE(v.f.view().markers[0].duration, int64_t(1));
        QCOMPARE(v.undoCount(), beforeMove + 1);
        QCOMPARE(v.f.project.undoStack()->undoText(), QStringLiteral("Move Marker"));
    }

    void spec18_14_markersMoveButNeverOntoAnother()
    {
        View v;
        v.addMarker(20);
        v.addMarker(50);
        v.drag(v.ruler(20), v.ruler(50), {}); // onto the other one: refused
        QCOMPARE(v.f.view().markers[0].frame, int64_t(20));
        QCOMPARE(v.f.view().markers[1].frame, int64_t(50));
        v.drag(v.ruler(50), v.ruler(10), {}); // past the first: the list stays sorted
        QCOMPARE(v.f.view().markers[0].frame, int64_t(10));
        QCOMPARE(v.f.view().markers[1].frame, int64_t(20));
        // A click on a flag (no drag) jumps the playhead to it and changes nothing.
        v.f.project.timeline()->setPlayhead(0);
        const int before = v.undoCount();
        QTest::mouseClick(v.vp(), Qt::LeftButton, {}, v.ruler(20) + QPoint(2, 0));
        QCOMPARE(v.f.project.timeline()->playhead(), int64_t(20));
        QCOMPARE(v.undoCount(), before);
    }

    void spec18_15_clipMarkerDoubleClickOpensTheEditor()
    {
        View v;
        v.f.project.beginEdit(QStringLiteral("setup"));
        ClipData clip = solidClip(0, 100);
        Marker cm;
        cm.frame = 30;
        clip.markers = {cm};
        const quint64 id = ops::overwriteClip(v.f.data(), {TrackType::Video, 0}, clip);
        v.f.project.endEdit();
        bool opened = false;
        v.c.setMarkerEditor([&](Marker& m, bool clipMarker) {
            opened = clipMarker;
            m.name = QStringLiteral("Edited");
            return true;
        });
        const TimelineLayout& L = v.w.layout();
        const QRectF r = L.clipRect(L.rowOf({TrackType::Video, 0}), *ops::clipById(v.f.view(), id));
        QTest::mouseDClick(
            v.vp(), Qt::LeftButton, {}, QPoint(int(std::lround(L.frameToX(30))), int(r.top()) + 7));
        QVERIFY(opened);
        QCOMPARE(ops::clipById(v.f.view(), id)->markers[0].name, QStringLiteral("Edited"));
    }

    void spec18_11_markerEditorSetsTheDuration()
    {
        View v;
        v.addMarker(30);
        v.c.setMarkerEditor([](Marker& m, bool) {
            m.duration = 50;
            m.name = QStringLiteral("Scene");
            return true;
        });
        QVERIFY(v.c.editTimelineMarker(30));
        QCOMPARE(v.f.view().markers[0].duration, int64_t(50));
        QCOMPARE(v.f.view().markers[0].end(), int64_t(80));
        // Both ends are snap targets; navigation stops at the start.
        const auto snaps = ops::snapCandidates(v.f.view(), 0);
        QVERIFY(std::find(snaps.cbegin(), snaps.cend(), 80) != snaps.cend());
        v.f.project.timeline()->setPlayhead(0);
        v.c.goToNextMarker();
        QCOMPARE(v.f.project.timeline()->playhead(), int64_t(30));
        v.c.goToNextMarker();
        QCOMPARE(v.f.project.timeline()->playhead(), int64_t(30)); // no stop at the end
    }

    void spec18_11_rangedMarkersSerializeOptionally()
    {
        TimelineData d = makeDefaultTimeline(QStringLiteral("T"), {25, 1}, 1920, 1080, 2, 2);
        Marker single;
        single.frame = 5;
        Marker ranged;
        ranged.frame = 10;
        ranged.duration = 40;
        d.markers = {single, ranged};
        ClipData clip = solidClip(0, 100);
        Marker clipRanged;
        clipRanged.frame = 3;
        clipRanged.duration = 12;
        clip.markers = {clipRanged};
        ops::overwriteClip(d, {TrackType::Video, 0}, clip);
        const QJsonObject json = serialization::timelineToJson(d);
        const QJsonArray markers = json.value(QStringLiteral("markers")).toArray();
        QVERIFY(!markers[0].toObject().contains(QStringLiteral("duration"))); // as format 12
        QCOMPARE(markers[1].toObject().value(QStringLiteral("duration")).toInt(), 40);
        TimelineData back;
        QString error;
        QVERIFY2(serialization::timelineFromJson(json, back, &error), qPrintable(error));
        QCOMPARE(back.markers, d.markers);
        QCOMPARE(back.videoTracks[0].clips[0].markers[0].duration, int64_t(12));
        QVERIFY(serialization::kFormatVersion >= 13); // later branches bump it
    }

    void spec18_11_version12FilesLoadUnchanged()
    {
        // A format-12 project (no "duration" anywhere) loads with single-frame markers and is
        // written back without any duration field.
        Project p;
        p.beginEdit(QStringLiteral("m"));
        Marker m;
        m.frame = 7;
        m.name = QStringLiteral("old");
        p.timeline()->mutableData().markers = {m};
        p.endEdit();
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("v12.kkproj"));
        QVERIFY(p.save(path));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        root[QStringLiteral("formatVersion")] = 12;
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(root).toJson());
        file.close();
        Project loaded;
        QString error;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        QCOMPARE(loaded.timeline()->data().markers.size(), 1);
        QCOMPARE(loaded.timeline()->data().markers[0].duration, int64_t(1));
        const QString again = dir.filePath(QStringLiteral("again.kkproj"));
        QVERIFY(loaded.save(again));
        QFile written(again);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QVERIFY(!written.readAll().contains("\"duration\"")); // single frames: no new field
    }

    void spec18_11_editIndexShowsTheDuration()
    {
        TimelineFixture f;
        f.project.beginEdit(QStringLiteral("m"));
        Marker m;
        m.frame = 25;
        m.duration = 75;
        f.data().markers = {m};
        f.project.endEdit();
        EditIndexModel model(&f.project);
        model.setMode(EditIndexModel::Mode::Markers);
        model.rebuild();
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.index(0, EditIndexModel::MarkerDuration).data().toString(),
            QStringLiteral("00:00:03:00"));
        QCOMPARE(model.index(0, EditIndexModel::MarkerDuration)
                     .data(EditIndexModel::SortRole)
                     .toLongLong(),
            75);
    }

    void spec18_11_paintsRangedMarkers()
    {
        // Smoke: a ranged ruler marker and a ranged clip marker paint their band.
        View v;
        v.f.project.beginEdit(QStringLiteral("setup"));
        ClipData clip = solidClip(0, 100);
        Marker cm;
        cm.frame = 10;
        cm.duration = 30;
        cm.color = QColor(0xe0, 0x4a, 0x4a);
        clip.markers = {cm};
        ops::overwriteClip(v.f.data(), {TrackType::Video, 0}, clip);
        Marker tm;
        tm.frame = 40;
        tm.duration = 20;
        tm.color = QColor(0x4c, 0xb8, 0x4c);
        v.f.data().markers = {tm};
        v.f.project.endEdit();
        QImage img(v.vp()->size(), QImage::Format_ARGB32_Premultiplied);
        v.vp()->render(&img);
        // Inside the ruler band (frame 50) the colour is green-ish.
        const QColor band = img.pixelColor(v.ruler(50).x(), kRulerHeight - 10);
        QVERIFY2(band.green() > band.red() && band.green() > band.blue(), qPrintable(band.name()));
    }

    // ---- Edit Index column chooser (spec 18.10) ----

    void spec18_10_headerMenuTogglesColumns()
    {
        TimelineFixture f;
        QSettings().remove(QLatin1String(prefs::kEditIndexHiddenColumns));
        {
            EditIndexPanel panel(f.ctx);
            panel.setMode(EditIndexModel::Mode::Edits);
            std::unique_ptr<QMenu> menu(panel.createColumnMenu(nullptr));
            QCOMPARE(menu->actions().size(), EditIndexModel::EditColumnCount - 1); // no "#"
            QAction* reel = menu->actions().at(EditIndexModel::EditReel - 1);
            QCOMPARE(reel->text(), QStringLiteral("Reel"));
            QVERIFY(reel->isChecked());
            reel->trigger();
            QVERIFY(panel.table()->isColumnHidden(EditIndexModel::EditReel));
            // Markers mode has its own choice.
            panel.setMode(EditIndexModel::Mode::Markers);
            QVERIFY(!panel.table()->isColumnHidden(EditIndexModel::MarkerColor));
            panel.setColumnVisible(EditIndexModel::MarkerNotes, false);
            panel.setColumnVisible(0, false); // "#" stays
            QVERIFY(!panel.table()->isColumnHidden(0));
        }
        // Persisted: a new panel starts with the same columns hidden.
        EditIndexPanel again(f.ctx);
        again.setMode(EditIndexModel::Mode::Edits);
        QVERIFY(again.table()->isColumnHidden(EditIndexModel::EditReel));
        QVERIFY(!again.table()->isColumnHidden(EditIndexModel::EditName));
        again.setMode(EditIndexModel::Mode::Markers);
        QVERIFY(again.table()->isColumnHidden(EditIndexModel::MarkerNotes));
        QVERIFY(again.table()->horizontalHeader()->contextMenuPolicy() == Qt::CustomContextMenu);
        QSettings().remove(QLatin1String(prefs::kEditIndexHiddenColumns));
    }

    // ---- History window (spec 17.6) ----

    void spec17_6_historyListsAndJumps()
    {
        View v;
        for (int i = 0; i < 3; ++i)
            v.addMarker(10 + i * 10);
        HistoryWindow history(&v.f.project);
        history.show();
        QVERIFY(QTest::qWaitForWindowExposed(&history));
        QUndoView* list = history.view();
        QCOMPARE(list->model()->rowCount(), 4); // Original State + three steps
        QCOMPARE(list->model()->index(0, 0).data().toString(), QStringLiteral("Original State"));
        QCOMPARE(list->model()->index(2, 0).data().toString(), QStringLiteral("Add Marker"));
        // Click the first step: the two later ones are undone at once.
        const QRect r = list->visualRect(list->model()->index(1, 0));
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, r.center());
        QCOMPARE(v.f.project.undoStack()->index(), 1);
        QCOMPARE(v.f.view().markers.size(), 1);
        // And forward again.
        const QRect last = list->visualRect(list->model()->index(3, 0));
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, last.center());
        QCOMPARE(v.f.view().markers.size(), 3);
        // While an edit is open the list ignores clicks.
        v.f.project.beginEdit(QStringLiteral("open"));
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, r.center());
        QCOMPARE(v.f.project.undoStack()->index(), 3);
        v.f.project.cancelEdit();
    }

    void spec17_6_historyCommandOpensOneWindow()
    {
        View v;
        QAction* open = v.f.actions.action(QStringLiteral("edit.history_window"));
        QVERIFY(open);
        QCOMPARE(open->text(), QStringLiteral("Open History &Window…"));
    }
};

QTEST_MAIN(TestPolish4)
#include "test_polish4.moc"
