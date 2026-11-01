// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeButton — the diamond toggle next to an animatable parameter, plus the small
// previous/next-key arrow buttons.
#pragma once

#include <QAbstractButton>

namespace kk {

/// A self-painted diamond showing the keyframe state of one parameter at the playhead:
/// - NotAnimated: dim outline (clicking adds the first key),
/// - Animated: bright outline (the parameter has keys, but none at the playhead),
/// - OnKey: filled with the accent colour (clicking removes the key).
///
/// The button never changes its own state; the owner calls setState() after the model
/// changed, so the diamond always reflects the project.
class KeyframeButton : public QAbstractButton
{
    Q_OBJECT
public:
    /// Keyframe state of the parameter at the current frame.
    enum class State {
        NotAnimated,
        Animated,
        OnKey
    };

    /// Creates a diamond in the NotAnimated state.
    explicit KeyframeButton(QWidget* parent = nullptr);

    /// Current state.
    [[nodiscard]] State state() const { return m_state; }
    /// Changes the state (repaints only when it changed).
    void setState(State state);

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    State m_state = State::NotAnimated;
    bool m_hover = false;
};

/// A tiny triangle arrow button used for "go to previous / next keyframe".
class KeyframeArrowButton : public QAbstractButton
{
    Q_OBJECT
public:
    /// Creates an arrow pointing left (@p forward false) or right.
    explicit KeyframeArrowButton(bool forward, QWidget* parent = nullptr);

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    bool m_forward = true;
    bool m_hover = false;
};

} // namespace kk
