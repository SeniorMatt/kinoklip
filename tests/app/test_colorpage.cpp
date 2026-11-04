// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen tests of the Color page basics (docs/behavior-spec.md 44b): Shift+6 opens it, the
// clip under the playhead is graded, the page keys (Alt+S / Alt+P / Alt+L, Ctrl+D, Shift+D,
// Ctrl+Alt+G) act only there and each change is one undo step, the wheels / fields / curves
// edit the selected node, the gallery grabs and applies stills, the scopes get the graded
// frame, and View > Video Scopes opens the scopes window on any page.
#include "app/MainWindow.h"
#include "app/pages/ColorPage.h"
#include "core/EditOps.h"
#include "core/GradeGallery.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/color/ColorBrowsers.h"
#include "ui/color/ColorNodeEditor.h"
#include "ui/color/ColorSession.h"
#include "ui/color/ColorWidgets.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/inspector/ValueField.h"
#include "ui/scopes/ScopeFeeder.h"
#include "ui/scopes/ScopeView.h"
#include "ui/scopes/ScopesWindow.h"

#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QListWidget>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QUndoStack>

#include <memory>

using namespace kk;
namespace gk = kk::gradekeys;

namespace {

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<PlaybackEngine> playback;
    std::unique_ptr<MainWindow> window;
    quint64 first = 0;
    quint64 second = 0;

    explicit Fixture(bool withEngine = false)
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        if (withEngine) {
            PlaybackEngine::Options options;
            options.noAudio = true;
            playback = std::make_unique<PlaybackEngine>(std::move(options));
            playback->setMediaPool(project.mediaPool());
            playback->setTimeline(project.timeline());
            ctx.playback = playback.get();
        }
        project.beginEdit(QStringLiteral("Add Clips"));
        TimelineData& tl = project.timeline()->mutableData();
        tl.width = 320;
        tl.height = 180;
        for (const auto& [start, colour, name] :
            {std::tuple {int64_t(0), QColor(200, 60, 40), QStringLiteral("Red")},
                std::tuple {int64_t(50), QColor(40, 90, 200), QStringLiteral("Blue")}}) {
            ClipData clip;
            clip.kind = ClipKind::Solid;
            clip.solidColor = colour;
            clip.name = name;
            clip.start = start;
            clip.duration = 50;
            const quint64 id = editops::overwriteClip(tl, TrackRef {TrackType::Video, 0}, clip);
            (start == 0 ? first : second) = id;
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
    int steps() const { return project.undoStack()->count(); }
    ClipGrade grade(quint64 clip) const
    {
        return project.clipGrade(project.activeTimelineUid(), clip);
    }

    void showPage()
    {
        window->resize(1600, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        action("view.page_color")->trigger();
        QCoreApplication::processEvents();
    }
    /// Key @p key (with @p mods) on the page's node editor (focus inside the page).
    void key(Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        page()->nodeEditor()->setFocus();
        QTest::keyClick(page()->nodeEditor(), key, mods);
        QCoreApplication::processEvents();
    }
};

} // namespace

class TestColorPage : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-test"));
        QCoreApplication::setApplicationName(QStringLiteral("test_colorpage"));
        QSettings().clear();
        initResources();
    }

    void pageOpensAndScopesCommands()
    {
        Fixture f;
        QVERIFY(f.action("view.page_color")->isEnabled());
        QVERIFY(!f.action("color.add_serial")->isEnabled()); // only on the Color page
        f.showPage();
        QCOMPARE(f.window->currentPageId(), QStringLiteral("color"));
        QVERIFY(f.action("color.add_serial")->isEnabled());
        // The Edit page's timeline commands rest while the Color page is shown (Ctrl+D is
        // Enable / Disable Current Node here, Change Clip Duration on the Edit page).
        QVERIFY(!f.action("clip.change_duration")->isEnabled());
        QVERIFY(f.action("edit.undo")->isEnabled());
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        QVERIFY(f.action("clip.change_duration")->isEnabled());
        QVERIFY(!f.action("color.bypass_node")->isEnabled());
    }

    void gradesTheClipUnderThePlayhead()
    {
        Fixture f;
        f.showPage();
        QCOMPARE(f.session()->clipId(), f.first);
        f.project.timeline()->setPlayhead(60);
        QCOMPARE(f.session()->clipId(), f.second);
        f.project.timeline()->setPlayhead(200); // past the clips
        QCOMPARE(f.session()->clipId(), quint64(0));
        QVERIFY(!f.session()->hasGrade());
        QCOMPARE(f.session()->addSerialNode(), quint64(0));
        f.project.timeline()->setPlayhead(10);
        QCOMPARE(f.session()->clipId(), f.first);
        QCOMPARE(f.session()->gradeFrame(), int64_t(10));
    }

    void pageKeysAreUndoSteps()
    {
        Fixture f;
        f.showPage();
        const int base = f.steps();
        // The ungraded clip shows node 01: Alt+S adds 02 after it (bughunt5, spec 44b.20).
        f.key(Qt::Key_S, Qt::AltModifier);
        QCOMPARE(f.grade(f.first).nodes.size(), 2);
        f.key(Qt::Key_S, Qt::AltModifier); // serial after it
        QCOMPARE(f.grade(f.first).nodes.size(), 3);
        QCOMPARE(f.steps(), base + 2);
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Add Serial Node"));
        f.key(Qt::Key_P, Qt::AltModifier);
        QCOMPARE(f.grade(f.first).nodes.size(), 5); // node + Parallel mixer
        f.key(Qt::Key_L, Qt::AltModifier);
        ClipGrade g = f.grade(f.first);
        int layers = 0;
        for (const GradeNode& n : g.nodes)
            layers += n.type == GradeNodeType::Layer ? 1 : 0;
        QCOMPARE(layers, 1);
        QCOMPARE(f.steps(), base + 4);
        // Ctrl+D bypasses the current node; Ctrl+Z brings it back.
        const quint64 current = f.session()->selectedNode();
        QVERIFY(g.node(current)->enabled);
        f.key(Qt::Key_D, Qt::ControlModifier);
        QVERIFY(!f.grade(f.first).node(current)->enabled);
        QCOMPARE(f.steps(), base + 5);
        QCOMPARE(f.grade(f.first).nodes.size(), g.nodes.size()); // not Change Clip Duration
        f.action("edit.undo")->trigger();
        QVERIFY(f.grade(f.first).node(current)->enabled);
        // Delete removes the current node (one step).
        const int count = int(f.grade(f.first).nodes.size());
        f.key(Qt::Key_Delete);
        QVERIFY(int(f.grade(f.first).nodes.size()) < count);
        f.action("edit.undo")->trigger();
        QCOMPARE(int(f.grade(f.first).nodes.size()), count);
        // On the Edit page the Color keys do nothing.
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        const int before = f.steps();
        QTest::keyClick(f.window.get(), Qt::Key_S, Qt::AltModifier);
        QCoreApplication::processEvents();
        QCOMPARE(f.steps(), before);
    }

    void primariesAndCurvesEdit()
    {
        Fixture f;
        f.showPage();
        const int base = f.steps();
        // A field on an ungraded clip creates the first node.
        ValueField* contrast = f.page()->primaries()->field(gk::Contrast);
        QVERIFY(contrast);
        Q_EMIT contrast->valueCommitted(1.25);
        ClipGrade g = f.grade(f.first);
        QCOMPARE(g.nodes.size(), 1);
        QCOMPARE(g.nodes[0].value(gk::Contrast, 0), 1.25);
        QCOMPARE(f.steps(), base + 1);
        // A wheel drag is one step, whatever the number of moves.
        ColorWheel* gain = f.page()->primaries()->wheel(gk::Wheel::Gain);
        gain->resize(140, 170);
        const QPointF centre = gain->discRect().center();
        QTest::mousePress(gain, Qt::LeftButton, {}, centre.toPoint());
        for (int i = 1; i <= 5; ++i)
            QTest::mouseMove(gain, (centre + QPointF(4 * i, -3 * i)).toPoint());
        QTest::mouseRelease(gain, Qt::LeftButton, {}, (centre + QPointF(20, -15)).toPoint());
        QCOMPARE(f.steps(), base + 2);
        g = f.grade(f.first);
        const double r = g.nodes[0].value(gk::GainR, 0), b = g.nodes[0].value(gk::GainB, 0);
        QVERIFY2(r > 1.0 || b > 1.0, qPrintable(QStringLiteral("%1 %2").arg(r).arg(b)));
        // The puck stays luma-neutral.
        const double gr = g.nodes[0].value(gk::GainG, 0);
        QVERIFY(std::abs(0.2126 * (r - 1) + 0.7152 * (gr - 1) + 0.0722 * (b - 1)) < 1e-6);
        // Round trip of the puck mapping.
        const QPointF puck = gain->puckFor({r - 1, gr - 1, b - 1});
        const auto back = gain->deltaFor(puck);
        QVERIFY(std::abs(back[0] - (r - 1)) < 1e-9 && std::abs(back[2] - (b - 1)) < 1e-9);
        // Curves: a click adds a point to the Y curve (one step).
        GradeCurveEditor* curves = f.page()->curves()->editor();
        curves->resize(300, 240);
        const QRectF plot = curves->plotRect();
        const QPoint at(
            int(plot.left() + plot.width() * 0.5), int(plot.bottom() - plot.height() * 0.7));
        QTest::mouseClick(curves, Qt::LeftButton, {}, at);
        g = f.grade(f.first);
        QCOMPARE(g.nodes[0].curves[GradeCurve::Luma].size(), 1);
        QVERIFY(std::abs(g.nodes[0].curves[GradeCurve::Luma][0].y - 0.7) < 0.02);
        QCOMPARE(f.steps(), base + 3);
        // A keyframed parameter gets a key at the playhead.
        ClipGrade animated = g;
        animated.nodes[0].params.values[gk::Saturation].setKey(0, 1.0);
        QVERIFY(f.session()->commit(animated, QStringLiteral("Key")));
        f.project.timeline()->setPlayhead(20);
        Q_EMIT f.page()->primaries()->field(gk::Saturation)->valueCommitted(0.5);
        const AnimatedValue& sat = f.grade(f.first).nodes[0].params.values[gk::Saturation];
        QCOMPARE(sat.keys.size(), 2);
        QCOMPARE(sat.valueAt(20), 0.5);
        QCOMPARE(sat.valueAt(0), 1.0);
    }

    void timelineGradeTarget()
    {
        Fixture f;
        f.showPage();
        f.action("color.timeline_grade")->trigger();
        QCOMPARE(f.session()->target(), ColorSession::Target::Timeline);
        QVERIFY(f.session()->hasGrade());
        f.key(Qt::Key_S, Qt::AltModifier); // 01 and 02 (spec 44b.20)
        QCOMPARE(f.project.timeline()->data().grade.nodes.size(), 2);
        QVERIFY(f.grade(f.first).isEmpty());
        f.action("color.timeline_grade")->trigger();
        QCOMPARE(f.session()->target(), ColorSession::Target::Clip);
    }

    void galleryAndScopesWithEngine()
    {
        Fixture f(true);
        f.showPage();
        // Shift+D bypasses every grade in the viewers (not a project change).
        const int base = f.steps();
        f.key(Qt::Key_D, Qt::ShiftModifier);
        QVERIFY(f.playback->bypassGrades());
        QVERIFY(f.action("color.bypass_all")->isChecked());
        QCOMPARE(f.steps(), base);
        f.key(Qt::Key_D, Qt::ShiftModifier);
        QVERIFY(!f.playback->bypassGrades());
        // Grade the red clip grey, grab a still (Ctrl+Alt+G), apply it to the blue clip.
        QVERIFY(f.session()->setParam(0, gk::Saturation, 0.0, QStringLiteral("Saturation")));
        f.key(Qt::Key_G, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(f.project.gallery()->stills().size(), 1);
        const GalleryStill& still = f.project.gallery()->stills()[0];
        QCOMPARE(still.grade, f.grade(f.first));
        QVERIFY(!still.image.isNull());
        // The still shows the graded (grey) picture.
        const QColor c = still.image.pixelColor(still.image.width() / 2, still.image.height() / 2);
        QVERIFY2(std::abs(c.red() - c.blue()) < 8, qPrintable(c.name()));
        QCOMPARE(f.page()->gallery()->list()->count(), 1);
        const int steps = f.steps();
        QCOMPARE(f.session()->applyStill(still.id, {f.second}), 1);
        QCOMPARE(f.grade(f.second), f.grade(f.first));
        QCOMPARE(f.steps(), steps + 1);
        // Copy a grade between clips (reset the blue one first).
        QVERIFY(f.project.setClipGrade(
            f.project.activeTimelineUid(), f.second, ClipGrade {}, QStringLiteral("Reset")));
        QCOMPARE(f.session()->copyGrade(f.first, {f.second}), 1);
        QCOMPARE(f.grade(f.second), f.grade(f.first));
        // The scopes receive the graded frame: a grey picture has every vectorscope count in
        // the centre area.
        ScopeFeeder* feeder = f.page()->scopeFeeder();
        QVERIFY(feeder->isActive());
        QSignalSpy images(feeder, &ScopeFeeder::imageReady);
        feeder->refresh();
        QVERIFY(images.wait(5000) || !feeder->lastImage().isNull());
        const QImage frame = feeder->lastImage();
        QVERIFY(!frame.isNull());
        const scopes::VectorscopeData v = scopes::vectorscope(frame, 64);
        quint64 total = 0, centre = 0;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                total += v.at(x, y);
                if (std::abs(x - 31.5) < 3 && std::abs(y - 31.5) < 3)
                    centre += v.at(x, y);
            }
        QVERIFY(total > 0);
        QVERIFY2(centre * 10 >= total * 9,
            qPrintable(QStringLiteral("%1 of %2").arg(centre).arg(total)));
    }

    void videoScopesWindow()
    {
        Fixture f(true);
        f.window->resize(1600, 900);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        QAction* scopes = f.action("view.video_scopes");
        QVERIFY(scopes->isEnabled());
        scopes->trigger(); // on the Edit page
        QVERIFY(f.window->scopesWindow());
        QVERIFY(f.window->scopesWindow()->isVisible());
        QVERIFY(f.window->scopesWindow()->feeder()->isActive());
        f.window->scopesWindow()->close();
        QVERIFY(!scopes->isChecked());
        QVERIFY(!f.window->scopesWindow()->feeder()->isActive());
    }
};

QTEST_MAIN(TestColorPage)
#include "test_colorpage.moc"
