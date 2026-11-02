// SPDX-License-Identifier: GPL-3.0-or-later
// UiAnimation — the shared switch, durations, easing curves and clock of the small UI
// animations (v2.2: timeline edits, playhead glides, the snap indicator).
#pragma once

#include <QEasingCurve>
#include <QObject>

#include <cstdint>
#include <functional>

namespace kk::uianim {

/// What is animated; each kind has its own standard duration (duration()).
enum class Kind {
    Move, ///< clips sliding to their new place (ripple, trims, undo), OutCubic
    Pop, ///< a clip appearing or settling (insert, drop), light OutBack
    Flash, ///< the blade flash / trim edge flash, fades out
    Glide, ///< the playhead line gliding to a jump target, OutCubic
    Fade ///< the snap indicator fading in or out, linear
};

/// True if the animations run. False when the user turned them off (prefs::kAnimations), the
/// desktop asks for no animations (KDE "Animation speed" set to instant), on the offscreen
/// platform (tests) unless KK_ANIMATIONS is set or a test turned them on with setEnabled(), or
/// with the environment variable KK_NO_ANIMATIONS set. Off means every animated view shows the
/// model state at once.
[[nodiscard]] bool enabled();
/// Overrides the switch at run time (not stored; tests and the preference page's live apply).
/// Emits Notifier::changed() if it changed.
void setEnabled(bool on);

/// Speed factor: the user's prefs::kAnimationSpeed times the desktop's speed (KDE
/// AnimationDurationFactor); durations are divided by it.
[[nodiscard]] double speed();
/// Overrides the speed factor at run time (clamped to [0.25, 4]); emits Notifier::changed().
void setSpeed(double factor);

/// Rereads prefs::kAnimations, prefs::kAnimationSpeed and the desktop settings (start-up and
/// after the Preferences dialog applies); emits Notifier::changed().
void reload();

/// Standard duration of @p kind in milliseconds at the current speed (about 120..220 ms at
/// speed 1; at least 1 ms).
[[nodiscard]] int duration(Kind kind);
/// Standard easing curve of @p kind.
[[nodiscard]] const QEasingCurve& easing(Kind kind);
/// Eased progress of @p kind at linear progress @p t (clamped to [0, 1]).
[[nodiscard]] double ease(Kind kind, double t);

/// Animation clock in milliseconds (monotonic; QElapsedTimer unless a test installed one).
[[nodiscard]] int64_t nowMs();
/// Installs a test clock (deterministic animation tests); an empty function restores the
/// real clock.
void setClockForTesting(std::function<int64_t()> clock);

/// The drawn position of a playhead line that glides to jump targets (Kind::Glide). Value type;
/// the owner repaints while running() and draws frame(). Never changes the playhead itself.
class PlayheadGlide
{
public:
    /// The playhead moved from @p fromFrame to @p toFrame. Starts a glide when @p allowed
    /// (animations on, not playing, not scrubbing), the jump is isolated (no other move within
    /// the last 100 ms, so drags and key auto-repeat stay instant) and at least 6 px long at
    /// @p pixelsPerFrame; a running glide continues from where the line is drawn. Otherwise
    /// stops a running glide. Returns whether a glide runs.
    bool moved(int64_t fromFrame, int64_t toFrame, bool allowed, double pixelsPerFrame);
    /// Fractional frame to draw for the model playhead @p modelFrame at time @p nowMs.
    [[nodiscard]] double frame(int64_t modelFrame, int64_t nowMs) const;
    /// True if a glide runs at @p nowMs.
    [[nodiscard]] bool running(int64_t nowMs) const;
    /// Ends a running glide.
    void stop() { m_running = false; }

private:
    bool m_running = false;
    double m_from = 0;
    int64_t m_startMs = 0;
    int64_t m_lastMoveMs = -1000000;
};

/// Emits changed() when enabled() or speed() changed (views stop their animations at once).
class Notifier : public QObject
{
    Q_OBJECT
public:
    /// The process-wide instance (GUI thread).
    static Notifier* instance();

signals:
    void changed();
};

} // namespace kk::uianim
