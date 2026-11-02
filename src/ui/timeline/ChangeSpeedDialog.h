// SPDX-License-Identifier: GPL-3.0-or-later
// ChangeSpeedDialog — the Change Clip Speed dialog (R): speed in percent, frames per second and
// duration (linked), reverse, freeze frame, ripple sequence and pitch correction.
#pragma once

#include "core/RetimeOps.h"
#include "core/TimelineData.h"

#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QLineEdit;

namespace kk {

/// Modal dialog filled from one clip; result() gives the retimeops::SpeedChange to apply.
///
/// The three amounts are one value seen three ways: speed (%), frames per second (speed times
/// the source frame rate) and duration (the clip's material at that speed, timeline timecode).
/// Editing one updates the others. Freeze frame disables them (the clip keeps its length).
/// Thread-safety: GUI thread only.
class ChangeSpeedDialog : public QDialog
{
    Q_OBJECT
public:
    /// @p clip the clip whose values are shown, @p fps the timeline rate, @p sourceFps the
    /// clip's media frame rate (invalid: the timeline rate).
    ChangeSpeedDialog(const ClipData& clip, Rational fps, Rational sourceFps, QWidget* parent);

    /// The change as entered.
    [[nodiscard]] retimeops::SpeedChange result() const;

    /// Controls (tests).
    QDoubleSpinBox* speedBox() const { return m_speed; }
    QDoubleSpinBox* fpsBox() const { return m_fps; }
    QLineEdit* durationEdit() const { return m_duration; }
    QCheckBox* reverseBox() const { return m_reverse; }
    QCheckBox* freezeBox() const { return m_freeze; }
    QCheckBox* rippleBox() const { return m_ripple; }
    QCheckBox* pitchBox() const { return m_pitch; }

private:
    void setSpeed(double speed, QObject* from);
    void updateEnabled();

    Rational m_timelineFps;
    double m_sourceFps = 25.0;
    double m_material = 1.0; ///< the clip's material in retime frames (frames at 100 %)
    double m_value = 1.0; ///< speed magnitude
    bool m_syncing = false;
    QDoubleSpinBox* m_speed = nullptr;
    QDoubleSpinBox* m_fps = nullptr;
    QLineEdit* m_duration = nullptr;
    QCheckBox* m_reverse = nullptr;
    QCheckBox* m_freeze = nullptr;
    QCheckBox* m_ripple = nullptr;
    QCheckBox* m_pitch = nullptr;
};

} // namespace kk
