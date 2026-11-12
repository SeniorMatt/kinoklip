// SPDX-License-Identifier: GPL-3.0-or-later
// The graphical clip Equalizer in the Inspector's Audio tab (docs/behavior-spec.md 67.8 - 67.12):
// the curve is the DSP's own response, handles map to frequency / gain on a log axis, a drag is
// one undo step, keyboard and reset.
#include "PanelTestUtils.h"
#include "core/ParamKeys.h"
#include "core/Timeline.h"
#include "render/Biquad.h"
#include "ui/audio/EqCurveView.h"
#include "ui/inspector/ClipEqEditor.h"
#include "ui/inspector/InspectorPanel.h"

#include <QApplication>
#include <QMouseEvent>
#include <QTabWidget>
#include <QTest>
#include <QUndoStack>
#include <QWheelEvent>

#include <cmath>

using namespace kk;
using namespace kk::test;

namespace {
const TrackRef kA1 {TrackType::Audio, 0};

struct Rig
{
    PanelFixture f;
    quint64 clip = 0;
    InspectorPanel panel;
    ClipEqEditor* editor = nullptr;
    EqCurveView* view = nullptr;

    Rig()
        : panel(f.ctx)
    {
        clip = f.addClip(kA1, 0, 50);
        panel.resize(460, 900);
        panel.show();
        f.selection.selectClip(clip);
        editor = panel.findChild<ClipEqEditor*>();
        view = editor ? editor->curve() : nullptr;
        if (view) {
            for (int i = 0; i < panel.tabs()->count(); ++i) {
                if (panel.tabs()->widget(i)->isAncestorOf(editor))
                    panel.tabs()->setCurrentIndex(i);
            }
            QApplication::processEvents();
            QApplication::processEvents();
        }
    }
    double value(const QString& key, double def = 0.0) const
    {
        return f.clip(clip)->audio.value(key, 0, def);
    }
    int steps() { return f.project.undoStack()->count(); }
};
} // namespace

class ClipEqEditorTest : public QObject
{
    Q_OBJECT
private slots:
    void responseMatchesTheDsp()
    {
        Rig r;
        QVERIFY(r.editor && r.view);
        // Gain at the centre of a bell equals the band gain; a shelf is flat far above / below.
        const dsp::BiquadCoeffs bell = dsp::designBand(EqBandType::Bell, 2000.0, 9.0, 1.0, 48000.0);
        QVERIFY(std::abs(dsp::magnitudeDb(bell, 2000.0, 48000.0) - 9.0) < 0.01);
        const dsp::BiquadCoeffs low
            = dsp::designBand(EqBandType::LowShelf, 100.0, -6.0, 0.707, 48000.0);
        QVERIFY(std::abs(dsp::magnitudeDb(low, 20.0, 48000.0) + 6.0) < 0.6);
        QVERIFY(std::abs(dsp::magnitudeDb(low, 10000.0, 48000.0)) < 0.1);
        // The editor shows the clip's values with the fixed band types.
        r.f.project.beginEdit(QStringLiteral("set"));
        r.f.project.timeline()->mutableData().audioTracks[0].clips[0].audio.setBase(
            keys::EqBandGain[1], 9.0);
        r.f.project.endEdit();
        QApplication::processEvents();
        QCOMPARE(r.editor->settings().bands[1].gainDb, 9.0);
        QCOMPARE(r.editor->settings().bands[0].type, EqBandType::LowShelf);
        QCOMPARE(r.editor->settings().bands[3].type, EqBandType::HighShelf);
        saveScreenshot(r.panel, QStringLiteral("clip-equalizer-editor"));
    }

    void dragMapsToLogFrequencyAndGainInOneUndoStep()
    {
        Rig r;
        QVERIFY(r.view);
        const int before = r.steps();
        const QPoint h = r.view->handlePosition(1).toPoint(); // 500 Hz bell
        QTest::mousePress(r.view, Qt::LeftButton, {}, h);
        QTest::mouseMove(r.view, h + QPoint(20, 0));
        QTest::mouseMove(r.view, h + QPoint(40, -30));
        QTest::mouseRelease(r.view, Qt::LeftButton, {}, h + QPoint(40, -30));
        QVERIFY(r.value(keys::EqBandFreq[1], 500) > 500.0);
        QVERIFY(r.value(keys::EqBandGain[1]) > 3.0);
        QVERIFY(r.f.clip(r.clip)->audio.staticValue(keys::EqEnabled, false).toBool());
        QCOMPARE(r.steps(), before + 1);
        // Log axis: the handle sits where the frequency maps to, so a round trip is exact.
        const QPointF pos = r.view->handlePosition(1);
        QVERIFY(std::abs(pos.x() - (h.x() + 40)) < 1.5);
        r.f.project.undo();
        QCOMPARE(r.value(keys::EqBandGain[1]), 0.0);
        QCOMPARE(r.value(keys::EqBandFreq[1], 500), 500.0);
    }

    void shiftDragIsFine()
    {
        Rig r;
        const QPoint h = r.view->handlePosition(2).toPoint();
        QTest::mousePress(r.view, Qt::LeftButton, {}, h);
        QTest::mouseMove(r.view, h + QPoint(0, -40), 0); // coarse
        QTest::mouseRelease(r.view, Qt::LeftButton, {}, h + QPoint(0, -40));
        const double coarse = r.value(keys::EqBandGain[2]);
        r.f.project.undo();
        QTest::mousePress(r.view, Qt::LeftButton, {}, h);
        QMouseEvent move(QEvent::MouseMove, QPointF(h + QPoint(0, -40)),
            QPointF(r.view->mapToGlobal(h + QPoint(0, -40))), Qt::NoButton, Qt::LeftButton,
            Qt::ShiftModifier);
        QApplication::sendEvent(r.view, &move);
        QTest::mouseRelease(r.view, Qt::LeftButton, {}, h + QPoint(0, -40));
        const double fine = r.value(keys::EqBandGain[2]);
        QVERIFY(coarse > 2.0);
        QVERIFY(std::abs(fine - coarse / 10.0) < 0.2);
    }

    void wheelAndAltDragChangeQ()
    {
        Rig r;
        const QPoint h = r.view->handlePosition(1).toPoint();
        const int before = r.steps();
        QWheelEvent wheel(QPointF(h), QPointF(r.view->mapToGlobal(h)), {}, QPoint(0, 120),
            Qt::NoButton, {}, Qt::NoScrollPhase, false);
        QApplication::sendEvent(r.view, &wheel);
        QVERIFY(r.value(keys::EqBandQ[1], 1.0) > 1.05);
        QCOMPARE(r.steps(), before + 1);
        const double q0 = r.value(keys::EqBandQ[1], 1.0);
        QTest::mousePress(r.view, Qt::LeftButton, {}, h);
        QMouseEvent move(QEvent::MouseMove, QPointF(h + QPoint(0, -30)),
            QPointF(r.view->mapToGlobal(h + QPoint(0, -30))), Qt::NoButton, Qt::LeftButton,
            Qt::AltModifier);
        QApplication::sendEvent(r.view, &move);
        QTest::mouseRelease(r.view, Qt::LeftButton, {}, h);
        QVERIFY(r.value(keys::EqBandQ[1], 1.0) > q0 * 1.3);
        QCOMPARE(r.value(keys::EqBandGain[1]), 0.0); // Q only
        QCOMPARE(r.steps(), before + 2);
    }

    void keyboardMovesTheSelectedBandAndTabCyclesBands()
    {
        Rig r;
        r.view->setFocus();
        r.view->setSelectedBand(1);
        const int before = r.steps();
        QTest::keyClick(r.view, Qt::Key_Up);
        QCOMPARE(r.value(keys::EqBandGain[1]), 0.5);
        QTest::keyClick(r.view, Qt::Key_Down, Qt::ShiftModifier);
        QVERIFY(std::abs(r.value(keys::EqBandGain[1]) - 0.4) < 1e-9);
        QTest::keyClick(r.view, Qt::Key_Right);
        QVERIFY(r.value(keys::EqBandFreq[1], 500) > 500.0);
        QCOMPARE(r.steps(), before + 3);
        QTest::keyClick(r.view, Qt::Key_Tab);
        QCOMPARE(r.view->selectedBand(), 2);
        QTest::keyClick(r.view, Qt::Key_Backtab, Qt::ShiftModifier);
        QCOMPARE(r.view->selectedBand(), 1);
    }

    void doubleClickResetsTheBand()
    {
        Rig r;
        r.f.project.beginEdit(QStringLiteral("set"));
        auto& audio = r.f.project.timeline()->mutableData().audioTracks[0].clips[0].audio;
        audio.setBase(keys::EqBandGain[3], -8.0);
        audio.setBase(keys::EqBandFreq[3], 5000.0);
        r.f.project.endEdit();
        QApplication::processEvents();
        const QPoint h = r.view->handlePosition(3).toPoint();
        const int before = r.steps();
        QTest::mouseDClick(r.view, Qt::LeftButton, {}, h);
        QCOMPARE(r.value(keys::EqBandGain[3]), 0.0);
        QCOMPARE(r.value(keys::EqBandFreq[3], 8000), 8000.0);
        QCOMPARE(r.steps(), before + 1);
    }
};

QTEST_MAIN(ClipEqEditorTest)
#include "test_clipeqeditor.moc"
