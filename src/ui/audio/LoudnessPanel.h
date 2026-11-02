// SPDX-License-Identifier: GPL-3.0-or-later
// LoudnessPanel — the Audio page's monitoring panel: output and bus meters, BS.1770 / EBU
// R128 loudness (momentary, short-term, integrated) and true peak of the output bus.
#pragma once

#include "ui/common/PanelFrame.h"

#include <QVector>

class QHBoxLayout;
class QLabel;

namespace kk {

struct AppContext;
class LevelMeter;
class MeterModel;
class MixerController;

/// Vertical loudness bars (momentary, short-term) on a -50 ... 0 LUFS scale with the target
/// line; within +-1 LU of the target the bars use the scheme's positive colour, above it the
/// negative one.
class LoudnessGauge : public QWidget
{
    Q_OBJECT
public:
    explicit LoudnessGauge(MeterModel* meters, QWidget* parent = nullptr);
    [[nodiscard]] QSize sizeHint() const override { return {90, 120}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    MeterModel* m_meters;
};

/// The monitoring panel: the output bus meter with a dB scale, a small meter per bus, the
/// loudness gauge and readouts (M / S / I in LUFS, true peak max in dBTP, target -23 LUFS)
/// and Reset (the registry action "audio.reset_loudness"). Loudness is measured on the
/// output bus by the playback engine (render/LoudnessMeter.h).
///
/// Thread-safety: GUI thread only.
class LoudnessPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// EBU R128 programme target (LUFS).
    static constexpr double kTargetLufs = -23.0;

    LoudnessPanel(AppContext& ctx, MixerController* controller, MeterModel* meters,
        QWidget* parent = nullptr);

    /// The readout labels (tests): momentary, short-term, integrated, true peak.
    [[nodiscard]] QString readoutText() const;

private:
    void syncBuses();
    void updateReadouts();

    MixerController* m_controller;
    MeterModel* m_meters;
    LevelMeter* m_output = nullptr;
    QHBoxLayout* m_busRow = nullptr;
    QVector<LevelMeter*> m_busMeters;
    QVector<QLabel*> m_busLabels;
    QVector<int> m_busIds;
    QLabel* m_momentary = nullptr;
    QLabel* m_shortTerm = nullptr;
    QLabel* m_integrated = nullptr;
    QLabel* m_truePeak = nullptr;
};

} // namespace kk
