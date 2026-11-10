// SPDX-License-Identifier: GPL-3.0-or-later
// SpringLoad — "spring-loaded" buttons: while a drag hovers a button for a moment, the button
// is pressed for the user (spec 71). Used by the page bar; any QAbstractButton can use it.
#pragma once

#include <QObject>
#include <QPointer>

class QAbstractButton;
class QTimer;
class QWidget;

namespace kk {

/// How long a drag has to rest on a spring-loaded button before it triggers, in milliseconds.
/// Long enough not to fire while the pointer only crosses the button, short enough to feel
/// like a deliberate "go there" (Finder opens spring-loaded folders after about a second).
inline constexpr int kSpringLoadDelayMs = 900;

/// Event filter that makes @p button spring-loaded: it accepts every drag that enters it (no
/// forbidden cursor), shows a highlight with a fill that sweeps across the button over
/// kSpringLoadDelayMs, and clicks the button when the sweep is complete (the drag goes on, so
/// the user can drop on what the click opened). Leaving the button cancels. A checked button
/// (the current page) accepts the drag but shows nothing and does nothing. A drop on the
/// button clicks it at once and is ignored (nothing is imported). The sweep is time based
/// (uianim::nowMs()); with the UI animations off the button shows a plain highlight instead.
/// Owned by the button (deleted with it).
class SpringLoad : public QObject
{
    Q_OBJECT
public:
    /// Installs the filter on @p button and lets it accept drops.
    explicit SpringLoad(QAbstractButton* button);

    /// Progress of the sweep, 0 when idle, 1 when it fires.
    [[nodiscard]] double progress() const { return m_progress; }
    /// True while a drag rests on the button and the countdown runs.
    [[nodiscard]] bool active() const { return m_active; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    class Overlay;
    void start();
    void cancel();
    void tick();

    QAbstractButton* m_button;
    Overlay* m_overlay = nullptr;
    QTimer* m_timer = nullptr;
    bool m_active = false;
    qint64 m_startMs = 0;
    double m_progress = 0;
};

} // namespace kk
