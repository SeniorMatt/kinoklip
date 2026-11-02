// SPDX-License-Identifier: GPL-3.0-or-later
// DynamicsWindow — the small floating editor of a mixer channel's compressor and limiter,
// with its transfer curve and a gain reduction meter.
#pragma once

#include "core/MixerOps.h"

#include <QVector>
#include <QWidget>

#include <functional>
#include <utility>

class QCheckBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;

namespace kk {

class MeterModel;
class MixerController;

/// Draws the static input / output curve of the compressor plus the limiter ceiling
/// (dsp::compressorGainReductionDb(), the mixer's own formula), from -60 to 0 dBFS.
class TransferCurveView : public QWidget
{
    Q_OBJECT
public:
    explicit TransferCurveView(QWidget* parent = nullptr);
    /// Shows @p settings.
    void setSettings(const DynamicsSettings& settings);
    [[nodiscard]] QSize sizeHint() const override { return {200, 200}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    DynamicsSettings m_settings;
};

/// Tool window: compressor on, threshold, ratio, attack, release, knee, make-up; limiter on,
/// ceiling, release; the transfer curve; the current gain reduction (from the meters). Each
/// committed value or switch is one undo step.
///
/// Thread-safety: GUI thread only.
class DynamicsWindow : public QWidget
{
    Q_OBJECT
public:
    DynamicsWindow(MixerController* controller, MeterModel* meters, const MixTarget& target,
        QWidget* parent = nullptr);

    [[nodiscard]] const MixTarget& target() const { return m_target; }

private:
    void refresh();
    void editDynamics(const QString& label, const std::function<void(DynamicsSettings&)>& change);
    QDoubleSpinBox* addField(QFormLayout* form, const QString& label, double min, double max,
        int decimals, const QString& suffix, double DynamicsSettings::*field);

    MixerController* m_controller;
    MeterModel* m_meters;
    MixTarget m_target;
    TransferCurveView* m_curve = nullptr;
    QCheckBox* m_compressor = nullptr;
    QCheckBox* m_limiter = nullptr;
    QLabel* m_reduction = nullptr;
    QVector<std::pair<QDoubleSpinBox*, double DynamicsSettings::*>> m_fields;
};

} // namespace kk
