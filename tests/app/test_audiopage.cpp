// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen tests of the Audio page (docs/behavior-spec.md section 37): page wiring (Shift+7,
// shared project / timeline / playhead / undo with the Edit page), the timeline's audio page
// view, one undo step per mixer gesture (fader, pan, mute, EQ handle, bus routing), touch
// automation recording, meters feeding the widgets, the Edit page's Mixer panel.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/AudioPage.h"
#include "app/pages/EditPage.h"
#include "core/MixerOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/audio/AudioScales.h"
#include "ui/audio/DynamicsWindow.h"
#include "ui/audio/EqCurveView.h"
#include "ui/audio/EqWindow.h"
#include "ui/audio/FaderWidget.h"
#include "ui/audio/LoudnessPanel.h"
#include "ui/audio/MeterModel.h"
#include "ui/audio/MixerController.h"
#include "ui/audio/MixerPanel.h"
#include "ui/audio/MixerStrip.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>

#include <memory>

using namespace kk;

namespace {

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        MainWindowOptions options;
        options.autosave = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
    AudioPage* audio() const { return window->audioPage(); }
    const TimelineData& data() const { return project.timeline()->data(); }
    int steps() const { return project.undoStack()->count(); }
    MixerStrip* strip(const MixTarget& t) const { return audio()->mixer()->strip(t); }
    quint64 a1() const { return data().audioTracks[0].id; }

    void showAudio()
    {
        window->resize(1800, 1000);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        action("view.page_audio")->trigger();
        QCoreApplication::processEvents();
    }
};

/// Press at @p from, move to @p to in steps, release (left button).
void drag(QWidget* w, QPoint from, QPoint to)
{
    auto send = [w](QEvent::Type type, QPoint p, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent e(
            type, QPointF(p), w->mapToGlobal(QPointF(p)), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(w, &e);
    };
    send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 5; ++i)
        send(QEvent::MouseMove, from + (to - from) * i / 5, Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

} // namespace

class TestAudioPage : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_audiopage"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    // ---- 37.1 page wiring ----
    void pageIsEnabledAndSwitchesWithShift7()
    {
        Fixture f;
        QVERIFY(f.audio());
        QVERIFY(f.action("view.page_audio")->isEnabled());
        QCOMPARE(f.actions.shortcuts(QStringLiteral("view.page_audio")).value(0),
            QKeySequence(QStringLiteral("Shift+7")));
        f.action("view.page_audio")->trigger();
        QCOMPARE(f.window->currentPageId(), QStringLiteral("audio"));
        // The interface bar offers the mixer toggle there.
        QVERIFY(f.action("view.toggle_mixer")->isEnabled());
        f.action("view.toggle_mixer")->trigger();
        QVERIFY(!f.audio()->isPanelVisible(PanelId::Mixer));
        f.action("view.toggle_mixer")->trigger();
        QVERIFY(f.audio()->isPanelVisible(PanelId::Mixer));
    }

    void pageActionsOnlyOnTheAudioPage()
    {
        Fixture f;
        f.showAudio();
        QVERIFY(f.action("audio.float_viewer")->isEnabled());
        f.action("audio.float_viewer")->trigger();
        QVERIFY(f.audio()->isViewerFloating());
        // Polish round 2: the floating viewer and the EQ / dynamics windows carry the
        // window-scoped shortcuts (Space plays while they have focus).
        QWidget* floating = f.window->findChild<QWidget*>(QStringLiteral("AudioViewerWindow"));
        QVERIFY(floating);
        QVERIFY(floating->actions().contains(f.action("transport.play_pause")));
        QVERIFY(f.strip(MixTarget::track(f.a1()))
                    ->openEq()
                    ->actions()
                    .contains(f.action("transport.play_pause")));
        QVERIFY(f.strip(MixTarget::track(f.a1()))
                    ->openDynamics()
                    ->actions()
                    .contains(f.action("edit.undo")));
        f.action("audio.float_viewer")->trigger();
        QVERIFY(!f.audio()->isViewerFloating());
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        QVERIFY(!f.action("audio.float_viewer")->isEnabled());
        QVERIFY(f.action("audio.add_bus")->isEnabled()); // the Edit page's mixer uses it too
    }

    // ---- 37.2 shared state ----
    void sharesTimelinePlayheadAndUndoWithEditPage()
    {
        Fixture f;
        f.showAudio();
        // One timeline controller (tool, snapping, targets) for every view.
        QCOMPARE(
            f.audio()->timeline()->controller(), f.window->editPage()->timeline()->controller());
        f.project.timeline()->setPlayhead(42);
        f.action("view.page_edit")->trigger();
        QCOMPARE(f.project.timeline()->playhead(), int64_t(42));
        // A mixer edit on the Audio page is undone from the Edit page (one undo stack) and
        // shows in the Edit page's mixer panel.
        f.action("view.page_audio")->trigger();
        f.audio()->mixerController()->setControl(
            MixTarget::track(f.a1()), MixerController::Control::Fader, -9.0);
        QCOMPARE(f.data().audioTracks[0].mix.strip.volumeDb, -9.0);
        auto* editMixer = f.window->editPage()->findChild<MixerPanel*>();
        QVERIFY(editMixer);
        QCOMPARE(editMixer->strip(MixTarget::track(f.a1()))->fader()->value(), -9.0);
        f.action("view.page_edit")->trigger();
        f.action("edit.undo")->trigger();
        QCOMPARE(f.data().audioTracks[0].mix.strip.volumeDb, 0.0);
    }

    // ---- 37.3 the timeline view ----
    void timelineShowsTallAudioAndCollapsedVideo()
    {
        Fixture f;
        f.showAudio();
        const TimelineLayout& L = f.audio()->timeline()->layout();
        const int audioRow = L.rowOf(TrackRef {TrackType::Audio, 0});
        const int videoRow = L.rowOf(TrackRef {TrackType::Video, 0});
        QVERIFY(audioRow >= 0 && videoRow >= 0);
        QVERIFY(L.rowRect(audioRow).height() >= 2 * timelinemetrics::kDefaultAudioHeight);
        QCOMPARE(L.rowRect(videoRow).height(), timelinemetrics::kMinTrackHeight);
        // Audio page headers: record arm and a meter; the Edit page has neither.
        QVERIFY(!L.headerButtonRect(audioRow, HeaderButton::Arm).isEmpty());
        QVERIFY(!L.headerMeterRect(audioRow).isEmpty());
        // Round 2 polish: the buttons go to a second line so the track name has the first
        // line up to the meter (it used to be cut to "A...").
        QVERIFY(L.twoLineHeader(audioRow));
        const QRect patch = L.headerButtonRect(audioRow, HeaderButton::Target);
        for (const HeaderButton b : L.buttonsFor(TrackType::Audio)) {
            const QRect br = L.headerButtonRect(audioRow, b);
            QVERIFY(br.top() > patch.bottom());
            QVERIFY(br.right() < L.headerMeterRect(audioRow).left());
        }
        QVERIFY(L.headerMeterRect(audioRow).left() - patch.right() > 100); // name room
        const TimelineLayout& edit = f.window->editPage()->timeline()->timelineWidget()->layout();
        QVERIFY(edit.headerButtonRect(edit.rowOf(TrackRef {TrackType::Audio, 0}), HeaderButton::Arm)
                    .isEmpty());
        // The arm button does nothing (recording is deferred): no undo step.
        const int before = f.steps();
        QWidget* vp = f.audio()->timeline()->viewport();
        const QPoint arm = L.headerButtonRect(audioRow, HeaderButton::Arm).center();
        QTest::mouseClick(vp, Qt::LeftButton, {}, arm);
        QCOMPARE(f.steps(), before);
        // Mute from the timeline header shows in the mixer (shared state).
        QTest::mouseClick(
            vp, Qt::LeftButton, {}, L.headerButtonRect(audioRow, HeaderButton::Mute).center());
        QVERIFY(f.data().audioTracks[0].muted);
        QVERIFY(f.strip(MixTarget::track(f.a1()))->muteButton()->isChecked());
    }

    // ---- 37.x one undo step per gesture ----
    void faderDragIsOneUndoStep()
    {
        Fixture f;
        f.showAudio();
        FaderWidget* fader = f.strip(MixTarget::track(f.a1()))->fader();
        const int before = f.steps();
        const QPoint cap = fader->capRect().center();
        drag(fader, cap, cap + QPoint(0, 40));
        const double db = f.data().audioTracks[0].mix.strip.volumeDb;
        QVERIFY2(db < -1.0, qPrintable(QString::number(db)));
        QCOMPARE(f.steps(), before + 1);
        QCOMPARE(fader->value(), db);
        // Double-click: back to 0 dB, one more step.
        QTest::mouseDClick(fader, Qt::LeftButton, {}, cap);
        QCOMPARE(f.data().audioTracks[0].mix.strip.volumeDb, 0.0);
        QCOMPARE(f.steps(), before + 2);
        f.project.undo();
        QCOMPARE(f.data().audioTracks[0].mix.strip.volumeDb, db);
    }

    void panMuteSoloAndBusFaderSteps()
    {
        Fixture f;
        f.showAudio();
        MixerStrip* s = f.strip(MixTarget::track(f.a1()));
        int steps = f.steps();
        drag(s->panKnob(), QPoint(20, 20), QPoint(60, 20));
        QVERIFY(f.data().audioTracks[0].mix.strip.pan > 0.1);
        QCOMPARE(f.steps(), ++steps);
        s->muteButton()->click();
        QVERIFY(f.data().audioTracks[0].muted);
        QCOMPARE(f.steps(), ++steps);
        s->soloButton()->click();
        QVERIFY(f.data().audioTracks[0].solo);
        QCOMPARE(f.steps(), ++steps);
        MixerStrip* bus = f.strip(MixTarget::bus(kMainBusId));
        QVERIFY(bus);
        const QPoint cap = bus->fader()->capRect().center();
        drag(bus->fader(), cap, cap + QPoint(0, 30));
        QVERIFY(f.data().mixer.buses[0].strip.volumeDb < 0.0);
        QCOMPARE(f.steps(), ++steps);
    }

    void busesAndRouting()
    {
        Fixture f;
        f.showAudio();
        const int steps = f.steps();
        f.action("audio.add_bus")->trigger();
        QCOMPARE(int(f.data().mixer.buses.size()), 2);
        QCOMPARE(f.steps(), steps + 1);
        const int bus2 = f.data().mixer.buses[1].id;
        QVERIFY(f.strip(MixTarget::bus(bus2))); // a strip appeared
        // Route A1 to Bus 2 from its strip's bus menu.
        QMenu* menu = f.strip(MixTarget::track(f.a1()))->busButton()->menu();
        emit menu->aboutToShow();
        QAction* entry = nullptr;
        for (QAction* a : menu->actions()) {
            if (a->text() == QStringLiteral("Bus %1").arg(bus2))
                entry = a;
        }
        QVERIFY(entry);
        entry->trigger();
        QCOMPARE(f.data().audioTracks[0].mix.buses, (QVector<int> {kMainBusId, bus2}));
        QCOMPARE(f.steps(), steps + 2);
        // The bus strip's Output button makes it the output.
        f.strip(MixTarget::bus(bus2))->busButton()->click();
        QCOMPARE(f.data().mixer.outputBus, bus2);
        QCOMPARE(f.steps(), steps + 3);
    }

    void eqHandleDragIsOneUndoStep()
    {
        Fixture f;
        f.showAudio();
        EqWindow* eq = f.strip(MixTarget::track(f.a1()))->openEq();
        eq->resize(700, 500);
        QVERIFY(QTest::qWaitForWindowExposed(eq));
        EqCurveView* curve = eq->curve();
        const int steps = f.steps();
        const QPoint handle = curve->handlePosition(2).toPoint(); // the 500 Hz bell
        drag(curve, handle, handle + QPoint(30, -40));
        const EqBand& band = f.data().audioTracks[0].mix.strip.eq.bands[2];
        QVERIFY(band.gainDb > 3.0);
        QVERIFY(band.frequencyHz > 500.0);
        QVERIFY(f.data().audioTracks[0].mix.strip.eq.enabled); // dragging switches the EQ on
        QCOMPARE(f.steps(), steps + 1);
        // The strip's EQ button shows it is on.
        QVERIFY(f.strip(MixTarget::track(f.a1()))->eqButton()->isChecked());
        f.project.undo();
        QCOMPARE(f.data().audioTracks[0].mix.strip.eq, EqSettings {});
        QVERIFY(eq->isVisible());
    }

    // ---- 37.x automation ----
    void touchAutomationRecordsWhilePlaying()
    {
        Fixture f;
        f.showAudio();
        MixerController* mc = f.audio()->mixerController();
        bool playing = false;
        mc->setPlayingProbe([&playing] { return playing; });
        const MixTarget t = MixTarget::track(f.a1());
        mc->edit(QStringLiteral("Touch"), [&](TimelineData& d) {
            return mixerops::setAutomationMode(d, f.a1(), AutomationMode::Touch);
        });
        const int steps = f.steps();
        // "Playing": the playhead advances; the fader is held from frame 10 to 40.
        playing = true;
        mc->transportStateChanged();
        f.project.timeline()->setPlayhead(10);
        mc->pressControl(t, MixerController::Control::Fader);
        mc->moveControl(t, MixerController::Control::Fader, -12.0);
        for (int64_t frame = 11; frame <= 40; ++frame) {
            f.project.timeline()->setPlayhead(frame);
            if (frame == 25)
                mc->moveControl(t, MixerController::Control::Fader, -6.0);
        }
        mc->releaseControl(t, MixerController::Control::Fader);
        f.project.timeline()->setPlayhead(60); // released: no more keys
        playing = false;
        mc->transportStateChanged();
        QCOMPARE(f.steps(), steps + 1); // one undo step for the pass
        const TrackData& track = f.data().audioTracks[0];
        QVERIFY(mixerops::readsAutomation(track.mix, mixkeys::kVolumeDb));
        QCOMPARE(mixerops::faderDbAt(track, 15.0), -12.0);
        QCOMPARE(mixerops::faderDbAt(track, 35.0), -6.0);
        QVERIFY(!track.mix.automationKeys.values.value(mixkeys::kVolumeDb).hasKeyAt(60));
        // The strip's fader follows the curve at the playhead.
        f.project.timeline()->setPlayhead(15);
        QCOMPARE(f.strip(t)->fader()->value(), -12.0);
        f.project.undo();
        QVERIFY(!mixerops::readsAutomation(f.data().audioTracks[0].mix, mixkeys::kVolumeDb));
    }

    void writeModeRecordsTheWholePass()
    {
        Fixture f;
        f.showAudio();
        MixerController* mc = f.audio()->mixerController();
        bool playing = false;
        mc->setPlayingProbe([&playing] { return playing; });
        mc->edit(QStringLiteral("Write"), [&](TimelineData& d) {
            d.audioTracks[0].mix.strip.volumeDb = -3.0;
            return mixerops::setAutomationMode(d, f.a1(), AutomationMode::Write);
        });
        const int steps = f.steps();
        f.project.timeline()->setPlayhead(0);
        playing = true;
        mc->transportStateChanged();
        for (int64_t frame = 1; frame <= 20; ++frame)
            f.project.timeline()->setPlayhead(frame);
        playing = false;
        mc->transportStateChanged();
        QCOMPARE(f.steps(), steps + 1);
        const TrackData& track = f.data().audioTracks[0];
        const AnimatedValue curve = track.mix.automationKeys.values.value(mixkeys::kVolumeDb);
        QVERIFY(curve.hasKeyAt(0) && curve.hasKeyAt(20));
        QCOMPARE(mixerops::faderDbAt(track, 10.0), -3.0);
    }

    void stoppedFaderMoveSetsTheKeyAtThePlayhead()
    {
        Fixture f;
        f.showAudio();
        MixerController* mc = f.audio()->mixerController();
        mc->edit(QStringLiteral("Keys"), [&](TimelineData& d) {
            mixerops::writeAutomation(d, f.a1(), mixkeys::kVolumeDb, 0, 0.0, 0);
            return mixerops::writeAutomation(d, f.a1(), mixkeys::kVolumeDb, 50, -10.0, 50);
        });
        f.project.timeline()->setPlayhead(25);
        const MixTarget t = MixTarget::track(f.a1());
        mc->setControl(t, MixerController::Control::Fader, -2.0);
        const TrackData& track = f.data().audioTracks[0];
        QCOMPARE(mixerops::faderDbAt(track, 25.0), -2.0); // a key at the playhead
        QCOMPARE(track.mix.strip.volumeDb, 0.0); // the static value is untouched
    }

    // ---- 37.x meters ----
    void metersFeedTheWidgets()
    {
        Fixture f;
        f.showAudio();
        MeterFrame frame;
        frame.sample = 0;
        ChannelLevel* level = frame.levels.tracks.add();
        level->id = f.a1();
        level->peak[0] = 0.5f;
        level->peak[1] = 0.25f;
        ChannelLevel* bus = frame.levels.buses.add();
        bus->id = kMainBusId;
        bus->peak[0] = bus->peak[1] = 0.5f;
        frame.levels.output[0] = frame.levels.output[1] = 0.5f;
        frame.momentaryLufs = -23.0;
        frame.shortTermLufs = -22.5;
        frame.integratedLufs = -23.4;
        frame.truePeakMaxDb = -5.9;
        MeterModel* model = f.audio()->meterModel();
        model->setFrame(frame);
        const MeterReading r = model->track(f.a1());
        QVERIFY(std::abs(r.levelDb[0] - audioscale::toDb(0.5)) < 0.01);
        QVERIFY(std::abs(r.holdDb[1] - audioscale::toDb(0.25)) < 0.01);
        QVERIFY(std::abs(model->output().levelDb[0] - audioscale::toDb(0.5)) < 0.01);
        const QString readout = f.audio()->meters()->readoutText();
        QVERIFY2(readout.contains(QStringLiteral("-23.0 LUFS"))
                && readout.contains(QStringLiteral("-23.4 LUFS"))
                && readout.contains(QStringLiteral("-5.9 dBTP")),
            qPrintable(readout));
        // A silent frame later: the bar falls back, the hold mark stays for a while.
        QTest::qWait(200);
        model->setFrame(MeterFrame {});
        const MeterReading later = model->track(f.a1());
        QVERIFY(later.levelDb[0] < r.levelDb[0]);
        QCOMPARE(later.holdDb[0], r.holdDb[0]);
    }

    void scalesAreMonotonicAndInvertible()
    {
        for (double db : {-60.0, -30.0, -12.0, -6.0, 0.0, 6.0, 12.0}) {
            QVERIFY(std::abs(audioscale::faderDb(audioscale::faderPosition(db)) - db) < 1e-9);
            QVERIFY(
                audioscale::faderPosition(db) < audioscale::faderPosition(db + 1.0) || db >= 12.0);
        }
        QCOMPARE(audioscale::faderPosition(kFaderMinDb), 0.0);
        QCOMPARE(audioscale::faderPosition(12.0), 1.0);
        QVERIFY(std::abs(audioscale::faderPosition(0.0) - 0.76) < 1e-9);
        QVERIFY(
            std::abs(audioscale::positionFrequency(audioscale::frequencyPosition(1000.0)) - 1000.0)
            < 1e-6);
    }
};

QTEST_MAIN(TestAudioPage)
#include "test_audiopage.moc"
