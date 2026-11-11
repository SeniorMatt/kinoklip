// SPDX-License-Identifier: GPL-3.0-or-later
// MixerStrip — one channel strip of the mixer: input, EQ, dynamics, bus routing, pan,
// automation mode, record arm / mute / solo, fader with meter, name and channel format.
#pragma once

#include "core/MixerOps.h"

#include <QPointer>
#include <QWidget>

class QComboBox;
class QLabel;
class QToolButton;

namespace kk {

class DynamicsWindow;
class EqWindow;
class FaderWidget;
class LevelMeter;
class MeterModel;
class MixerController;
class PanKnob;

/// A vertical channel strip for a track or a bus (top to bottom, Resolve's order): input,
/// EQ and Dynamics buttons (lit when on; a click opens the small editor window), bus
/// assignment (tracks: a menu of checkable buses; buses: "Output" makes the bus the one heard
/// and rendered), pan knob and automation mode (tracks), record arm (voice-over, spec 75;
/// right-click: the input; a level bar shows while armed) / mute / solo, the fader with its meter,
/// the fader value, and the name with the channel format (right-click: Mono / Stereo; buses also
/// Rename / Delete).
///
/// Every gesture is one undo step through MixerController. The strip re-reads the timeline on
/// MixerController::mixChanged() and follows the playhead (automated faders move).
///
/// Thread-safety: GUI thread only.
class MixerStrip : public QWidget
{
    Q_OBJECT
public:
    MixerStrip(MixerController* controller, MeterModel* meters, const MixTarget& target,
        QWidget* parent = nullptr);

    [[nodiscard]] const MixTarget& target() const { return m_target; }
    [[nodiscard]] FaderWidget* fader() const { return m_fader; }
    [[nodiscard]] PanKnob* panKnob() const { return m_pan; }
    [[nodiscard]] QToolButton* eqButton() const { return m_eq; }
    [[nodiscard]] QToolButton* dynamicsButton() const { return m_dynamics; }
    [[nodiscard]] QToolButton* muteButton() const { return m_mute; }
    [[nodiscard]] QToolButton* soloButton() const { return m_solo; }
    [[nodiscard]] QToolButton* armButton() const { return m_arm; }
    [[nodiscard]] QToolButton* busButton() const { return m_bus; }
    [[nodiscard]] QComboBox* automationCombo() const { return m_automation; }

    /// Opens (or raises) the EQ / dynamics window of this strip.
    EqWindow* openEq();
    DynamicsWindow* openDynamics();

    /// Re-reads the strip's values at timeline frame @p frame.
    void refresh(int64_t frame);

    /// Strip width (px).
    static constexpr int kWidth = 92;

private:
    void fillBusMenu();
    void showNameMenu(const QPoint& globalPos);

    MixerController* m_controller;
    MeterModel* m_meters;
    MixTarget m_target;
    QLabel* m_input = nullptr;
    QToolButton* m_eq = nullptr;
    QToolButton* m_dynamics = nullptr;
    QToolButton* m_bus = nullptr;
    PanKnob* m_pan = nullptr;
    QComboBox* m_automation = nullptr;
    QToolButton* m_arm = nullptr;
    QWidget* m_inputLevel = nullptr; ///< level of the microphone, shown while armed
    QToolButton* m_mute = nullptr;
    QToolButton* m_solo = nullptr;
    FaderWidget* m_fader = nullptr;
    LevelMeter* m_meter = nullptr;
    QLabel* m_value = nullptr;
    QLabel* m_name = nullptr;
    int64_t m_frame = 0; ///< frame of the last refresh()
    QPointer<EqWindow> m_eqWindow;
    QPointer<DynamicsWindow> m_dynamicsWindow;
};

} // namespace kk
