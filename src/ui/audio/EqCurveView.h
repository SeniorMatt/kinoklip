// SPDX-License-Identifier: GPL-3.0-or-later
// EqCurveView — the EQ window's response curve with one draggable handle per band.
#pragma once

#include "core/MixerData.h"

#include <QWidget>

namespace kk {

/// Draws the magnitude response of an EqSettings (the product of its bands' biquads,
/// render/Biquad.h, so the picture is exactly what the mixer does) on a log frequency axis
/// (20 Hz - 20 kHz) and a +-24 dB axis, with a numbered handle per enabled band at (frequency,
/// gain). Dragging a handle moves the band (filters and the notch only in frequency); the
/// wheel over a handle changes its Q. Colours: scheme series colours per band, grid from the
/// scheme. Gestures are reported, not applied (EqWindow turns them into undo steps).
///
/// Thread-safety: GUI thread only.
class EqCurveView : public QWidget
{
    Q_OBJECT
public:
    explicit EqCurveView(QWidget* parent = nullptr);

    /// Shows @p settings (no signals).
    void setSettings(const EqSettings& settings);
    /// Selects band @p band (-1 = none): it is drawn highlighted with its own curve.
    void setSelectedBand(int band);
    [[nodiscard]] int selectedBand() const { return m_selected; }
    /// Handle position of band @p band in widget coordinates (tests).
    [[nodiscard]] QPointF handlePosition(int band) const;

    [[nodiscard]] QSize sizeHint() const override { return {520, 240}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {260, 140}; }

signals:
    void bandSelected(int band);
    void bandPressed(int band);
    void bandDragged(int band, double frequencyHz, double gainDb);
    void bandReleased(int band);
    /// Wheel over a band handle: the new Q.
    void bandQRequested(int band, double q);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    [[nodiscard]] QRectF plot() const;
    [[nodiscard]] double xOf(double hz) const;
    [[nodiscard]] double yOf(double db) const;
    [[nodiscard]] int bandAt(QPointF pos) const;

    EqSettings m_settings;
    int m_selected = -1;
    int m_dragging = -1;
};

} // namespace kk
