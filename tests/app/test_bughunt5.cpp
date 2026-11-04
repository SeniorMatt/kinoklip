// SPDX-License-Identifier: GPL-3.0-or-later
// Fifth bug-hunting round (mostly the Color page), in the real main window where it matters:
// one test per fix, named after the spec row it covers.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/ColorPage.h"
#include "app/pages/CutPage.h"
#include "core/EditOps.h"
#include "core/GradeGallery.h"
#include "core/KeyframeOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "platform/FileDialogs.h"
#include "playback/PlaybackEngine.h"
#include "ui/color/ColorBrowsers.h"
#include "ui/color/ColorFrameSource.h"
#include "ui/color/ColorNodeEditor.h"
#include "ui/color/ColorPalettes.h"
#include "ui/color/ColorSession.h"
#include "ui/color/ColorWidgets.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/cut/CutToolBar.h"
#include "ui/dialogs/KeyboardCustomizationDialog.h"
#include "ui/inspector/ValueField.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/timeline/TimelineDialogs.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QUndoStack>

#include <memory>

using namespace kk;
namespace gk = kk::gradekeys;

namespace {

/// Everything main() creates, with @p clipCount solid clips of 50 frames on V1 (and an engine
/// when asked).
struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<PlaybackEngine> playback;
    std::unique_ptr<MainWindow> window;
    QVector<quint64> clips;

    explicit Fixture(int clipCount = 3, bool withEngine = false)
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
        const QColor colours[] = {QColor(200, 60, 40), QColor(40, 90, 200), QColor(60, 180, 70)};
        for (int i = 0; i < clipCount; ++i) {
            ClipData clip;
            clip.kind = ClipKind::Solid;
            clip.solidColor = colours[i % 3];
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
    int steps() const { return project.undoStack()->index(); }
    ClipGrade grade(quint64 clip) const
    {
        return project.clipGrade(project.activeTimelineUid(), clip);
    }
    void showPage(const char* pageAction = "view.page_color")
    {
        window->resize(1600, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        action(pageAction)->trigger();
        QCoreApplication::processEvents();
    }
    void key(Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        page()->nodeEditor()->setFocus();
        QTest::keyClick(page()->nodeEditor(), key, mods);
        QCoreApplication::processEvents();
    }
};

/// Names of the nodes of @p g in list order (gradeNodeName()).
QStringList nodeNames(const ClipGrade& g)
{
    QStringList names;
    for (int i = 0; i < g.nodes.size(); ++i)
        names.append(gradeNodeName(g, i));
    return names;
}

int correctors(const ClipGrade& g)
{
    return int(std::count_if(
        g.nodes.begin(), g.nodes.end(), [](const GradeNode& n) { return !n.isMixer(); }));
}

/// Waits until @p pred holds (or @p ms pass).
template<typename Pred>
bool waitFor(Pred pred, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < ms)
        QTest::qWait(20);
    return pred();
}

} // namespace

class TestBughunt5 : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_bughunt5"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }
    void init() { QSettings().clear(); }

    // Spec 44b.21: only correctors are numbered (Resolve): Parallel and Layer mixers do not use
    // up a number, so the node after a parallel mixer is 03, not 04; the keyframe editors' groups
    // follow ("Color 03").
    void spec44b_19_mixersAreNotNumbered()
    {
        ClipGrade g;
        const quint64 first = gradeops::initialize(g);
        const quint64 parallel = gradeops::addParallelNode(g, first);
        QCOMPARE(
            nodeNames(g), (QStringList {QStringLiteral("01"), QStringLiteral("02"), QString()}));
        const quint64 mixer = g.output;
        QVERIFY(g.node(mixer)->isMixer());
        const quint64 third = gradeops::addSerialNode(g, mixer);
        QCOMPARE(gradeNodeName(g, g.indexOf(third)), QStringLiteral("03"));
        QCOMPARE(gradeNodeName(g, g.indexOf(parallel)), QStringLiteral("02"));
        // A label wins, also on a mixer.
        g.node(mixer)->label = QStringLiteral("Mix");
        QCOMPARE(gradeNodeName(g, g.indexOf(mixer)), QStringLiteral("Mix"));
        // Deleting a corrector renumbers the later ones (index based, as before).
        QVERIFY(gradeops::removeNode(g, parallel));
        QCOMPARE(gradeNodeName(g, g.indexOf(third)), QStringLiteral("02"));
    }

    // Spec 44b.22: an ungraded clip shows node 01 as in Resolve, so Alt+S gives 01 -> 02,
    // Alt+P 01 and 02 under a Parallel mixer, Alt+L the same with a Layer mixer and Shift+S a
    // node before 01; each one undo step. A qualifier pick on an ungraded clip makes node 01
    // only.
    void spec44b_20_ungradedClipHasNodeOne()
    {
        Fixture f;
        f.showPage();
        QVERIFY(f.grade(f.clips[0]).isEmpty());
        int base = f.steps();
        f.key(Qt::Key_S, Qt::AltModifier);
        ClipGrade g = f.grade(f.clips[0]);
        QCOMPARE(correctors(g), 2);
        QCOMPARE(f.steps(), base + 1);
        QCOMPARE(gradeNodeName(g, g.indexOf(f.session()->selectedNode())), QStringLiteral("02"));
        // 01 feeds 02 feeds the output.
        const GradeNode* two = g.node(f.session()->selectedNode());
        QCOMPARE(g.output, two->id);
        QVERIFY(two->inputs.value(0) != 0);
        f.project.undoStack()->undo();
        QVERIFY(f.grade(f.clips[0]).isEmpty());

        f.key(Qt::Key_P, Qt::AltModifier);
        g = f.grade(f.clips[0]);
        QCOMPARE(correctors(g), 2);
        QCOMPARE(g.node(g.output)->type, GradeNodeType::Parallel);
        f.project.undoStack()->undo();

        f.key(Qt::Key_L, Qt::AltModifier);
        g = f.grade(f.clips[0]);
        QCOMPARE(correctors(g), 2);
        QCOMPARE(g.node(g.output)->type, GradeNodeType::Layer);
        f.project.undoStack()->undo();

        base = f.steps();
        f.key(Qt::Key_S, Qt::ShiftModifier);
        g = f.grade(f.clips[0]);
        QCOMPARE(correctors(g), 2);
        QCOMPARE(f.steps(), base + 1);
        // The new node (02) comes before 01.
        const GradeNode* out = g.node(g.output);
        QCOMPARE(gradeNodeName(g, g.indexOf(out->id)), QStringLiteral("01"));
        QCOMPARE(out->inputs.value(0), f.session()->selectedNode());
        f.project.undoStack()->undo();

        // A qualifier pick on the ungraded clip: node 01 only.
        QImage img(320, 180, QImage::Format_ARGB32_Premultiplied);
        img.fill(QColor(220, 40, 40));
        f.page()->frames()->setInput(img);
        f.page()->pickQualifier(QRectF(0.5, 0.5, 0, 0));
        g = f.grade(f.clips[0]);
        QCOMPARE(g.nodes.size(), 1);
        QVERIFY(g.nodes[0].qualifier.enabled);
    }

    // Spec 44b.23: the clips strip selection follows the graded clip: after moving the playhead
    // to a clip outside the selection, Ctrl+V pastes to the clip on screen, not to the clip
    // selected before.
    void spec44b_21_pasteGoesToTheClipOnScreen()
    {
        Fixture f;
        f.showPage();
        ColorClipStrip* strip = f.page()->clipStrip();
        QCOMPARE(strip->count(), 3);
        // Grade clip 1, copy it.
        QVERIFY(f.session()->setParam(0, gk::Saturation, 0.0, QStringLiteral("Sat")));
        const ClipGrade copied = f.grade(f.clips[0]);
        f.key(Qt::Key_C, Qt::ControlModifier);
        // Click clip 2 in the strip (selects it), then move the playhead to clip 3.
        QTest::mouseClick(
            strip->viewport(), Qt::LeftButton, {}, strip->visualItemRect(strip->item(1)).center());
        QCOMPARE(f.session()->clipId(), f.clips[1]);
        QCOMPARE(f.session()->selectedClips(), QVector<quint64> {f.clips[1]});
        f.project.timeline()->setPlayhead(110);
        QCoreApplication::processEvents();
        QCOMPARE(f.session()->clipId(), f.clips[2]);
        QVERIFY(f.session()->selectedClips().isEmpty());
        f.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(f.grade(f.clips[2]), copied);
        QVERIFY(f.grade(f.clips[1]).isEmpty());
        // A Ctrl-click selection stays while the graded clip is in it... and paste goes there.
        QTest::mouseClick(strip->viewport(), Qt::LeftButton, Qt::ControlModifier,
            strip->visualItemRect(strip->item(1)).center());
        QVERIFY(f.session()->selectedClips().contains(f.clips[1]));
        f.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(f.grade(f.clips[1]), copied);
    }

    // Spec 44b.24: a wheel drag keeps a luma part typed into the wheel's R, G, B fields (the
    // puck only shows and changes the colour part); the reset arrow at the wheel's top right
    // resets it in one step.
    void spec44b_22_wheelDragKeepsLumaAndResetArrow()
    {
        Fixture f;
        f.showPage();
        // Lift R typed as +0.1: luma goes up a little.
        QVERIFY(f.session()->setParam(0, gk::LiftR, 0.1, QStringLiteral("Lift R")));
        ColorWheel* wheel = f.page()->primaries()->wheel(gk::Wheel::Lift);
        QCoreApplication::processEvents();
        const QPointF c = wheel->discRect().center();
        QTest::mousePress(wheel, Qt::LeftButton, {}, c.toPoint());
        QTest::mouseMove(wheel, (c + QPointF(1, 0)).toPoint());
        QTest::mouseMove(wheel, (c + QPointF(2, 0)).toPoint());
        QTest::mouseRelease(wheel, Qt::LeftButton, {}, (c + QPointF(2, 0)).toPoint());
        const GradeNode& n = f.grade(f.clips[0]).nodes.first();
        auto luma = [&](const GradeNode& node) {
            return 0.2126 * node.value(gk::LiftR, 0) + 0.7152 * node.value(gk::LiftG, 0)
                + 0.0722 * node.value(gk::LiftB, 0);
        };
        QVERIFY2(std::abs(luma(n) - 0.2126 * 0.1) < 1e-3, qPrintable(QString::number(luma(n))));
        QVERIFY(n.value(gk::LiftB, 0) != 0.0); // the drag moved the colour

        // The reset arrow.
        const int base = f.steps();
        QTest::mouseClick(wheel, Qt::LeftButton, {}, wheel->resetRect().center().toPoint());
        const GradeNode& reset = f.grade(f.clips[0]).nodes.first();
        for (int ch = 0; ch < 4; ++ch)
            QCOMPARE(reset.value(gk::wheelKey(gk::Wheel::Lift, ch), 0), 0.0);
        QCOMPARE(f.steps(), base + 1);
    }

    // Spec 44b.28: the wheel drag sensitivity does not depend on the wheel's size: a full
    // puck throw takes at least 200 px of mouse travel, the master strip's range 400 px (on a
    // 1366 x 768 screen the wheels are small and a 30 px drag used to swing the gain by 0.7).
    void spec44b_26_wheelSensitivity()
    {
        Fixture f;
        f.showPage();
        ColorWheel* wheel = f.page()->primaries()->wheel(gk::Wheel::Gain);
        QVERIFY(wheel->puckThrowPx() >= 200.0);
        QVERIFY(wheel->masterThrowPx() >= 400.0);
        const QPointF c = wheel->discRect().center();
        QTest::mousePress(wheel, Qt::LeftButton, {}, c.toPoint());
        QTest::mouseMove(wheel, (c + QPointF(10, 0)).toPoint());
        QTest::mouseMove(wheel, (c + QPointF(20, 0)).toPoint());
        QTest::mouseRelease(wheel, Qt::LeftButton, {}, (c + QPointF(20, 0)).toPoint());
        const GradeNode& n = f.grade(f.clips[0]).nodes.first();
        const double expected = 1.0 + wheel->deltaFor(QPointF(20.0 / wheel->puckThrowPx(), 0))[2];
        QVERIFY2(std::abs(n.value(gk::GainB, 0) - expected) < 1e-6,
            qPrintable(QString::number(n.value(gk::GainB, 0))));
        QVERIFY(n.value(gk::GainB, 0) - 1.0 < 0.1); // 20 px is a small change
        // The master strip: 40 px is a tenth of the range.
        const QPointF s = wheel->stripRect().center();
        QTest::mousePress(wheel, Qt::LeftButton, {}, s.toPoint());
        QTest::mouseMove(wheel, (s + QPointF(20, 0)).toPoint());
        QTest::mouseMove(wheel, (s + QPointF(40, 0)).toPoint());
        QTest::mouseRelease(wheel, Qt::LeftButton, {}, (s + QPointF(40, 0)).toPoint());
        const double gainY = f.grade(f.clips[0]).nodes.first().value(gk::GainY, 0);
        QVERIFY2(
            std::abs(gainY - (1.0 + 40.0 / wheel->masterThrowPx() * 2 * wheel->range())) < 1e-6,
            qPrintable(QString::number(gainY)));
    }

    // Spec 44b.29: the Color page's keys work right after switching to it with the keyboard
    // (Shift+6, then Alt+S), without clicking into the page first: the node editor takes the
    // focus when the page shows.
    void spec44b_27_pageKeysWorkAfterSwitchingPages()
    {
        Fixture f;
        f.window->resize(1600, 900);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        // Focus on an Edit page widget (the timeline).
        QWidget* edit = f.window->findChild<TimelineWidget*>();
        QVERIFY(edit);
        edit->setFocus();
        QCoreApplication::processEvents();
        f.action("view.page_color")->trigger();
        QTest::qWait(20);
        QWidget* focus = QApplication::focusWidget();
        QVERIFY(focus);
        QVERIFY2(f.page()->isAncestorOf(focus), focus->metaObject()->className());
        QTest::keyClick(focus, Qt::Key_S, Qt::AltModifier);
        QCoreApplication::processEvents();
        QCOMPARE(f.grade(f.clips[0]).nodes.size(), 2);
    }

    // Spec 44b.25: Color > Add / Remove Keyframe keys the current node at the playhead (all of
    // its values, as a Resolve corrector keyframe), so later edits at other frames add keys and
    // the grade animates; the palettes show the values at the playhead when it moves over the
    // keys; the command again removes the keys there. One undo step each.
    void spec44b_23_keyframeTheCurrentNode()
    {
        Fixture f;
        f.showPage();
        QAction* key = f.action("color.keyframe");
        QVERIFY(key->isEnabled());
        int base = f.steps();
        key->trigger();
        QCOMPARE(f.steps(), base + 1);
        ClipGrade g = f.grade(f.clips[0]);
        QCOMPARE(g.nodes.size(), 1);
        QVERIFY(g.isAnimated());
        QVERIFY(f.session()->hasKeyframe());
        // Saturation 0 at frame 20: a second key, frame 0 keeps 1.
        f.project.timeline()->setPlayhead(20);
        QCoreApplication::processEvents();
        QVERIFY(f.session()->setParam(0, gk::Saturation, 0.0, QStringLiteral("Sat")));
        g = f.grade(f.clips[0]);
        QCOMPARE(g.nodes[0].value(gk::Saturation, 0), 1.0);
        QCOMPARE(g.nodes[0].value(gk::Saturation, 20), 0.0);
        QVERIFY(g.nodes[0].value(gk::Saturation, 10) > 0.0);
        QVERIFY(g.nodes[0].value(gk::Saturation, 10) < 1.0);
        // The keyframe editors list the node.
        bool listed = false;
        for (const auto& p : keyframeops::animatedParams(
                 *editops::clipById(f.project.timeline()->data(), f.clips[0])))
            listed = listed || p.groupName == QStringLiteral("Color 01");
        QVERIFY(listed);
        // The Sat field follows the playhead.
        ValueField* sat = f.page()->primaries()->field(gk::Saturation);
        QVERIFY(sat);
        QCOMPARE(sat->value(), 0.0);
        f.project.timeline()->setPlayhead(0);
        QCoreApplication::processEvents();
        QCOMPARE(sat->value(), 1.0);
        // Remove the key at frame 0.
        base = f.steps();
        key->trigger();
        QCOMPARE(f.steps(), base + 1);
        QVERIFY(!f.session()->hasKeyframe());
        QCOMPARE(f.grade(f.clips[0]).nodes[0].value(gk::Saturation, 0), 0.0);
    }

    // Spec 44b.26: the clips strip thumbnails stay graded while Shift+D bypasses the grades in
    // the viewers, and the timeline grade's pictures (node thumbnails, highlight, qualifier
    // picks) start from the graded clips, not the ungraded ones.
    void spec44b_24_thumbnailsAndTimelineInput()
    {
        Fixture f(1, true);
        f.showPage();
        QCOMPARE(f.page()->frames()->inputGrades(), PlaybackEngine::ImageGrades::None);
        // Grade the clip grey, bypass all, then let a thumbnail render.
        f.key(Qt::Key_D, Qt::ShiftModifier);
        QVERIFY(f.playback->bypassGrades());
        QVERIFY(f.session()->setParam(0, gk::Saturation, 0.0, QStringLiteral("Sat")));
        ColorFrameSource* frames = f.page()->frames();
        QSignalSpy thumbs(frames, &ColorFrameSource::thumbnailChanged);
        frames->refreshThumbnails();
        QVERIFY(waitFor([&] { return !thumbs.isEmpty(); }));
        const QImage t = frames->thumbnail(f.clips[0]);
        QVERIFY(!t.isNull());
        const QColor c = t.pixelColor(t.width() / 2, t.height() / 2);
        QVERIFY2(std::abs(c.red() - c.blue()) < 8, qPrintable(c.name()));
        f.key(Qt::Key_D, Qt::ShiftModifier);

        // Timeline grade: its input picture carries the clip grade (grey), not the red clip.
        f.session()->setTarget(ColorSession::Target::Timeline);
        QCOMPARE(frames->inputGrades(), PlaybackEngine::ImageGrades::ClipsOnly);
        QVERIFY(f.session()->setParam(0, gk::Contrast, 1.2, QStringLiteral("Contrast")));
        QSignalSpy inputs(frames, &ColorFrameSource::inputChanged);
        frames->refreshInput();
        QVERIFY(waitFor([&] { return !inputs.isEmpty(); }));
        const QImage in = frames->input();
        QVERIFY(!in.isNull());
        const QColor ic = in.pixelColor(in.width() / 2, in.height() / 2);
        QVERIFY2(std::abs(ic.red() - ic.blue()) < 8, qPrintable(ic.name()));
    }

    // Spec 23.11: tooltips that name a key follow Keyboard Customization: the Cut page's
    // Dissolve and edit buttons, the viewer buttons and the Color page's Highlight, Wipe and
    // Bypass buttons.
    void spec23_11_toolTipsFollowTheKeymap()
    {
        Fixture f;
        f.showPage("view.page_cut");
        auto* dissolve = f.window->findChild<QToolButton*>(QStringLiteral("CutDissolveButton"));
        QVERIFY(dissolve);
        const QString id = QStringLiteral("timeline.add_transition");
        f.actions.setShortcuts(id, {QKeySequence(QStringLiteral("Ctrl+Alt+F9"))});
        QVERIFY2(
            dissolve->toolTip().contains(
                QKeySequence(QStringLiteral("Ctrl+Alt+F9")).toString(QKeySequence::NativeText)),
            qPrintable(dissolve->toolTip()));
        f.actions.setShortcuts(id, {});
        QCOMPARE(dissolve->toolTip(), QStringLiteral("Dissolve"));
        // An edit button of the Cut page tool bar.
        auto* split = f.window->findChild<QToolButton*>(QStringLiteral("CutButton_edit.split"));
        QVERIFY(split);
        f.actions.setShortcuts(QStringLiteral("edit.split"), {QKeySequence(QStringLiteral("F7"))});
        QVERIFY2(split->toolTip().contains(QStringLiteral("F7")), qPrintable(split->toolTip()));
        // The Color page buttons.
        f.action("view.page_color")->trigger();
        auto* highlight = f.window->findChild<QToolButton*>(QStringLiteral("ColorHighlightButton"));
        QVERIFY(highlight);
        QVERIFY(highlight->toolTip().contains(QStringLiteral("Shift+H")));
        f.actions.setShortcuts(
            QStringLiteral("color.highlight"), {QKeySequence(QStringLiteral("F8"))});
        QVERIFY2(
            highlight->toolTip().contains(QStringLiteral("F8")), qPrintable(highlight->toolTip()));
        QVERIFY(!highlight->toolTip().contains(QStringLiteral("Shift+H")));
        auto* bypass = f.window->findChild<QToolButton*>(QStringLiteral("ColorBypassButton"));
        f.actions.setShortcuts(
            QStringLiteral("color.bypass_all"), {QKeySequence(QStringLiteral("F6"))});
        QVERIFY2(bypass->toolTip().contains(QStringLiteral("F6")), qPrintable(bypass->toolTip()));
    }

    // Spec 23.12: Cancel in Keyboard Customization also takes back the profiles imported or
    // duplicated while it was open (and renames), not only the active profile and its keys.
    void spec23_12_cancelRemovesProfilesMadeInTheSession()
    {
        QTemporaryDir dir;
        ActionRegistry actions;
        actions.setUserProfileDirectory(dir.filePath(QStringLiteral("profiles")));
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        const QString original = actions.activeProfile();
        const int before = int(actions.profiles().size());
        const QString exported = dir.filePath(QStringLiteral("mine.json"));
        QVERIFY(actions.exportProfile(original, exported));

        KeyboardCustomizationDialog dialog(actions);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        // Import.
        platform::setFileDialogHandlerForTests(
            [exported](const platform::FileDialogRequest&) { return QStringList {exported}; });
        QPushButton* import = nullptr;
        for (QPushButton* b : dialog.findChildren<QPushButton*>()) {
            if (b->text().startsWith(QStringLiteral("Import")))
                import = b;
        }
        QVERIFY(import);
        import->click();
        platform::setFileDialogHandlerForTests({});
        QCOMPARE(int(actions.profiles().size()), before + 1);
        // Duplicate (answer the name dialog).
        QTimer::singleShot(0, qApp, [] {
            if (auto* input = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                input->setTextValue(QStringLiteral("Copy for test"));
                input->accept();
            }
        });
        QAction* duplicate = nullptr;
        for (QMenu* m : dialog.findChildren<QMenu*>()) {
            for (QAction* a : m->actions()) {
                if (a->text().startsWith(QStringLiteral("Duplicate")))
                    duplicate = a;
            }
        }
        QVERIFY(duplicate);
        duplicate->trigger();
        QCOMPARE(int(actions.profiles().size()), before + 2);
        QVERIFY(actions.activeProfile() != original);

        dialog.reject();
        QCOMPARE(actions.activeProfile(), original);
        QCOMPARE(int(actions.profiles().size()), before);
        QVERIFY(QDir(dir.filePath(QStringLiteral("profiles")))
                    .entryList({QStringLiteral("*.json")}, QDir::Files)
                    .isEmpty());
    }

    // Spec 33.35: Relink finds a file whose name differs only in case ("CLIP.MP4" for
    // "clip.mp4"; a copy from a camera card, or a case-insensitive file system); a match with
    // the same case wins. Replace Selected Clip treats a name that differs from the file name
    // only in case as the file name.
    void spec33_35_relinkIgnoresCase()
    {
        const QString source = QStringLiteral(KK_TEST_DATA_DIR "/generated/h264_25fps.mp4");
        if (!QFileInfo::exists(source))
            QSKIP("generated test media missing");
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("card")));
        const QString upper = dir.filePath(QStringLiteral("card/TAKE ONE.MP4"));
        QVERIFY(QFile::copy(source, upper));
        Project project;
        MediaItem m;
        m.name = QStringLiteral("take one.mp4");
        m.path = QStringLiteral("/nonexistent/take one.mp4");
        m.info.valid = true;
        m.info.hasVideo = true;
        const QString id = project.mediaPool()->addItem(m);
        QVERIFY(project.mediaPool()->isOffline(id));
        QCOMPARE(project.mediaPool()->relinkFromFolder(dir.path()), 1);
        QCOMPARE(project.mediaPool()->item(id)->path, QFileInfo(upper).absoluteFilePath());

        // The same case wins over another case.
        const QString exact = dir.filePath(QStringLiteral("card/deep/take two.mp4"));
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("card/deep")));
        QVERIFY(QFile::copy(source, exact));
        QVERIFY(QFile::copy(source, dir.filePath(QStringLiteral("TAKE TWO.MP4"))));
        MediaItem m2 = m;
        m2.name = QStringLiteral("take two.mp4");
        m2.path = QStringLiteral("/nonexistent/take two.mp4");
        const QString id2 = project.mediaPool()->addItem(m2);
        QCOMPARE(project.mediaPool()->relinkFromFolder(dir.path(), {id2}), 1);
        QCOMPARE(project.mediaPool()->item(id2)->path, QFileInfo(exact).absoluteFilePath());

        // Replace Selected Clip: "take one.mp4" shown for "TAKE ONE.MP4" follows the new file.
        SelectionModel selection;
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        AppContext ctx;
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        MainWindow window(ctx, options);
        const QString other = dir.filePath(QStringLiteral("new name.mp4"));
        QVERIFY(QFile::copy(source, other));
        QString error;
        QVERIFY2(
            window.findChild<MediaPoolPanel*>()->replaceClip(id, other, &error), qPrintable(error));
        QCOMPARE(project.mediaPool()->item(id)->name, QStringLiteral("new name.mp4"));
    }

    // Spec 25.17: Timeline Settings offers Resolve's resolution presets (1280 x 720 HD,
    // 1920 x 1080 HD, 3840 x 2160 Ultra HD, vertical and square ones, Custom) like Project
    // Settings; a preset sets and locks the sizes, Custom unlocks them.
    void spec25_17_timelineSettingsResolutionPresets()
    {
        Project project;
        const quint64 uid = project.activeTimelineUid();
        QVERIFY(project.setTimelineFormat(uid, false, 1920, 1080, Rational {24, 1}));
        QStringList items;
        QString selected;
        bool widthEnabled = true;
        QTimer::singleShot(100, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto* presets = dialog->findChild<QComboBox*>(QStringLiteral("TimelineResolution"));
            auto* width = dialog->findChild<QSpinBox*>(QStringLiteral("TimelineWidth"));
            if (!presets || !width) {
                dialog->reject();
                return;
            }
            for (int i = 0; i < presets->count(); ++i)
                items.append(presets->itemText(i));
            selected = presets->currentText();
            widthEnabled = width->isEnabled();
            presets->setCurrentIndex(presets->findText(QStringLiteral("1080 x 1920 Vertical HD")));
            dialog->accept();
        });
        timelinedialogs::editTimelineSettings(nullptr, project, uid);
        QCOMPARE(selected, QStringLiteral("1920 x 1080 HD"));
        QVERIFY(!widthEnabled);
        for (const char* name : {"1280 x 720 HD", "1920 x 1080 HD", "3840 x 2160 Ultra HD",
                 "1080 x 1920 Vertical HD", "1080 x 1080 Square", "Custom"})
            QVERIFY2(items.contains(QLatin1String(name)), name);
        const auto data = project.timelineData(uid);
        QCOMPARE(data->width, 1080);
        QCOMPARE(data->height, 1920);

        // An odd size shows Custom with the sizes unlocked.
        QVERIFY(project.setTimelineFormat(uid, false, 1000, 500, Rational {24, 1}));
        QTimer::singleShot(100, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            selected = dialog->findChild<QComboBox*>(QStringLiteral("TimelineResolution"))
                           ->currentText();
            widthEnabled
                = dialog->findChild<QSpinBox*>(QStringLiteral("TimelineWidth"))->isEnabled();
            dialog->reject();
        });
        timelinedialogs::editTimelineSettings(nullptr, project, uid);
        QCOMPARE(selected, QStringLiteral("Custom"));
        QVERIFY(widthEnabled);
    }
};

QTEST_MAIN(TestBughunt5)
#include "test_bughunt5.moc"
