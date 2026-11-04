// SPDX-License-Identifier: GPL-3.0-or-later
// Seventh bug-hunting round, in the real main window where it matters: one test per fix, named
// after the spec row it covers.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "core/DynamicZoom.h"
#include "core/EditOps.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <QToolBar>
#include <QUndoStack>

#include <memory>

using namespace kk;

namespace {

/// Everything main() creates, with @p clipCount clips of 50 frames on V1 (Solid, or Media
/// with 20 s of material when @p media).
struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    QVector<quint64> clips;

    explicit Fixture(int clipCount = 2, bool media = false)
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
        for (int i = 0; i < clipCount; ++i) {
            ClipData clip;
            clip.kind = media ? ClipKind::Media : ClipKind::Solid;
            clip.solidColor = QColor(40 + 60 * i, 90, 200);
            clip.name = QStringLiteral("Clip %1").arg(i + 1);
            clip.start = i * 50;
            clip.duration = 50;
            clip.sourceDurationUs = 20'000'000;
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
    const ClipData* clip(int i) const
    {
        return editops::clipById(project.timeline()->data(), clips[i]);
    }
    InspectorPanel* inspector() const { return window->editPage()->inspector(); }
    ParamGroupBox* group(const char* name) const
    {
        return inspector()->findChild<ParamGroupBox*>(QLatin1String(name));
    }
    void show()
    {
        window->resize(1600, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }
};

} // namespace

class TestBughunt7 : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_bughunt7"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }
    void init() { QSettings().clear(); }

    // Spec 8.23: the timeline tool bar has a Dynamic Trim Mode button between Trim and Blade,
    // with a Breeze icon (no own icon art), checked while dynamic trim is on.
    void spec8_23_dynamicTrimButtonBetweenTrimAndBlade()
    {
        Fixture f;
        QToolBar* bar = f.window->editPage()->timeline()->toolBar();
        const QList<QAction*> list = bar->actions();
        const qsizetype trim = list.indexOf(f.action("tool.trim"));
        const qsizetype dynamic = list.indexOf(f.action("trim.dynamic"));
        const qsizetype blade = list.indexOf(f.action("tool.blade"));
        QVERIFY(trim >= 0 && dynamic == trim + 1 && blade == dynamic + 1);
        if (QIcon::hasThemeIcon(QStringLiteral("chronometer-start")))
            QVERIFY(!f.action("trim.dynamic")->icon().isNull());
        f.action("trim.dynamic")->trigger();
        QVERIFY(f.action("trim.dynamic")->isChecked());
        f.action("trim.dynamic")->trigger();
        QVERIFY(!f.action("trim.dynamic")->isChecked());
    }

    // Spec 37.36: the Audio page's timeline starts with the (collapsed) video tracks at the
    // top and the audio tracks right below them, with no empty band above.
    void spec37_36_audioPageTracksStartAtTheTop()
    {
        Fixture f;
        f.show();
        f.action("view.page_audio")->trigger();
        QCoreApplication::processEvents();
        auto* audio = f.window->findChild<TimelineWidget*>(QStringLiteral("AudioTimeline"));
        QVERIFY(audio && audio->isVisible());
        const TimelineLayout& L = audio->layout();
        QVERIFY(L.contentHeight() < L.rowAreaHeight()); // everything fits: no scrolling
        QCOMPARE(L.verticalOffset(), 0);
        const int topVideo = L.rowOf({TrackType::Video, 0});
        QCOMPARE(L.rowRect(0).top(), timelinemetrics::kRulerHeight);
        QVERIFY(L.rowRect(topVideo).height() <= 2 * timelinemetrics::kMinTrackHeight);
        const QRect a1 = L.rowRect(L.rowOf({TrackType::Audio, 0}));
        QVERIFY2(
            a1.top() < timelinemetrics::kRulerHeight + 80, qPrintable(QString::number(a1.top())));
        // The Edit page keeps the V/A divider near the middle.
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        auto* edit = f.window->editPage()->timeline()->findChild<TimelineWidget*>();
        QVERIFY(edit->layout().verticalOffset() > 0);
    }

    // Spec 24.20: the Inspector's Video tab has a Dynamic Zoom section between Cropping and
    // Composite: an on/off switch, the ease and Swap; each change is one undo step.
    void spec24_20_inspectorDynamicZoom()
    {
        Fixture f;
        f.show();
        f.selection.setClips({f.clips[0]});
        QCoreApplication::processEvents();
        ParamGroupBox* dz = f.group("DynamicZoomGroup");
        QVERIFY(dz);
        const QVector<ParamGroupBox*>& groups = f.inspector()->groups();
        QStringList titles;
        for (ParamGroupBox* g : groups)
            titles << g->title();
        const qsizetype at = titles.indexOf(QStringLiteral("Dynamic Zoom"));
        QVERIFY2(at > 0 && titles[at - 1] == QStringLiteral("Cropping")
                && titles[at + 1] == QStringLiteral("Composite"),
            qPrintable(titles.join('|')));
        auto* enable = dz->findChild<QCheckBox*>(QStringLiteral("DynamicZoomEnable"));
        QVERIFY(enable);
        QVERIFY(!enable->isChecked());
        QUndoStack* undo = f.project.undoStack();
        const int before = undo->index();
        enable->click();
        QVERIFY(dynamiczoom::isEnabled(f.clip(0)->transform));
        QCOMPARE(undo->index(), before + 1);
        // The default is a slow push-in.
        QVERIFY(dynamiczoom::at(*f.clip(0), 49).size < dynamiczoom::at(*f.clip(0), 0).size);
        auto* ease = dz->findChild<QComboBox*>();
        QVERIFY(ease);
        QCOMPARE(ease->count(), 4);
        QCOMPARE(ease->itemText(3), QStringLiteral("Ease In and Out"));
        ease->setCurrentIndex(1);
        emit ease->activated(1);
        QCOMPARE(dynamiczoom::ease(f.clip(0)->transform), QStringLiteral("easeIn"));
        auto* swap = dz->findChild<QPushButton*>(QStringLiteral("DynamicZoomSwap"));
        QVERIFY(swap && swap->isEnabled());
        swap->click();
        QCOMPARE(dynamiczoom::start(f.clip(0)->transform), dynamiczoom::defaultEnd());
        QCOMPARE(undo->text(undo->index() - 1), QStringLiteral("Swap Dynamic Zoom"));
    }

    // Spec 24.21: the Inspector's Speed Change and Retime and Scaling sections edit the clip
    // like the Change Clip Speed dialog: speed, frames per second and duration are linked,
    // Ripple Sequence (on by default) moves the later clips, Freeze Frame and Direction, the
    // retime process. One undo step each.
    void spec24_21_inspectorSpeedChange()
    {
        Fixture f(2, true);
        f.show();
        f.selection.setClips({f.clips[0]});
        QCoreApplication::processEvents();
        ParamGroupBox* speed = f.group("SpeedChangeGroup");
        QVERIFY(speed);
        auto* percent = speed->findChild<QDoubleSpinBox*>(QStringLiteral("SpeedPercent"));
        auto* perSecond = speed->findChild<QDoubleSpinBox*>(QStringLiteral("SpeedFps"));
        auto* duration = speed->findChild<QLineEdit*>(QStringLiteral("SpeedDuration"));
        auto* ripple = speed->findChild<QCheckBox*>(QStringLiteral("SpeedRipple"));
        auto* freeze = speed->findChild<QCheckBox*>(QStringLiteral("SpeedFreeze"));
        auto* direction = speed->findChild<QComboBox*>(QStringLiteral("SpeedDirection"));
        QVERIFY(percent && perSecond && duration && ripple && freeze && direction);
        QCOMPARE(percent->value(), 100.0);
        QVERIFY(ripple->isChecked());
        QUndoStack* undo = f.project.undoStack();
        int before = undo->index();
        percent->setValue(200.0);
        QCOMPARE(f.clip(0)->speed, 2.0);
        QCOMPARE(f.clip(0)->duration, int64_t(25));
        QCOMPARE(f.clip(1)->start, int64_t(25)); // rippled
        QCOMPARE(undo->index(), before + 1);
        QCOMPARE(undo->text(undo->index() - 1), QStringLiteral("Change Clip Speed"));
        QCOMPARE(perSecond->value(), 2.0 * f.project.timeline()->data().fps.toDouble());
        QCOMPARE(duration->text(), timecodeOf(f, 25));
        // Without ripple the clip keeps its length.
        ripple->click();
        percent->setValue(50.0);
        QCOMPARE(f.clip(0)->duration, int64_t(25));
        QCOMPARE(f.clip(0)->speed, 0.5);
        ripple->click();
        // A typed duration sets the speed (the material at the new length).
        duration->setText(timecodeOf(f, 25));
        emit duration->editingFinished();
        QCOMPARE(f.clip(0)->duration, int64_t(25));
        QCOMPARE(f.clip(0)->speed, 0.5);
        duration->setText(timecodeOf(f, 50));
        emit duration->editingFinished();
        QCOMPARE(f.clip(0)->speed, 0.25);
        // Reverse, then freeze.
        direction->setCurrentIndex(1);
        emit direction->activated(1);
        QVERIFY(f.clip(0)->speed < 0);
        QCOMPARE(direction->currentIndex(), 1);
        before = undo->index();
        freeze->click();
        QCOMPARE(f.clip(0)->speed, 0.0);
        QVERIFY(!percent->isEnabled());
        QCOMPARE(undo->index(), before + 1);
        // Retime process.
        ParamGroupBox* retime = f.group("RetimeScalingGroup");
        QVERIFY(retime);
        auto* process = retime->findChild<QComboBox*>(QStringLiteral("RetimeProcess"));
        QCOMPARE(process->currentText(), QStringLiteral("Project Settings"));
        process->setCurrentIndex(2);
        emit process->activated(2);
        QCOMPARE(f.clip(0)->retimeProcess, RetimeProcess::FrameBlend);
        // Solid clips have no speed sections.
        Fixture solid;
        solid.show();
        solid.selection.setClips({solid.clips[0]});
        QCoreApplication::processEvents();
        QVERIFY(!solid.group("SpeedChangeGroup"));
        QVERIFY(solid.group("DynamicZoomGroup"));
    }

private:
    static QString timecodeOf(const Fixture& f, int64_t frames);
};

#include "ui/common/TimecodeFormat.h"

QString TestBughunt7::timecodeOf(const Fixture& f, int64_t frames)
{
    return timecode::format(frames, f.project.timeline()->data().fps);
}

QTEST_MAIN(TestBughunt7)
#include "test_bughunt7.moc"
