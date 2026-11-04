// SPDX-License-Identifier: GPL-3.0-or-later
// On-screen Dynamic Zoom controls of the timeline viewer (spec 24.20): the mode in the
// on-screen controls menu, the whole picture while it is chosen, moving and resizing the
// start (green) and end (red) rectangles, one undo step per drag, Esc cancels.
#include "core/DynamicZoom.h"
#include "core/EditOps.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/viewer/DynamicZoomOverlay.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>

#include <cmath>
#include <memory>

using namespace kk;
namespace dz = kk::dynamiczoom;

namespace {

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    std::unique_ptr<PlaybackEngine> engine;
    AppContext ctx;
    quint64 clipId = 0;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        PlaybackEngine::Options options;
        options.noAudio = true;
        engine = std::make_unique<PlaybackEngine>(std::move(options));
        engine->setMediaPool(project.mediaPool());
        engine->setTimeline(project.timeline());
        ctx.playback = engine.get();
        TimelineData tl = makeDefaultTimeline(QStringLiteral("Demo"), {25, 1}, 320, 180, 2, 2);
        ClipData c;
        c.id = tl.allocateId();
        c.kind = ClipKind::Solid;
        c.duration = 100;
        c.solidColor = QColor(0x30, 0x60, 0x90);
        c.transform.setStatic(keys::DynamicZoom, true);
        tl.videoTracks[0].clips.push_back(c);
        project.timeline()->setData(tl);
        clipId = c.id;
        selection.setClips({clipId});
    }
    [[nodiscard]] const ClipData& clip() const
    {
        return *editops::clipById(project.timeline()->data(), clipId);
    }
    QAction* action(const char* id) { return actions.action(QLatin1String(id)); }
};

void press(QWidget* w, QPointF p)
{
    QMouseEvent e(QEvent::MouseButtonPress, p, w->mapToGlobal(p), Qt::LeftButton, Qt::LeftButton,
        Qt::NoModifier);
    QCoreApplication::sendEvent(w, &e);
}
void move(QWidget* w, QPointF p)
{
    QMouseEvent e(
        QEvent::MouseMove, p, w->mapToGlobal(p), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &e);
}
void release(QWidget* w, QPointF p)
{
    QMouseEvent e(QEvent::MouseButtonRelease, p, w->mapToGlobal(p), Qt::LeftButton, Qt::NoButton,
        Qt::NoModifier);
    QCoreApplication::sendEvent(w, &e);
}

bool near(double a, double b, double tolerance = 1e-3)
{
    return std::abs(a - b) <= tolerance;
}

} // namespace

class TestDynamicZoomControls : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        initResources();
    }

    void spec24_20_onScreenDynamicZoomControls()
    {
        Fixture f;
        ViewerPanel viewer(ViewerPanel::Mode::Timeline, f.ctx);
        viewer.resize(700, 500);
        viewer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        DynamicZoomOverlay* overlay = viewer.dynamicZoomOverlay();
        QVERIFY(overlay);
        QVERIFY(!overlay->isEditing());
        QVERIFY(f.engine->dynamicZoomApplied());

        // The mode is in the on-screen controls menu after Crop; choosing it shows the whole
        // picture (no Dynamic Zoom in the viewer) and the rectangles.
        auto* modes = viewer.findChild<QToolButton*>(QStringLiteral("ViewerOnScreenControls"));
        QVERIFY(modes && modes->menu());
        QStringList entries;
        for (QAction* a : modes->menu()->actions())
            entries << a->text().remove(QLatin1Char('&'));
        const qsizetype crop = entries.indexOf(QStringLiteral("On-Screen Crop Controls"));
        QCOMPARE(entries.value(crop + 1), QStringLiteral("On-Screen Dynamic Zoom Controls"));
        f.action("view.onscreen_dynamic_zoom")->setChecked(true);
        QCOMPARE(modes->text(), QStringLiteral("Dynamic Zoom"));
        QVERIFY(overlay->isEditing());
        QVERIFY(!f.engine->dynamicZoomApplied());

        const auto map = overlay->pictureToWidget();
        QVERIFY(map);
        const QSizeF frame(320, 180);
        // Drag inside the end (red) rectangle: it moves by the drag; one undo step.
        QUndoStack* undo = f.project.undoStack();
        const int before = undo->index();
        const QRectF endRect = dz::toPicture(dz::end(f.clip().transform), frame);
        const QPointF from = map->map(endRect.center());
        QCOMPARE(overlay->hitTest(from).part, DynamicZoomOverlay::Handle::Part::Move);
        QVERIFY(overlay->hitTest(from).end);
        const QPointF to = map->map(endRect.center() + QPointF(32, 18));
        press(overlay, from);
        move(overlay, (from + to) / 2);
        move(overlay, to);
        release(overlay, to);
        QVERIFY(near(dz::end(f.clip().transform).cx, 0.6));
        QVERIFY(near(dz::end(f.clip().transform).cy, 0.6));
        QVERIFY(near(dz::end(f.clip().transform).size, dz::defaultEnd().size));
        QCOMPARE(undo->index(), before + 1);
        QCOMPARE(undo->text(undo->index() - 1), QStringLiteral("Move Dynamic Zoom"));

        // The start (green) rectangle's top-left corner: it shrinks about the bottom-right.
        const QPointF corner = map->map(QPointF(-160, -90));
        const auto h = overlay->hitTest(corner);
        QCOMPARE(h.part, DynamicZoomOverlay::Handle::Part::Corner);
        QVERIFY(!h.end);
        const QPointF inward = map->map(QPointF(-160 + 64, -90 + 36));
        press(overlay, corner);
        move(overlay, inward);
        release(overlay, inward);
        const dz::Rect s = dz::start(f.clip().transform);
        QVERIFY2(near(s.size, 0.8), qPrintable(QString::number(s.size)));
        QVERIFY(near(s.cx, 0.6));
        QVERIFY(near(s.cy, 0.6));
        QCOMPARE(undo->text(undo->index() - 1), QStringLiteral("Resize Dynamic Zoom"));

        // Esc during a drag cancels it.
        const int steps = undo->index();
        const dz::Rect kept = dz::start(f.clip().transform);
        const QPointF inside = map->map(dz::toPicture(kept, frame).topLeft() + QPointF(10, 10));
        press(overlay, inside);
        move(overlay, inside + QPointF(30, 0));
        QTest::keyClick(overlay, Qt::Key_Escape);
        QCOMPARE(dz::start(f.clip().transform), kept);
        QCOMPARE(undo->index(), steps);

        // Off again: the viewer shows the zoom.
        f.action("view.onscreen_dynamic_zoom")->setChecked(false);
        QVERIFY(!overlay->isEditing());
        QVERIFY(f.engine->dynamicZoomApplied());
    }
};

QTEST_MAIN(TestDynamicZoomControls)
#include "test_dynamiczoomcontrols.moc"
