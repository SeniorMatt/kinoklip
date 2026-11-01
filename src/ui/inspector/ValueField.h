// SPDX-License-Identifier: GPL-3.0-or-later
// ValueField — a compact numeric field that is scrubbed by dragging and typed on double-click.
#pragma once

#include <QString>
#include <QWidget>

class QLineEdit;

namespace kk {

/// Numeric value editor in the style common to NLE inspectors.
///
/// - Press and drag horizontally to scrub: one pixel = `step` (Shift = step / 10 for fine
///   control, Ctrl = step * 10 for coarse changes). The value is clamped to [min, max].
/// - Double-click (or Enter while focused) opens an inline line edit to type a value;
///   Enter commits, Escape cancels.
/// - Mouse wheel over a focused field changes the value by one step.
///
/// The widget paints itself (no child widgets while not editing), so a refresh is a cheap
/// repaint: setValue() only formats the text and calls update() when the value changed.
///
/// Signals for undo grouping: scrubStarted() on press, valueScrubbed() during the drag and
/// scrubFinished() on release (one undo step for the whole drag); valueCommitted() for a
/// one-shot change (typed value, wheel, arrow keys).
///
/// Thread-safety: GUI thread only.
class ValueField : public QWidget
{
    Q_OBJECT
public:
    /// Creates a field with range [0, 1] and step 0.01.
    explicit ValueField(QWidget* parent = nullptr);

    /// Sets the allowed range (inclusive).
    void setRange(double min, double max);
    /// Value change per dragged pixel / wheel notch (before the Shift/Ctrl modifiers).
    void setStep(double step);
    /// Number of decimals shown and accepted (0 for integer parameters).
    void setDecimals(int decimals);
    /// Unit suffix drawn after the number (e.g. "px", "%", "dB").
    void setSuffix(const QString& suffix);

    /// Current value.
    [[nodiscard]] double value() const { return m_value; }
    /// Displays @p value (clamped) without emitting any signal. Cheap when unchanged.
    void setValue(double value);

    /// Minimum of the range.
    [[nodiscard]] double minimum() const { return m_min; }
    /// Maximum of the range.
    [[nodiscard]] double maximum() const { return m_max; }
    /// True while the user is dragging.
    [[nodiscard]] bool isScrubbing() const { return m_scrubbing; }

    /// Text shown for @p value (number with the configured decimals plus suffix).
    [[nodiscard]] QString textForValue(double value) const;

    /// Opens the inline text editor (same as a double-click).
    void startTextEdit();

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

signals:
    /// The user pressed the mouse to start dragging (open an undo group now).
    void scrubStarted();
    /// The value changed during a drag.
    void valueScrubbed(double value);
    /// The drag ended (close the undo group).
    void scrubFinished();
    /// The user changed the value in one step (typed, wheel, keys).
    void valueCommitted(double value);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    [[nodiscard]] double clamp(double value) const;
    [[nodiscard]] double modifierStep(Qt::KeyboardModifiers modifiers) const;
    void finishTextEdit(bool commit);

    double m_min = 0.0;
    double m_max = 1.0;
    double m_step = 0.01;
    int m_decimals = 2;
    QString m_suffix;
    double m_value = 0.0;
    QString m_text; ///< cached display text of m_value (formatted once per change)

    // Drag state. The value is accumulated in double precision from the press position so
    // switching modifiers mid-drag never makes the value jump.
    bool m_pressed = false;
    bool m_scrubbing = false;
    int m_lastX = 0;
    int m_pressX = 0;
    double m_dragValue = 0.0;

    QLineEdit* m_editor = nullptr; ///< created lazily on the first text edit
};

} // namespace kk
