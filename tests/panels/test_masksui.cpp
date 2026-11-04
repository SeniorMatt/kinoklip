// SPDX-License-Identifier: GPL-3.0-or-later
// Mask UI: the Inspector's Masks section (add / reorder / delete / enable / mode / keys) and the
// viewer's MaskOverlay gestures (move, resize, rotate, feather, drawing polygon / bezier /
// freehand shapes, point editing), the overlay's zoom mapping and one undo step per gesture
// (docs/behavior-spec.md section 27).
#include "PanelTestUtils.h"
#include "app/Theme.h"
#include "core/KeyframeOps.h"
#include "core/Mask.h"
#include "core/ParamKeys.h"
#include "core/Timeline.h"
#include "ui/common/Resources.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/KeyframeButton.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/inspector/ParamRows.h"
#include "ui/inspector/ValueField.h"
#include "ui/keyframes/KeyframeItemModel.h"
#include "ui/viewer/MaskOverlay.h"
#include "ui/viewer/ViewerSurface.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>

#include <cmath>
#include <memory>

using namespace kk;
using namespace kk::test;

namespace {

const TrackRef kV1 {TrackType::Video, 0};

/// Sends press, moves (with the button held) and release at widget positions.
void dragPath(QWidget* w, const QVector<QPointF>& points, Qt::KeyboardModifiers mods = {})
{
    auto send
        = [&](QEvent::Type type, QPointF p, Qt::MouseButton button, Qt::MouseButtons buttons) {
              QMouseEvent e(type, p, w->mapToGlobal(p), button, buttons, mods);
              QApplication::sendEvent(w, &e);
          };
    send(QEvent::MouseButtonPress, points.front(), Qt::LeftButton, Qt::LeftButton);
    for (qsizetype i = 1; i < points.size(); ++i)
        send(QEvent::MouseMove, points[i], Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, points.back(), Qt::LeftButton, Qt::NoButton);
}

void click(QWidget* w, QPointF p, Qt::KeyboardModifiers mods = {})
{
    dragPath(w, {p}, mods);
}

bool near(double a, double b, double eps = 0.6)
{
    return std::abs(a - b) <= eps;
}

/// A 320 × 180 timeline with one solid clip selected (content = the frame), a raster viewer
/// surface of 656 × 376 widget px (picture 640 × 360 in Fit: 2 widget px per timeline px) and
/// the overlay on it.
struct OverlayFixture
{
    PanelFixture f;
    std::unique_ptr<QWidget> host;
    ViewerSurface* surface = nullptr;
    MaskOverlay* overlay = nullptr;
    quint64 clipId = 0;

    OverlayFixture()
    {
        f.project.timeline()->mutableData()
            = makeDefaultTimeline(QStringLiteral("t"), Rational {25, 1}, 320, 180, 2, 2);
        clipId = f.addClip(kV1, 0, 100, ClipKind::Solid);
        f.selection.selectClip(clipId);
        host = std::make_unique<QWidget>();
        surface = ViewerSurface::create(host.get());
        surface->setAspect(QSize(320, 180));
        surface->widget()->setGeometry(0, 0, 656, 376);
        overlay = new MaskOverlay(f.ctx, surface);
        host->resize(656, 376);
        host->show();
        QCoreApplication::processEvents();
    }
    quint64 addMask(MaskData m)
    {
        f.project.beginEdit(QStringLiteral("Add Mask"));
        ClipData* c = editops::clipById(f.project.timeline()->mutableData(), clipId);
        m.id = f.project.timeline()->mutableData().allocateId();
        c->masks.append(m);
        f.project.timeline()->notifyChanged();
        f.project.endEdit();
        f.selection.setActiveMask(m.id);
        return m.id;
    }
    const MaskData& mask(int i = 0) const { return f.clip(clipId)->masks[i]; }
    /// Widget position of content point (x, y).
    QPointF at(double x, double y) const { return overlay->contentToWidget()->map(QPointF(x, y)); }
    int undoCount() const { return f.project.undoStack()->count(); }
};

ParamGroupBox* groupNamed(InspectorPanel& panel, const QString& name)
{
    for (ParamGroupBox* g : panel.groups()) {
        if (g->objectName() == name)
            return g;
    }
    return nullptr;
}

} // namespace

class TestMasksUi : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        initResources();
        theme::apply(*qApp);
    }

    // ---- Inspector ----

    void inspectorAddsMasks()
    {
        PanelFixture f;
        const quint64 id = f.addClip(kV1, 0, 100, ClipKind::Solid);
        InspectorPanel panel(f.ctx);
        panel.resize(380, 900);
        f.selection.selectClip(id);
        auto* addEllipse = panel.findChild<QToolButton*>(QStringLiteral("add-mask:ellipse"));
        QVERIFY(addEllipse);
        const int before = f.project.undoStack()->count();
        addEllipse->click();
        QCOMPARE(f.project.undoStack()->count(), before + 1); // one step
        const ClipData* c = f.clip(id);
        QCOMPARE(c->masks.size(), 1);
        QCOMPARE(c->masks[0].shape, MaskShape::Ellipse);
        // Half of the 1920 × 1080 picture, active in the viewer.
        QCOMPARE(c->masks[0].value(maskkeys::Width, 0), 960.0);
        QCOMPARE(f.selection.activeMask(), c->masks[0].id);
        ParamGroupBox* group
            = groupNamed(panel, QStringLiteral("mask-group:%1").arg(c->masks[0].id));
        QVERIFY(group);
        QVERIFY(group->findChild<ValueField*>(QStringLiteral("value:") + maskkeys::FeatherOut));
        QVERIFY(group->findChild<ValueField*>(QStringLiteral("value:") + maskkeys::Width));
        // A bezier mask has no width / height rows but a shape row.
        panel.findChild<QToolButton*>(QStringLiteral("add-mask:bezier"))->click();
        const quint64 bezierId = f.clip(id)->masks[1].id;
        ParamGroupBox* bezier = groupNamed(panel, QStringLiteral("mask-group:%1").arg(bezierId));
        QVERIFY(bezier);
        QVERIFY(!bezier->findChild<ValueField*>(QStringLiteral("value:") + maskkeys::Width));
        QVERIFY(bezier->findChild<KeyframeButton*>(
            QStringLiteral("keyframe:mask-shape:%1").arg(bezierId)));
        QCOMPARE(f.selection.activeMask(), bezierId);
        saveScreenshot(panel, QStringLiteral("inspector-masks"));
    }

    void inspectorEditsMasks()
    {
        PanelFixture f;
        const quint64 id = f.addClip(kV1, 0, 100, ClipKind::Solid);
        InspectorPanel panel(f.ctx);
        f.selection.selectClip(id);
        panel.findChild<QToolButton*>(QStringLiteral("add-mask:rectangle"))->click();
        panel.findChild<QToolButton*>(QStringLiteral("add-mask:ellipse"))->click();
        const quint64 rect = f.clip(id)->masks[0].id;
        const quint64 ellipse = f.clip(id)->masks[1].id;

        // Value edits through the generic rows.
        ParamGroupBox* g = groupNamed(panel, QStringLiteral("mask-group:%1").arg(rect));
        auto* feather = g->findChild<ValueField*>(QStringLiteral("value:") + maskkeys::FeatherOut);
        QVERIFY(feather);
        emit feather->valueCommitted(12.0);
        QCOMPARE(f.clip(id)->masks[0].value(maskkeys::FeatherOut, 0), 12.0);
        // Mode and invert map to the struct fields.
        auto* mode = g->findChild<QComboBox*>();
        QVERIFY(mode);
        const int subtract = mode->findData(QStringLiteral("subtract"));
        QVERIFY(subtract >= 0);
        mode->setCurrentIndex(subtract);
        emit mode->activated(subtract);
        QCOMPARE(f.clip(id)->masks[0].mode, MaskMode::Subtract);
        // Enable toggle.
        g->findChild<QCheckBox*>(QStringLiteral("mask-enable:%1").arg(rect))->click();
        QVERIFY(!f.clip(id)->masks[0].enabled);
        // Reorder: the ellipse moves up.
        ParamGroupBox* e = groupNamed(panel, QStringLiteral("mask-group:%1").arg(ellipse));
        const auto buttons = e->findChildren<QToolButton*>();
        for (QToolButton* b : buttons) {
            if (b->toolTip() == InspectorPanel::tr("Move up")) {
                b->click();
                break;
            }
        }
        QCOMPARE(f.clip(id)->masks[0].id, ellipse);
        // Section diamond keys every mask parameter at the playhead.
        e = groupNamed(panel, QStringLiteral("mask-group:%1").arg(ellipse));
        auto* diamond = e->findChild<KeyframeButton*>(
            QStringLiteral("keyframe-section:mask:%1").arg(ellipse));
        QVERIFY(diamond);
        diamond->click();
        QVERIFY(f.clip(id)->masks[0].params.values[maskkeys::Opacity].hasKeyAt(0));
        QVERIFY(f.clip(id)->masks[0].params.values[maskkeys::Width].hasKeyAt(0));
        // ... and the keyframe panel lists them in a "Masks" group.
        const KeyframeItems items = keyframeitems::build(f.project.timeline()->data(), {id}, false);
        bool masksGroup = false;
        for (const KeyframeEntry& entry : items.entries)
            masksGroup = masksGroup
                || (entry.kind == KeyRow::Kind::Group && entry.label == QStringLiteral("Masks"));
        QVERIFY(masksGroup);
        // Delete.
        auto* remove = panel.findChild<QToolButton*>(QStringLiteral("mask-remove:%1").arg(rect));
        QVERIFY(remove);
        remove->click();
        QCOMPARE(f.clip(id)->masks.size(), 1);
        // Undo restores it.
        f.project.undo();
        QCOMPARE(f.clip(id)->masks.size(), 2);
    }

    // ---- Viewer overlay ----

    void overlayVisibility()
    {
        OverlayFixture o;
        QVERIFY(!o.overlay->isEditing()); // no active mask, controls off
        o.addMask(makeMask(MaskShape::Rectangle, 0, QSizeF(100, 60)));
        QVERIFY(o.overlay->isEditing());
        QVERIFY(o.overlay->isVisible());
        o.f.project.timeline()->setPlayhead(150); // outside the clip
        QVERIFY(!o.overlay->isEditing());
        o.f.project.timeline()->setPlayhead(10);
        QVERIFY(o.overlay->isEditing());
        o.f.selection.setActiveMask(0);
        QVERIFY(!o.overlay->isEditing());
        // The on-screen controls action shows the outlines without an active mask.
        o.f.actions.action(QStringLiteral("view.onscreen_masks"))->setChecked(true);
        QVERIFY(o.overlay->isEditing());
        o.f.actions.action(QStringLiteral("view.onscreen_masks"))->setChecked(false);
    }

    void overlayFollowsZoom()
    {
        OverlayFixture o;
        o.addMask(makeMask(MaskShape::Rectangle, 0, QSizeF(100, 60)));
        // Fit: the 320 × 180 timeline fills 640 × 360 px centred in 656 × 376.
        QCOMPARE(o.at(0, 0), QPointF(328, 188));
        QCOMPARE(o.at(10, 0) - o.at(0, 0), QPointF(20, 0));
        o.surface->setZoom(1.0); // 100 %: one device pixel per timeline pixel
        o.overlay->refresh();
        QCOMPARE(
            o.at(10, 0) - o.at(0, 0), QPointF(10 / o.surface->widget()->devicePixelRatioF(), 0));
        // A transformed clip moves its masks along.
        o.surface->setZoom(0.0);
        o.f.project.beginEdit(QStringLiteral("t"));
        editops::clipById(o.f.project.timeline()->mutableData(), o.clipId)
            ->transform.setBase(keys::PosX, 50);
        o.f.project.endEdit();
        QCOMPARE(o.at(0, 0), QPointF(428, 188));
    }

    void overlayMovesResizesRotates()
    {
        OverlayFixture o;
        o.addMask(makeMask(MaskShape::Rectangle, 0, QSizeF(100, 60)));
        int steps = o.undoCount();
        // Move: drag inside by (20, 10) content px.
        dragPath(o.overlay, {o.at(0, 0), o.at(10, 5), o.at(20, 10)});
        QVERIFY(near(o.mask().value(maskkeys::PosX, 0), 20));
        QVERIFY(near(o.mask().value(maskkeys::PosY, 0), 10));
        QCOMPARE(o.undoCount(), ++steps);
        // Resize from the bottom-right corner (70, 40): the top-left corner stays.
        dragPath(o.overlay, {o.at(70, 40), o.at(90, 50)});
        QVERIFY(near(o.mask().value(maskkeys::Width, 0), 120));
        QVERIFY(near(o.mask().value(maskkeys::Height, 0), 70));
        QVERIFY(near(o.mask().value(maskkeys::PosX, 0), 30));
        QVERIFY(near(o.mask().value(maskkeys::PosY, 0), 15));
        QCOMPARE(o.undoCount(), ++steps);
        // Alt: from the centre (right edge at x = 90 → 100 grows both sides).
        dragPath(o.overlay, {o.at(90, 15), o.at(100, 15)}, Qt::AltModifier);
        QVERIFY(near(o.mask().value(maskkeys::Width, 0), 140));
        QVERIFY(near(o.mask().value(maskkeys::PosX, 0), 30));
        // Rotate: the handle 26 px (13 content px) above the top edge (y = 15 - 35 = -20).
        const QPointF handle = o.at(30, -20 - 13);
        dragPath(o.overlay, {handle, o.at(30 + 60, 15)}); // to the right of the centre: +90°
        QVERIFY2(near(o.mask().value(maskkeys::Rotation, 0), 90, 1.0),
            QByteArray::number(o.mask().value(maskkeys::Rotation, 0)));
        steps += 2;
        QCOMPARE(o.undoCount(), steps);
        o.f.project.undo();
        QVERIFY(near(o.mask().value(maskkeys::Rotation, 0), 0));
    }

    void overlayFeatherHandles()
    {
        OverlayFixture o;
        o.addMask(makeMask(MaskShape::Ellipse, 0, QSizeF(100, 60)));
        // Markers sit 6 px (3 content px) outside / inside the right edge (x = 50), 16 px
        // (8 content px) below the axis.
        dragPath(o.overlay, {o.at(53, 8), o.at(63, 8), o.at(73, 8)});
        QVERIFY2(near(o.mask().value(maskkeys::FeatherOut, 0), 20, 2.5),
            QByteArray::number(o.mask().value(maskkeys::FeatherOut, 0)));
        QCOMPARE(o.mask().value(maskkeys::FeatherIn, 0), 0.0);
        // The inside marker (3 content px inside the edge while the inner feather is 0; the
        // outside one moved to x = 73) pulls the inner feather.
        dragPath(o.overlay, {o.at(47, 8), o.at(42, 8), o.at(37, 8)});
        QVERIFY2(near(o.mask().value(maskkeys::FeatherIn, 0), 10, 2.5),
            QByteArray::number(o.mask().value(maskkeys::FeatherIn, 0)));
    }

    void overlayDrawsPolygon()
    {
        OverlayFixture o;
        MaskData m = makeMask(MaskShape::Polygon, 0, {});
        o.addMask(m);
        int steps = o.undoCount();
        click(o.overlay, o.at(-50, -40));
        click(o.overlay, o.at(50, -40));
        click(o.overlay, o.at(0, 40));
        QCOMPARE(o.mask().path.pointCount(), 3);
        QVERIFY(!o.mask().closed);
        QCOMPARE(o.undoCount(), steps + 3); // one step per point
        click(o.overlay, o.at(-50, -40)); // the first point closes
        QVERIFY(o.mask().closed);
        QCOMPARE(o.mask().path.pointCount(), 3);
        QCOMPARE(o.undoCount(), steps + 4);
        // Recentred: the mask position is the bounding box centre, the outline unchanged.
        QVERIFY2(near(o.mask().value(maskkeys::PosY, 0), 0),
            QByteArray::number(o.mask().value(maskkeys::PosX, 0)) + " "
                + QByteArray::number(o.mask().value(maskkeys::PosY, 0)));
        const QPointF first = maskToContent(o.mask(), 0, o.mask().path.at(0)[0].pos());
        QVERIFY(near(first.x(), -50) && near(first.y(), -40));

        // Point editing: drag the tip down, insert a point on an edge, delete it again.
        dragPath(o.overlay, {o.at(0, 40), o.at(0, 60)});
        QVERIFY(near(maskToContent(o.mask(), 0, o.mask().path.at(0)[2].pos()).y(), 60));
        click(o.overlay, o.at(0, -40)); // on the top edge
        QCOMPARE(o.mask().path.pointCount(), 4);
        QCOMPARE(o.overlay->selectedPoints(), QSet<int>({1}));
        o.overlay->setFocus();
        QTest::keyClick(o.overlay, Qt::Key_Delete);
        QCOMPARE(o.mask().path.pointCount(), 3);
    }

    void overlayDrawsBezierAndFreehand()
    {
        OverlayFixture o;
        o.addMask(makeMask(MaskShape::Bezier, 0, {}));
        // Press and drag: a smooth point with tangents along the drag.
        dragPath(o.overlay, {o.at(-60, 0), o.at(-60, -30)});
        click(o.overlay, o.at(60, 0));
        QTest::keyClick(o.overlay, Qt::Key_Return); // Enter closes
        QVERIFY(o.mask().closed);
        const MaskPath path = o.mask().path.at(0);
        QCOMPARE(path.size(), 2);
        QVERIFY(path[0].smooth);
        QVERIFY(near(path[0].outY, -30) && near(path[0].inY, 30));
        // Turning a tangent of a smooth point mirrors the other side; Alt breaks it.
        const QPointF tip = maskToContent(
            o.mask(), 0, QPointF(path[0].x + path[0].outX, path[0].y + path[0].outY));
        dragPath(o.overlay, {o.at(tip.x(), tip.y()), o.at(tip.x() + 30, tip.y())});
        const MaskPath turned = o.mask().path.at(0);
        QVERIFY(near(turned[0].outX, 30)
            && near(turned[0].inX, -30 * 30 / std::hypot(30.0, 30.0), 1.5));

        OverlayFixture fh;
        fh.addMask(makeMask(MaskShape::Freehand, 0, {}));
        QVector<QPointF> stroke;
        for (int i = 0; i <= 72; ++i) {
            const double a = i * 5.0 * M_PI / 180.0;
            stroke.append(fh.at(60 * std::cos(a), 40 * std::sin(a)));
        }
        const int steps = fh.undoCount();
        dragPath(fh.overlay, stroke);
        QCOMPARE(fh.undoCount(), steps + 1);
        QVERIFY(fh.mask().closed);
        QVERIFY(fh.mask().path.pointCount() >= 6);
        QVERIFY(fh.mask().isRenderable());
    }

    void overlayDrawingCancel()
    {
        OverlayFixture o;
        o.addMask(makeMask(MaskShape::Polygon, 0, {}));
        click(o.overlay, o.at(0, 0));
        o.overlay->setFocus();
        QTest::keyClick(o.overlay, Qt::Key_Escape); // one point: the mask is discarded
        QCOMPARE(o.f.clip(o.clipId)->masks.size(), 0);
        QCOMPARE(o.f.selection.activeMask(), quint64(0));
    }

    void overlayScreenshot()
    {
        // Overlay drawn over a picture (for the docs; KK_SCREENSHOT_DIR).
        OverlayFixture o;
        QImage picture(320, 180, QImage::Format_ARGB32_Premultiplied);
        picture.fill(QColor(40, 70, 110));
        o.surface->setFrame(picture);
        MaskData e = makeMask(MaskShape::Ellipse, 0, QSizeF(140, 90));
        e.params.setBase(maskkeys::PosX, -60);
        e.params.setBase(maskkeys::FeatherOut, 12);
        o.addMask(e);
        MaskData b = makeMask(MaskShape::Bezier, 0, {});
        b.closed = true;
        b.params.setBase(maskkeys::PosX, 80);
        b.path.setShape(0,
            {{-40, -30, -10, 15, 10, -15, true}, {40, -20, -15, -10, 15, 10, true},
                {20, 40, 20, 0, -20, 0, true}, {-30, 30}});
        o.addMask(b);
        saveScreenshot(*o.host, QStringLiteral("viewer-mask-overlay"));
        QVERIFY(o.overlay->isEditing());
    }
};

QTEST_MAIN(TestMasksUi)
#include "test_masksui.moc"
