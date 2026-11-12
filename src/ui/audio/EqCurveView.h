// SPDX-License-Identifier: GPL-3.0-or-later
// EqCurveView — the EQ window's response curve with one draggable handle per band.
#pragma once

#include "core/MixerData.h"

#include <QPoint>
#include <QWidget>

namespace kk {

/// Draws the magnitude response of an EqSettings (the product of its bands' biquads,
/// render/Biquad.h, so the picture is exactly what the mixer does) on a log frequency axis
/// (20 Hz - 20 kHz) and a +-24 dB axis, with a numbered handle per enabled band at (frequency,
/// gain). Dragging a handle moves the band (filters and the notch only in frequency); the
/// wheel over a handle changes its Q. Colours: scheme series colours per band, grid from the
/// scheme. Gestures are reported, not applied (EqWindow turns them into undo steps).
///
/// Also: Shift while dragging = fine (a tenth of the mouse travel), Alt-drag = Q (up = narrower),
/// double-click = reset the band, right-click = context request, keyboard (the view takes
/// focus): arrows move the selected band (Shift = fine), Page Up / Down change Q, Tab / Shift+Tab
/// walk the bands, Delete / Backspace reset. The shared look is also the clip equalizer in the
/// Inspector (ui/inspector/ClipEqEditor).
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
    /// Number of bands shown and editable (the first @p count of the settings; default all).
    void setBandCount(int count);
    /// Gain axis half range in dB (default 24).
    void setRangeDb(double db);
    /// Draws every enabled band's own curve faintly, not only the selected one's.
    void setShowBandCurves(bool on);
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
    /// Alt-drag or keyboard Q change inside a bandPressed ... bandReleased gesture.
    void bandQDragged(int band, double q);
    /// Double-click or Delete on a band: reset it.
    void bandResetRequested(int band);
    /// Right-click on a band (-1 = on the plot); @p globalPos for a menu.
    void bandContextRequested(int band, QPoint globalPos);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    bool focusNextPrevChild(bool next) override;

private:
    [[nodiscard]] QRectF plot() const;
    [[nodiscard]] double xOf(double hz) const;
    [[nodiscard]] double yOf(double db) const;
    [[nodiscard]] int bandAt(QPointF pos) const;

    EqSettings m_settings;
    int m_selected = -1;
    int m_dragging = -1;
    int m_count = kEqBandCount;
    double m_rangeDb = kEqMaxGainDb;
    bool m_showBandCurves = false;
    QPointF m_dragPoint; ///< the handle while dragging (plot coordinates, fine-mode aware)
    QPointF m_lastMouse;
    bool m_dragQ = false;
    double m_dragQValue = 1.0;
};

} // namespace kk
