// SPDX-License-Identifier: GPL-3.0-or-later
// UiAnimation implementation: one process-wide state (GUI thread), read lazily on first use.
#include "ui/common/UiAnimation.h"

#include "core/Preferences.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace kk::uianim {

namespace {

constexpr double kMinSpeed = 0.25;
constexpr double kMaxSpeed = 4.0;
/// Overshoot of the "pop" OutBack curve: Qt's default is 1.70158; 0.8 keeps the overshoot
/// under ~3 % so a settling clip never visibly bounces.
constexpr double kPopOvershoot = 0.8;

/// Durations at speed 1 (ms), by Kind.
constexpr int kBaseMs[] = {
    180, // Move
    160, // Pop
    180, // Flash
    140, // Glide
    120, // Fade
};

struct State
{
    bool loaded = false;
    bool enabled = false;
    double speed = 1.0;
    QElapsedTimer timer;
    std::function<int64_t()> clock;
};

State& state()
{
    static State s;
    return s;
}

/// KDE's global animation speed (System Settings > Animation speed): kdeglobals [KDE]
/// AnimationDurationFactor, 0 = instant (no animations), 1 = normal, larger = slower. 1 when
/// unset or not on KDE.
double desktopDurationFactor()
{
    const QString path = QStandardPaths::locate(
        QStandardPaths::GenericConfigLocation, QStringLiteral("kdeglobals"));
    if (path.isEmpty())
        return 1.0;
    const QSettings kde(path, QSettings::IniFormat);
    bool ok = false;
    const double f = kde.value(QStringLiteral("KDE/AnimationDurationFactor")).toDouble(&ok);
    return ok && f >= 0.0 ? f : 1.0;
}

void load(State& s)
{
    s.loaded = true;
    // Offscreen (the tests) defaults to off so no UI test depends on timing; the animation
    // test opts in with KK_ANIMATIONS=1.
    const bool offscreen = QGuiApplication::platformName() == QLatin1String("offscreen")
        && !qEnvironmentVariableIsSet("KK_ANIMATIONS");
    const double desktop = desktopDurationFactor();
    s.enabled = prefs::animationsEnabled() && !offscreen && desktop > 0.0
        && !qEnvironmentVariableIsSet("KK_NO_ANIMATIONS");
    const double user = prefs::animationSpeed();
    s.speed = std::clamp(desktop > 0.0 ? user / desktop : user, kMinSpeed, kMaxSpeed);
}

State& loaded()
{
    State& s = state();
    if (!s.loaded)
        load(s);
    return s;
}

} // namespace

bool enabled()
{
    return loaded().enabled;
}

void setEnabled(bool on)
{
    State& s = loaded();
    if (s.enabled == on)
        return;
    s.enabled = on;
    emit Notifier::instance() -> changed();
}

double speed()
{
    return loaded().speed;
}

void setSpeed(double factor)
{
    State& s = loaded();
    s.speed = std::clamp(factor, kMinSpeed, kMaxSpeed);
    emit Notifier::instance() -> changed();
}

void reload()
{
    load(state());
    emit Notifier::instance() -> changed();
}

int duration(Kind kind)
{
    return std::max(1, int(std::lround(kBaseMs[int(kind)] / speed())));
}

const QEasingCurve& easing(Kind kind)
{
    static const QEasingCurve move(QEasingCurve::OutCubic);
    static const QEasingCurve pop = [] {
        QEasingCurve c(QEasingCurve::OutBack);
        c.setOvershoot(kPopOvershoot);
        return c;
    }();
    static const QEasingCurve flash(QEasingCurve::OutQuad);
    static const QEasingCurve linear(QEasingCurve::Linear);
    switch (kind) {
    case Kind::Move:
    case Kind::Glide:
        return move;
    case Kind::Pop:
        return pop;
    case Kind::Flash:
        return flash;
    case Kind::Fade:
        break;
    }
    return linear;
}

double ease(Kind kind, double t)
{
    return easing(kind).valueForProgress(std::clamp(t, 0.0, 1.0));
}

int64_t nowMs()
{
    State& s = state();
    if (s.clock)
        return s.clock();
    if (!s.timer.isValid())
        s.timer.start();
    return s.timer.elapsed();
}

void setClockForTesting(std::function<int64_t()> clock)
{
    state().clock = std::move(clock);
}

// ---- PlayheadGlide ----

namespace {
constexpr int64_t kIsolatedMs = 100; ///< closer playhead moves are a drag / auto-repeat
constexpr double kMinGlidePx = 6.0; ///< shorter jumps do not glide
} // namespace

bool PlayheadGlide::moved(int64_t fromFrame, int64_t toFrame, bool allowed, double pixelsPerFrame)
{
    const int64_t now = nowMs();
    const bool isolated = now - m_lastMoveMs > kIsolatedMs;
    m_lastMoveMs = now;
    if (!allowed || !isolated
        || std::abs(double(toFrame - fromFrame)) * pixelsPerFrame < kMinGlidePx) {
        m_running = false;
        return false;
    }
    // A glide that is still running continues from where the line is drawn.
    const double from = running(now) ? frame(fromFrame, now) : double(fromFrame);
    m_running = true;
    m_from = from;
    m_startMs = now;
    return true;
}

double PlayheadGlide::frame(int64_t modelFrame, int64_t now) const
{
    if (!running(now))
        return double(modelFrame);
    const double t = double(now - m_startMs) / duration(Kind::Glide);
    const double e = ease(Kind::Glide, t);
    return m_from + (double(modelFrame) - m_from) * e;
}

bool PlayheadGlide::running(int64_t now) const
{
    return m_running && now - m_startMs < duration(Kind::Glide);
}

Notifier* Notifier::instance()
{
    static Notifier* n = new Notifier; // lives until exit (no parent: no static destruction order)
    return n;
}

} // namespace kk::uianim
