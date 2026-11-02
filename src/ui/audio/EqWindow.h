// SPDX-License-Identifier: GPL-3.0-or-later
// EqWindow — the small floating editor of a mixer channel's 6-band EQ.
#pragma once

#include "core/MixerOps.h"

#include <QWidget>

#include <array>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;

namespace kk {

class EqCurveView;
class MixerController;

/// Tool window with the response curve (EqCurveView, handles draggable) and per band a type,
/// on / off, frequency, gain and Q. Every gesture is one undo step: a handle drag from press
/// to release, each committed field value, each switch. Follows the timeline (undo, the
/// strip's own buttons) and closes itself when its track or bus is deleted.
///
/// Thread-safety: GUI thread only.
class EqWindow : public QWidget
{
    Q_OBJECT
public:
    EqWindow(MixerController* controller, const MixTarget& target, QWidget* parent = nullptr);

    [[nodiscard]] const MixTarget& target() const { return m_target; }
    [[nodiscard]] EqCurveView* curve() const { return m_curve; }

private:
    void refresh();
    /// One undo step changing band @p band of the target's EQ with @p change.
    void editBand(int band, const QString& label, const std::function<void(EqBand&)>& change);

    MixerController* m_controller;
    MixTarget m_target;
    EqCurveView* m_curve = nullptr;
    QCheckBox* m_enabled = nullptr;
    struct BandRow
    {
        QCheckBox* on = nullptr;
        QComboBox* type = nullptr;
        QDoubleSpinBox* frequency = nullptr;
        QDoubleSpinBox* gain = nullptr;
        QDoubleSpinBox* q = nullptr;
    };
    std::array<BandRow, kEqBandCount> m_rows;
};

} // namespace kk
