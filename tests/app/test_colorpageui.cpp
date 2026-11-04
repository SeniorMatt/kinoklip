// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen tests of the full Color page (docs/behavior-spec.md 44b, rows from 44b.12): the
// node editor (add / connect / move / bypass, each one undo step), wheels and bars as one
// step per drag, the Curves palette (linked R G B, HSL modes), the qualifier eyedropper
// (pick / add / remove ranges, highlight), power window handles in the viewer, gallery stills
// (apply, wipe), the clips strip (selection changes the graded clip, middle-click and
// Ctrl+C / Ctrl+V copy grades) and the interface bar toggles.
#include "../media/MediaTestUtil.h"
#include "app/MainWindow.h"
#include "app/pages/ColorPage.h"
#include "core/EditOps.h"
#include "core/GradeGallery.h"
#include "core/GradeMath.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/color/ColorBrowsers.h"
#include "ui/color/ColorFrameSource.h"
#include "ui/color/ColorNodeEditor.h"
#include "ui/color/ColorPalettes.h"
#include "ui/color/ColorSession.h"
#include "ui/color/ColorViewerOverlay.h"
#include "ui/color/ColorWidgets.h"
#include "ui/color/QualifierPick.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/inspector/ValueField.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>

#include <memory>

using namespace kk;
namespace gk = kk::gradekeys;
namespace wk = kk::gradewindowkeys;

namespace {

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    QVector<quint64> clips;

    explicit Fixture(int clipCount = 3)
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        project.beginEdit(QStringLiteral("Add Clips"));
        TimelineData& tl = project.timeline()->mutableData();
        tl.width = 320;
        tl.height = 180;
        const QColor colours[]
            = {QColor(200, 60, 40), QColor(40, 90, 200), QColor(60, 180, 70), QColor(180, 180, 60)};
        for (int i = 0; i < clipCount; ++i) {
            ClipData clip;
            clip.kind = ClipKind::Solid;
            clip.solidColor = colours[i % 4];
            clip.name = QStringLiteral("Clip %1").arg(i + 1);
            clip.start = i * 50;
            clip.duration = 50;
            clips.append(editops::overwriteClip(tl, TrackRef {TrackType::Video, 0}, clip));
        }
        project.endEdit();
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
    ColorPage* page() const { return window->colorPage(); }
    ColorSession* session() const { return page()->session(); }
    /// Undo steps done (the index: undone steps stay on the stack until the next push).
    int steps() const { return project.undoStack()->index(); }
    ClipGrade grade(quint64 clip) const
    {
        return project.clipGrade(project.activeTimelineUid(), clip);
    }
    void showPage(QSize size = QSize(1600, 900))
    {
        window->resize(size);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        action("view.page_color")->trigger();
        QCoreApplication::processEvents();
    }
    void key(Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        page()->nodeEditor()->setFocus();
        QTest::keyClick(page()->nodeEditor(), key, mods);
        QCoreApplication::processEvents();
    }
    /// Gives the frame source a picture: red left half, blue right half.
    void splitInput()
    {
        QImage img(320, 180, QImage::Format_ARGB32_Premultiplied);
        QPainter p(&img);
        p.fillRect(0, 0, 160, 180, QColor(220, 40, 40));
        p.fillRect(160, 0, 160, 180, QColor(40, 60, 220));
        p.end();
        page()->frames()->setInput(img);
        page()->refreshPictures();
    }
};

void drag(QWidget* w, QPointF from, QPointF to, Qt::MouseButton button = Qt::LeftButton,
    Qt::KeyboardModifiers mods = {})
{
    QTest::mousePress(w, button, mods, from.toPoint());
    for (int i = 1; i <= 4; ++i)
        QTest::mouseMove(w, (from + (to - from) * (i / 4.0)).toPoint());
    QTest::mouseRelease(w, button, mods, to.toPoint());
    QCoreApplication::processEvents();
}

} // namespace

class TestColorPageUi : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-test"));
        QCoreApplication::setApplicationName(QStringLiteral("test_colorpageui"));
        QSettings().clear();
        initResources();
    }

    void nodeEditorAddConnectBypassUndo()
    {
        Fixture f;
        f.showPage();
        ColorNodeEditor* ne = f.page()->nodeEditor();
        const quint64 clip = f.clips[0];
        const int base = f.steps();
        // Alt+S twice: two serial nodes; the editor shows both with a numbered tile.
        f.key(Qt::Key_S, Qt::AltModifier);
        f.key(Qt::Key_S, Qt::AltModifier);
        ClipGrade g = f.grade(clip);
        QCOMPARE(g.nodes.size(), 2);
        QCOMPARE(f.steps(), base + 2);
        const quint64 n1 = g.nodes[0].id, n2 = g.nodes[1].id;
        QCOMPARE(g.output, n2);
        QVERIFY(ne->tileRect(g.nodes[0]).isValid());
        QVERIFY(!ne->tileRect(g.nodes[0]).intersects(ne->tileRect(g.nodes[1])));
        // Clicking a tile selects its node.
        QTest::mouseClick(ne, Qt::LeftButton, {}, ne->tileRect(g.nodes[0]).center().toPoint());
        QCOMPARE(f.session()->selectedNode(), n1);
        // Drag node 1's output onto the grade output: node 1 becomes the output (one step).
        drag(ne, ne->outputPos(g.nodes[0]), ne->gradeOutputPos());
        g = f.grade(clip);
        QCOMPARE(g.output, n1);
        QCOMPARE(f.steps(), base + 3);
        f.action("edit.undo")->trigger();
        QCOMPARE(f.grade(clip).output, n2);
        // Drag the source onto node 2's input: it now reads the clip directly.
        g = f.grade(clip);
        drag(ne, ne->sourcePos(), ne->inputPos(*g.node(n2), 0));
        QCOMPARE(f.grade(clip).node(n2)->inputs.value(0), quint64(0));
        f.action("edit.undo")->trigger();
        QCOMPARE(f.grade(clip).node(n2)->inputs.value(0), n1);
        // Picking up node 2's input and dropping it into empty space disconnects it (source).
        g = f.grade(clip);
        drag(ne, ne->inputPos(*g.node(n2), 0), QPointF(ne->width() - 5, 5));
        QCOMPARE(f.grade(clip).node(n2)->inputs.value(0), quint64(0));
        f.action("edit.undo")->trigger();
        // Moving a tile stores GradeNode::pos as one step.
        g = f.grade(clip);
        const QPointF before = g.node(n2)->pos;
        const int beforeMove = f.steps();
        const QPointF at = ne->tileRect(*g.node(n2)).center();
        const double zoom = ne->zoom();
        drag(ne, at, at + QPointF(30, 40));
        QCOMPARE(f.steps(), beforeMove + 1);
        const QPointF after = f.grade(clip).node(n2)->pos;
        QVERIFY(std::abs(after.x() - before.x() - 30 / zoom) < 1.5);
        QVERIFY(std::abs(after.y() - before.y() - 40 / zoom) < 1.5);
        QCOMPARE(ne->zoom(), zoom); // moving a node keeps the view
        // Ctrl+D bypasses the selected node, Ctrl+Z restores it.
        f.session()->selectNode(n2);
        f.key(Qt::Key_D, Qt::ControlModifier);
        QVERIFY(!f.grade(clip).node(n2)->enabled);
        f.action("edit.undo")->trigger();
        QVERIFY(f.grade(clip).node(n2)->enabled);
        // Shift+D: bypass all (viewer state, no undo step).
        const int beforeBypass = f.steps();
        f.key(Qt::Key_D, Qt::ShiftModifier);
        QVERIFY(f.action("color.bypass_all")->isChecked());
        QCOMPARE(f.steps(), beforeBypass);
        f.key(Qt::Key_D, Qt::ShiftModifier);
        QVERIFY(!f.action("color.bypass_all")->isChecked());
        // Alt+P / Alt+L add mixers shown as small tiles with their input ports.
        f.key(Qt::Key_P, Qt::AltModifier);
        g = f.grade(clip);
        const GradeNode* mixer = nullptr;
        for (const GradeNode& n : g.nodes)
            mixer = n.type == GradeNodeType::Parallel ? &n : mixer;
        QVERIFY(mixer);
        QCOMPARE(mixer->inputs.size(), 2);
        QCOMPARE(ne->hitTest(ne->inputPos(*mixer, 2)).kind, ColorNodeEditor::Hit::Kind::Input);
        // The context menu: add / reset / delete through the color.* commands.
        f.session()->selectNode(n1);
        std::unique_ptr<QMenu> menu(ne->buildContextMenu(ne->tileRect(*g.node(n1)).center(), ne));
        QAction* del = menu->findChild<QAction*>(QStringLiteral("color.delete_node"));
        QAction* reset = menu->findChild<QAction*>(QStringLiteral("color.reset_node"));
        QAction* add = menu->findChild<QAction*>(QStringLiteral("color.add_serial"));
        QVERIFY(del && reset && add);
        QVERIFY(del->isEnabled());
        const int nodes = int(g.nodes.size());
        add->trigger();
        QCOMPARE(int(f.grade(clip).nodes.size()), nodes + 1);
        del->trigger();
        QCOMPARE(int(f.grade(clip).nodes.size()), nodes);
        // The stored positions survive undo / redo exactly.
        const ClipGrade stored = f.grade(clip);
        f.action("edit.undo")->trigger();
        f.action("edit.redo")->trigger();
        QCOMPARE(f.grade(clip), stored);
    }

    void wheelAndBarsDragsAreOneStep()
    {
        Fixture f;
        f.showPage();
        PrimariesPanel* prim = f.page()->primaries();
        ColorWheel* lift = prim->wheel(gk::Wheel::Lift);
        const int base = f.steps();
        const QPointF c = lift->discRect().center();
        drag(lift, c, c + QPointF(-15, 10));
        QCOMPARE(f.steps(), base + 1);
        ClipGrade g = f.grade(f.clips[0]);
        QCOMPARE(g.nodes.size(), 1); // the first node came with the drag
        QVERIFY(g.nodes[0].value(gk::LiftB, 0) != 0.0);
        // The master strip.
        const QPointF s = lift->stripRect().center();
        drag(lift, s, s + QPointF(20, 0));
        QCOMPARE(f.steps(), base + 2);
        QVERIFY(f.grade(f.clips[0]).nodes[0].value(gk::LiftY, 0) > 0.0);
        // Double-click resets the wheel (one step).
        QTest::mouseDClick(lift, Qt::LeftButton, {}, c.toPoint());
        g = f.grade(f.clips[0]);
        QCOMPARE(g.nodes[0].value(gk::LiftR, 0), 0.0);
        QCOMPARE(g.nodes[0].value(gk::LiftY, 0), 0.0);
        // The bars view: a drag on the Gain R bar is one step and raises Gain R only.
        prim->setBarsMode(true);
        QVERIFY(prim->barsMode());
        PrimaryBars* bars = prim->bars(gk::Wheel::Gain);
        bars->resize(140, 160);
        const int beforeBars = f.steps();
        const QPointF r = bars->barRect(0).center();
        drag(bars, r, r - QPointF(0, 30));
        QCOMPARE(f.steps(), beforeBars + 1);
        g = f.grade(f.clips[0]);
        QVERIFY(g.nodes[0].value(gk::GainR, 0) > 1.0);
        QCOMPARE(g.nodes[0].value(gk::GainG, 0), 1.0);
        // The numeric fields follow.
        QCOMPARE(prim->field(gk::GainR)->value(), g.nodes[0].value(gk::GainR, 0));
        // A scrubbed field is one step too.
        ValueField* hue = prim->field(gk::Hue);
        const int beforeField = f.steps();
        Q_EMIT hue->scrubStarted();
        Q_EMIT hue->valueScrubbed(5);
        Q_EMIT hue->valueScrubbed(10);
        Q_EMIT hue->scrubFinished();
        QCOMPARE(f.steps(), beforeField + 1);
        QCOMPARE(f.grade(f.clips[0]).nodes[0].value(gk::Hue, 0), 10.0);
    }

    void curvesPalette()
    {
        Fixture f;
        f.showPage();
        f.page()->showPalette(ColorPage::Palette::Curves);
        ColorCurvesPanel* panel = f.page()->curves();
        GradeCurveEditor* ed = panel->editor();
        ed->resize(300, 240);
        // Linked R editing writes R, G and B.
        panel->showCurve(GradeCurve::Red);
        QVERIFY(ed->isLinked());
        const QRectF plot = ed->plotRect();
        QTest::mouseClick(ed, Qt::LeftButton, {},
            QPoint(
                int(plot.left() + plot.width() * 0.4), int(plot.bottom() - plot.height() * 0.6)));
        ClipGrade g = f.grade(f.clips[0]);
        QCOMPARE(g.nodes[0].curves[GradeCurve::Red].size(), 1);
        QCOMPARE(g.nodes[0].curves[GradeCurve::Green], g.nodes[0].curves[GradeCurve::Red]);
        QCOMPARE(g.nodes[0].curves[GradeCurve::Blue], g.nodes[0].curves[GradeCurve::Red]);
        QVERIFY(g.nodes[0].curves[GradeCurve::Luma].isEmpty());
        // Hue vs Sat: a point, then removed by right-click; reset clears the curve.
        panel->showCurve(GradeCurve::HueSat);
        const QRectF hp = ed->plotRect();
        const QPoint at(int(hp.left() + hp.width() * 0.3), int(hp.bottom() - hp.height() * 0.8));
        QTest::mouseClick(ed, Qt::LeftButton, {}, at);
        // A first point on an HSL curve brings two neutral neighbours.
        QCOMPARE(f.grade(f.clips[0]).nodes[0].curves[GradeCurve::HueSat].size(), 3);
        QTest::mouseClick(ed, Qt::RightButton, {}, at);
        QCOMPARE(f.grade(f.clips[0]).nodes[0].curves[GradeCurve::HueSat].size(), 2);
        panel->resetCurve();
        QVERIFY(f.grade(f.clips[0]).nodes[0].curves[GradeCurve::HueSat].isEmpty());
        panel->showCurve(GradeCurve::Red);
        panel->resetCurve();
        g = f.grade(f.clips[0]);
        QVERIFY(g.nodes[0].curves[GradeCurve::Red].isEmpty());
        QVERIFY(g.nodes[0].curves[GradeCurve::Blue].isEmpty());
    }

    void qualifierPickRanges()
    {
        // The pure maths: pick, add, remove.
        const float red[3] = {0.86f, 0.16f, 0.16f};
        const float blue[3] = {0.16f, 0.24f, 0.86f};
        const qualifierpick::Hsl r = qualifierpick::toHsl(red);
        const qualifierpick::Hsl b = qualifierpick::toHsl(blue);
        GradeQualifier q = qualifierpick::apply({}, {r}, qualifierpick::Mode::Pick);
        QVERIFY(q.enabled);
        QVERIFY(grademath::qualifierKey(q, red) > 0.99f);
        QVERIFY(grademath::qualifierKey(q, blue) < 0.01f);
        q = qualifierpick::apply(q, {b}, qualifierpick::Mode::Add);
        QVERIFY(grademath::qualifierKey(q, red) > 0.99f);
        QVERIFY(grademath::qualifierKey(q, blue) > 0.99f);
        q = qualifierpick::apply(q, {b}, qualifierpick::Mode::Remove);
        QVERIFY(grademath::qualifierKey(q, blue) < 0.5f);
        QVERIFY(grademath::qualifierKey(q, red) > 0.99f);
        // Hue arcs wrap around red.
        const auto [centre, width] = qualifierpick::hueArc({0.97, 0.02});
        QVERIFY(width < 0.1);
        QVERIFY(centre > 0.98 || centre < 0.01);

        // On the page: the eyedropper over the viewer picks from the node's input picture.
        Fixture f;
        f.showPage();
        f.splitInput();
        QualifierPanel* qp = f.page()->qualifier();
        f.page()->showPalette(ColorPage::Palette::Qualifier);
        qp->setPickMode(qualifierpick::Mode::Pick);
        ColorViewerOverlay* ov = f.page()->overlay();
        QVERIFY(ov->pickMode());
        const QRectF pic = ov->pictureRect();
        QVERIFY(pic.width() > 50);
        const int base = f.steps();
        QTest::mouseClick(ov, Qt::LeftButton, {},
            QPoint(int(pic.left() + pic.width() * 0.25), int(pic.center().y())));
        QCoreApplication::processEvents();
        ClipGrade g = f.grade(f.clips[0]);
        QCOMPARE(g.nodes.size(), 1); // the pick gave the ungraded clip its first node
        QVERIFY(g.nodes[0].qualifier.enabled);
        QVERIFY(grademath::qualifierKey(g.nodes[0].qualifier, red) > 0.99f);
        QVERIFY(grademath::qualifierKey(g.nodes[0].qualifier, blue) < 0.01f);
        QVERIFY(f.steps() > base);
        // Pick + over the blue half: both selected (one more step).
        qp->setPickMode(qualifierpick::Mode::Add);
        const int beforeAdd = f.steps();
        drag(ov, QPointF(pic.left() + pic.width() * 0.7, pic.top() + 20),
            QPointF(pic.left() + pic.width() * 0.9, pic.bottom() - 20));
        QCOMPARE(f.steps(), beforeAdd + 1);
        g = f.grade(f.clips[0]);
        QVERIFY(grademath::qualifierKey(g.nodes[0].qualifier, blue) > 0.99f);
        // Highlight in black and white: the matte is white on the selection.
        qp->setPickMode(std::nullopt);
        f.page()->setHighlight(true);
        QVERIFY(f.action("color.highlight")->isChecked());
        QVERIFY(!ov->highlightImage().isNull());
        f.session()->setQualifier(
            0, qualifierpick::apply({}, {r}, qualifierpick::Mode::Pick), QStringLiteral("Q"));
        f.page()->refreshPictures();
        const QImage grey = ov->highlightImage();
        // Grey mode: the red half graded, the blue half flat grey.
        const QColor blueSide = grey.pixelColor(grey.width() * 3 / 4, grey.height() / 2);
        QVERIFY2(std::abs(blueSide.red() - blueSide.blue()) < 4, qPrintable(blueSide.name()));
        // Black / white.
        auto* mode = qp->findChild<QComboBox*>(QStringLiteral("QualifierHighlightMode"));
        QVERIFY(mode);
        mode->setCurrentIndex(1);
        const QImage bw = ov->highlightImage();
        QVERIFY(qGray(bw.pixel(bw.width() / 4, bw.height() / 2)) > 250);
        QVERIFY(qGray(bw.pixel(bw.width() * 3 / 4, bw.height() / 2)) < 5);
        f.key(Qt::Key_H, Qt::ShiftModifier); // Shift+H toggles it off
        QVERIFY(!f.page()->isHighlighting());
        QVERIFY(ov->highlightImage().isNull());
    }

    void windowHandles()
    {
        Fixture f;
        f.showPage();
        f.page()->showPalette(ColorPage::Palette::Windows);
        WindowsPanel* wp = f.page()->windows();
        ColorViewerOverlay* ov = f.page()->overlay();
        QVERIFY(ov->windowControls());
        // The Circle button adds a circle window (and the first node) as one step.
        const int base = f.steps();
        wp->shapeButton(GradeWindowShape::Circle)->click();
        QCOMPARE(f.steps(), base + 1);
        const GradeWindow* w = f.session()->window();
        QVERIFY(w);
        QCOMPARE(w->shape, GradeWindowShape::Circle);
        QCOMPARE(wp->list()->count(), 1);
        const quint64 id = w->id;
        // Width handle: drag right = wider; one step.
        GradeWindow win = *w;
        QPointF hw = ov->handlePos(win, ColorViewerOverlay::Handle::Width);
        QCOMPARE(ov->hitTest(hw).handle, ColorViewerOverlay::Handle::Width);
        const double width0 = win.value(wk::Width, 0);
        int before = f.steps();
        drag(ov, hw, hw + QPointF(20, 0));
        QCOMPARE(f.steps(), before + 1);
        win = *f.session()->window(id);
        QVERIFY(win.value(wk::Width, 0) > width0);
        QCOMPARE(win.value(wk::Height, 0), 0.5);
        // Move: drag inside.
        const QPointF centre = ov->handlePos(win, ColorViewerOverlay::Handle::Move);
        drag(ov, centre + QPointF(3, 3), centre + QPointF(33, 3));
        win = *f.session()->window(id);
        QVERIFY(win.value(wk::CenterX, 0) > 0.55);
        QVERIFY(std::abs(win.value(wk::CenterY, 0) - 0.5) < 1e-6);
        // Rotate: the knob dragged to the right of the centre = 90 degrees.
        const QPointF knob = ov->handlePos(win, ColorViewerOverlay::Handle::Rotate);
        QCOMPARE(ov->hitTest(knob).handle, ColorViewerOverlay::Handle::Rotate);
        const QPointF c2 = ov->handlePos(win, ColorViewerOverlay::Handle::Move);
        drag(ov, knob, c2 + QPointF(60, 0));
        win = *f.session()->window(id);
        QVERIFY2(std::abs(win.value(wk::Rotation, 0) - 90.0) < 3.0,
            qPrintable(QString::number(win.value(wk::Rotation, 0))));
        // Softness handle.
        const QPointF soft = ov->handlePos(win, ColorViewerOverlay::Handle::Soft);
        QCOMPARE(ov->hitTest(soft).handle, ColorViewerOverlay::Handle::Soft);
        // The fields show the values; Invert toggles; a second window joins the node.
        QCOMPARE(wp->field(wk::Rotation)->value(), win.value(wk::Rotation, 0));
        auto* invert = wp->findChild<QToolButton*>(QStringLiteral("WindowInvert"));
        invert->click();
        QVERIFY(f.session()->window(id)->invert);
        wp->shapeButton(GradeWindowShape::Polygon)->click();
        QCOMPARE(f.session()->currentNode()->windows.size(), 2);
        QCOMPARE(wp->list()->count(), 2);
        // Polygon: drag a point, insert one with a double-click on an edge, delete with a
        // right-click.
        GradeWindow poly = *f.session()->window();
        QCOMPARE(poly.shape, GradeWindowShape::Polygon);
        const QPointF p0 = ov->handlePos(poly, ColorViewerOverlay::Handle::Point, 0);
        QCOMPARE(ov->hitTest(p0).handle, ColorViewerOverlay::Handle::Point);
        before = f.steps();
        drag(ov, p0, p0 + QPointF(0, -15));
        QCOMPARE(f.steps(), before + 1);
        poly = *f.session()->window();
        QVERIFY(poly.points[0].y() < -0.5);
        const QPointF a = ov->handlePos(poly, ColorViewerOverlay::Handle::Point, 1);
        const QPointF b = ov->handlePos(poly, ColorViewerOverlay::Handle::Point, 2);
        QTest::mouseDClick(ov, Qt::LeftButton, {}, ((a + b) / 2).toPoint());
        QCOMPARE(f.session()->window()->points.size(), 5);
        poly = *f.session()->window();
        QTest::mouseClick(ov, Qt::RightButton, {},
            ov->handlePos(poly, ColorViewerOverlay::Handle::Point, 2).toPoint());
        QCOMPARE(f.session()->window()->points.size(), 4);
        // Delete removes the selected window.
        wp->findChild<QToolButton*>(QStringLiteral("WindowDelete"))->click();
        QCOMPARE(f.session()->currentNode()->windows.size(), 1);
        // Off the Windows palette the controls hide.
        f.page()->showPalette(ColorPage::Palette::Primaries);
        QVERIFY(!ov->windowControls());
    }

    void galleryStillApplyAndWipe()
    {
        Fixture f;
        f.showPage();
        // A still with a grade (desaturate).
        ClipGrade still;
        const quint64 node = gradeops::initialize(still);
        still.node(node)->params.animated(gk::Saturation, 1.0).setValue(0, 0.0);
        QImage img(64, 36, QImage::Format_RGB32);
        img.fill(QColor(90, 90, 90));
        const quint64 id = f.project.gallery()->addStill(img, still);
        ColorGallery* gal = f.page()->gallery();
        QCOMPARE(gal->list()->count(), 1);
        // Double-click applies it to the current clip (one step).
        const int base = f.steps();
        QListWidgetItem* item = gal->list()->item(0);
        const QPoint at = gal->list()->visualItemRect(item).center();
        QTest::mouseClick(gal->list()->viewport(), Qt::LeftButton, {}, at);
        QTest::mouseDClick(gal->list()->viewport(), Qt::LeftButton, {}, at);
        QCOMPARE(f.grade(f.clips[0]), still);
        QCOMPARE(f.steps(), base + 1);
        // Middle-click applies it too (to the clip being graded).
        f.session()->gradeClip(f.clips[1]);
        QTest::mouseClick(gal->list()->viewport(), Qt::MiddleButton, {}, at);
        QCOMPARE(f.grade(f.clips[1]), still);
        // The menu applies it to the clips selected in the strip.
        f.session()->setSelectedClips({f.clips[2]});
        std::unique_ptr<QMenu> menu(gal->buildMenu(id, gal));
        menu->findChild<QAction*>(QStringLiteral("GalleryApplySelected"))->trigger();
        QCOMPARE(f.grade(f.clips[2]), still);
        // Wipe: the overlay shows the still left of the line; Ctrl+W toggles.
        menu->findChild<QAction*>(QStringLiteral("GalleryWipe"))->trigger();
        QVERIFY(f.page()->overlay()->isWiping());
        QVERIFY(f.action("color.wipe")->isChecked());
        f.key(Qt::Key_W, Qt::ControlModifier);
        QVERIFY(!f.page()->overlay()->isWiping());
        f.key(Qt::Key_W, Qt::ControlModifier);
        QVERIFY(f.page()->overlay()->isWiping());
        // Deleting the still ends the wipe.
        f.project.gallery()->removeStill(id);
        QCoreApplication::processEvents();
        QVERIFY(!f.page()->overlay()->isWiping());
    }

    void clipStripSelectsTheGradedClip()
    {
        Fixture f;
        f.showPage();
        ColorClipStrip* strip = f.page()->clipStrip();
        QCOMPARE(strip->count(), 3);
        QCOMPARE(f.session()->clipId(), f.clips[0]);
        // Clicking the second thumbnail grades that clip and moves the playhead into it.
        QTest::mouseClick(
            strip->viewport(), Qt::LeftButton, {}, strip->visualItemRect(strip->item(1)).center());
        QCOMPARE(f.session()->clipId(), f.clips[1]);
        QVERIFY(f.project.timeline()->playhead() >= 50 && f.project.timeline()->playhead() < 100);
        // The mini timeline moves the playhead too.
        ColorMiniTimeline* mini = f.page()->miniTimeline();
        QTest::mouseClick(
            mini, Qt::LeftButton, {}, QPoint(int(mini->xOf(125)), mini->height() / 2));
        QCOMPARE(f.session()->clipId(), f.clips[2]);
        // Grade clip 3, then middle-click its thumbnail while clip 1 is selected: clip 1 gets it.
        QVERIFY(f.session()->setParam(0, gk::Saturation, 0.3, QStringLiteral("Sat")));
        QTest::mouseClick(
            strip->viewport(), Qt::LeftButton, {}, strip->visualItemRect(strip->item(0)).center());
        QCOMPARE(f.session()->clipId(), f.clips[0]);
        QCOMPARE(f.session()->selectedClips(), QVector<quint64> {f.clips[0]});
        const int base = f.steps();
        QTest::mouseClick(strip->viewport(), Qt::MiddleButton, {},
            strip->visualItemRect(strip->item(2)).center());
        QCOMPARE(f.grade(f.clips[0]), f.grade(f.clips[2]));
        QCOMPARE(f.steps(), base + 1);
        // Ctrl+C on clip 1, Ctrl-click clip 2 into the selection, Ctrl+V pastes to both.
        QVERIFY(f.project.setClipGrade(
            f.project.activeTimelineUid(), f.clips[0], ClipGrade {}, QStringLiteral("Reset")));
        QVERIFY(f.session()->setParam(0, gk::Hue, 20.0, QStringLiteral("Hue")));
        f.key(Qt::Key_C, Qt::ControlModifier);
        QVERIFY(f.session()->hasCopiedGrade());
        QTest::mouseClick(strip->viewport(), Qt::LeftButton, Qt::ControlModifier,
            strip->visualItemRect(strip->item(1)).center());
        QCOMPARE(f.session()->selectedClips().size(), 2);
        QCOMPARE(f.session()->clipId(), f.clips[0]); // Ctrl-click only selects
        const int beforePaste = f.steps();
        f.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(f.steps(), beforePaste + 1);
        QCOMPARE(f.grade(f.clips[1]), f.grade(f.clips[0]));
        // The current clip is outlined and graded clips carry a mark (item data).
        QVERIFY(strip->item(0)->data(Qt::UserRole + 4).toBool());
        QVERIFY(strip->item(1)->data(Qt::UserRole + 3).toBool());
    }

    void interfaceBarToggles()
    {
        Fixture f;
        f.showPage();
        ColorPage* page = f.page();
        for (const auto& [id, panel] : {std::pair {"view.toggle_gallery", PanelId::Gallery},
                 std::pair {"view.toggle_clips", PanelId::Clips},
                 std::pair {"view.toggle_color_timeline", PanelId::ColorTimeline},
                 std::pair {"view.toggle_nodes", PanelId::Nodes},
                 std::pair {"view.toggle_scopes", PanelId::Scopes}}) {
            QAction* a = f.action(id);
            QVERIFY2(a->isEnabled(), id);
            QVERIFY2(a->isChecked(), id);
            QVERIFY(page->isPanelVisible(panel));
            a->trigger();
            QVERIFY2(!page->isPanelVisible(panel), id);
            QVERIFY(!a->isChecked());
            a->trigger();
            QVERIFY(page->isPanelVisible(panel));
        }
        // Not on the Edit page.
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        QVERIFY(!f.action("view.toggle_gallery")->isEnabled());
        // Layout round trip.
        f.action("view.page_color")->trigger();
        page->showPalette(ColorPage::Palette::Luts);
        page->setPanelVisible(PanelId::Gallery, false);
        QSettings settings;
        settings.beginGroup(QStringLiteral("colorLayoutTest"));
        page->saveLayout(settings);
        page->setPanelVisible(PanelId::Gallery, true);
        page->showPalette(ColorPage::Palette::Primaries);
        page->restoreLayout(settings);
        QCOMPARE(page->currentPalette(), ColorPage::Palette::Luts);
        QVERIFY(!page->isPanelVisible(PanelId::Gallery));
    }

    void timelineTargetAndTypingKeys()
    {
        Fixture f;
        f.showPage();
        auto* timeline = f.page()->findChild<QToolButton*>(QStringLiteral("ColorTargetButton"));
        QVERIFY(timeline);
        timeline->click();
        QCOMPARE(f.session()->target(), ColorSession::Target::Timeline);
        QVERIFY(f.action("color.timeline_grade")->isChecked());
        // Clicking a clip in the strip goes back to the clip grade.
        ColorClipStrip* strip = f.page()->clipStrip();
        QTest::mouseClick(
            strip->viewport(), Qt::LeftButton, {}, strip->visualItemRect(strip->item(0)).center());
        QCOMPARE(f.session()->target(), ColorSession::Target::Clip);
        QVERIFY(!timeline->isChecked());
        // Typing in a field keeps Backspace / Ctrl+C for the text (no node deleted).
        f.key(Qt::Key_S, Qt::AltModifier);
        ValueField* contrast = f.page()->primaries()->field(gk::Contrast);
        contrast->startTextEdit();
        auto* edit = contrast->findChild<QLineEdit*>();
        QVERIFY(edit);
        edit->setFocus();
        const int nodes = int(f.grade(f.clips[0]).nodes.size());
        QTest::keyClick(edit, Qt::Key_Backspace);
        QCOMPARE(int(f.grade(f.clips[0]).nodes.size()), nodes);
    }

    void lutBrowserAndKeyPanel()
    {
        Fixture f;
        f.showPage();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile cube(dir.filePath(QStringLiteral("warm.cube")));
        QVERIFY(cube.open(QIODevice::WriteOnly));
        cube.write("LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
        cube.close();
        f.page()->showPalette(ColorPage::Palette::Luts);
        LutBrowser* luts = f.page()->luts();
        luts->addFolder(dir.path());
        QTreeWidgetItem* found = nullptr;
        for (int r = 0; r < luts->tree()->topLevelItemCount(); ++r) {
            QTreeWidgetItem* root = luts->tree()->topLevelItem(r);
            for (int c = 0; c < root->childCount(); ++c) {
                if (root->child(c)->text(0) == QStringLiteral("warm.cube"))
                    found = root->child(c);
            }
        }
        QVERIFY(found);
        Q_EMIT luts->tree()->itemDoubleClicked(found, 0);
        ClipGrade g = f.grade(f.clips[0]);
        QCOMPARE(g.nodes.size(), 1);
        QCOMPARE(g.nodes[0].lutPath, cube.fileName());
        auto* remove = luts->findChild<QToolButton*>(QStringLiteral("LutRemove"));
        QVERIFY(remove->isEnabled());
        remove->click();
        QVERIFY(f.grade(f.clips[0]).nodes[0].lutPath.isEmpty());
        // The Key tab: the key output gain is the node's Mix.
        KeyPanel* key = f.page()->keyPanel();
        Q_EMIT key->gainField()->valueCommitted(0.4);
        QCOMPARE(f.grade(f.clips[0]).nodes[0].value(gk::Mix, 0), 0.4);
    }

    /// Writes a demo project for screenshots when KK_COLOR_DEMO_PROJECT names a file (the
    /// Color page screenshots in docs; skipped otherwise).
    void demoProject()
    {
        const QString path = qEnvironmentVariable("KK_COLOR_DEMO_PROJECT");
        if (path.isEmpty())
            QSKIP("KK_COLOR_DEMO_PROJECT not set");
        QString why;
        if (!test::ensureFixtures(&why))
            QSKIP(qPrintable(why));
        Project project;
        const ImportResult imported
            = project.importMedia({test::fixture("h264_25fps.mp4"), test::fixture("still.jpg"),
                test::fixture("rotated_portrait.mp4"), test::fixture("perf_1080p.mp4")});
        QVERIFY(imported.importedIds.size() >= 2);
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& tl = project.timeline()->mutableData();
        int64_t at = 0;
        QVector<quint64> ids;
        for (int round = 0; round < 3; ++round) {
            for (const QString& media : imported.importedIds) {
                ClipData clip;
                clip.kind = ClipKind::Media;
                clip.mediaId = media;
                clip.start = at;
                clip.duration = 40;
                at += 40;
                ids.append(editops::overwriteClip(tl, TrackRef {TrackType::Video, 0}, clip));
            }
        }
        project.endEdit();
        // Grades: a warm serial pair with a window, a parallel node, a qualifier.
        ClipGrade g;
        const quint64 a = gradeops::initialize(g);
        g.node(a)->params.animated(gk::GainR, 1.0).setValue(0, 1.12);
        g.node(a)->params.animated(gk::LiftB, 0.0).setValue(0, 0.03);
        const quint64 b = gradeops::addSerialNode(g, a);
        gradeops::addWindow(g, b, GradeWindowShape::Circle);
        g.node(b)->params.animated(gk::GammaY, 0.0).setValue(0, 0.05);
        const quint64 c = gradeops::addParallelNode(g, b);
        g.node(c)->qualifier.enabled = true;
        g.node(c)->qualifier.hueCenter = 0.08;
        g.node(c)->params.animated(gk::Saturation, 1.0).setValue(0, 1.3);
        QVERIFY(project.setClipGrade(project.activeTimelineUid(), ids[0], g, QStringLiteral("G")));
        QVERIFY(project.setClipGrade(project.activeTimelineUid(), ids[2], g, QStringLiteral("G")));
        QImage still(320, 180, QImage::Format_RGB32);
        still.fill(QColor(120, 90, 70));
        project.gallery()->addStill(still, g);
        project.gallery()->addStill(still, ClipGrade {});
        QVERIFY(project.save(path));
    }
};

QTEST_MAIN(TestColorPageUi)
#include "test_colorpageui.moc"
