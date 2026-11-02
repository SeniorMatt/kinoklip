// SPDX-License-Identifier: GPL-3.0-or-later
// LevelMeter — a vertical peak meter (one or two bars with peak hold) reading a MeterModel.
#pragma once

#include "core/MixerOps.h"

#include <QWidget>

namespace kk {

class MeterModel;

/// Draws the level of one mixer channel (a track, a bus or the output) from a MeterModel:
/// linear-in-dB bars from audioscale::kMeterFloorDb to 0 dBFS, coloured by zone with the
/// scheme's positive / neutral / negative roles, a peak hold mark per bar, and optionally a
/// dB scale on the left. Mono channels draw one bar.
///
/// Thread-safety: GUI thread only.
class LevelMeter : public QWidget
{
    Q_OBJECT
public:
    /// What the meter shows.
    enum class Source {
        Track,
        Bus,
        Output
    };

    LevelMeter(MeterModel* model, Source source, quint64 id = 0, QWidget* parent = nullptr);

    /// Shows the dB scale left of the bars.
    void setScaleVisible(bool visible);
    /// Changes the metered channel.
    void setSource(Source source, quint64 id);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    MeterModel* m_model;
    Source m_source;
    quint64 m_id;
    bool m_scale = false;
};

} // namespace kk
