// SPDX-License-Identifier: GPL-3.0-or-later
// FaderWidget — a vertical console-style fader with a dB scale; PanKnob — a rotary pan control.
#pragma once

#include <QWidget>

namespace kk {

/// Vertical fader from -inf to +12 dB (audioscale::faderPosition()) with a dB scale.
///
/// Gestures (the owner turns them into undo steps, MixerController): press -> pressed(),
/// drag -> moved(dB) (relative to the press, Shift = a tenth of the speed), release ->
/// released(); a double-click asks for 0 dB (valueRequested()); a wheel notch asks for
/// +-0.5 dB. setValue() only moves the cap (no signal).
///
/// Thread-safety: GUI thread only.
class FaderWidget : public QWidget
{
    Q_OBJECT
public:
    explicit FaderWidget(QWidget* parent = nullptr);

    /// Shows @p db (clamped to the fader range; no signals).
    void setValue(double db);
    [[nodiscard]] double value() const { return m_db; }
    /// True while the cap is held.
    [[nodiscard]] bool isDragging() const { return m_dragging; }

    [[nodiscard]] QSize sizeHint() const override { return {44, 200}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {36, 90}; }

    /// The cap rect (tests drive drags with it).
    [[nodiscard]] QRect capRect() const;

signals:
    void pressed();
    void moved(double db);
    void released();
    /// A one-shot value (double-click: 0 dB; wheel: a step).
    void valueRequested(double db);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    [[nodiscard]] QRect track() const;
    [[nodiscard]] int yOf(double db) const;

    double m_db = 0.0;
    bool m_dragging = false;
    double m_pressY = 0.0;
    double m_pressPosition = 0.0;
};

/// Rotary pan control from -1 (left) to +1 (right): vertical or horizontal drags turn it
/// (200 px for the full range, Shift finer), a double-click centres it, the wheel steps by
/// 5 %. Signals as FaderWidget.
class PanKnob : public QWidget
{
    Q_OBJECT
public:
    explicit PanKnob(QWidget* parent = nullptr);

    /// Shows @p pan (no signals).
    void setValue(double pan);
    [[nodiscard]] double value() const { return m_pan; }

    [[nodiscard]] QSize sizeHint() const override { return {40, 40}; }

signals:
    void pressed();
    void moved(double pan);
    void released();
    void valueRequested(double pan);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    double m_pan = 0.0;
    bool m_dragging = false;
    QPointF m_pressPos;
    double m_pressPan = 0.0;
};

} // namespace kk
