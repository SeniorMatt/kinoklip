// SPDX-License-Identifier: GPL-3.0-or-later
// ResizeProfiler — per-step timing of live window resizing (layout, and resize to frame).
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QVector>

class QTimer;
class QWidget;
class QWindow;

namespace kk {

/// Measures what one step of an interactive resize costs the application: a window manager's
/// live resize sends the window a new size (a Wayland configure, an X11 ConfigureNotify) and
/// on Wayland the compositor shows the new size only after the client committed a frame at it.
///
/// For each size the window receives, it records the time the resize event took (layout of
/// every visible widget) and the time from the resize event to the end of the first paint +
/// flush at that size (QWidget repaint, OpenGL composition of QOpenGLWidgets, the buffer
/// commit; on Wayland the flush may also wait for the compositor's frame callback). With the
/// logging category `kk.ui.resize` at debug level (QT_LOGGING_RULES=kk.ui.resize.debug=true)
/// every step is logged, and a summary (median, 90th percentile, worst, steps over 16 ms)
/// 300 ms after the last step of a gesture.
///
/// It works by filtering the window's (QWidget and QWindow) resize / expose / update-request
/// events and delivering each again itself, timed, instead of the original delivery: it is
/// only attached when enabled (the window loses no events, but event filters installed on it
/// after the profiler see those three event types twice).
///
/// Thread-safety: GUI thread only.
class ResizeProfiler : public QObject
{
    Q_OBJECT
public:
    /// One size the window was resized to.
    struct Step
    {
        QSize size; ///< new window size, logical pixels
        double resizeMs = 0; ///< handling the resize event (layout)
        double frameMs = -1; ///< resize event start to the end of the first frame (-1: none yet)
    };
    /// Distribution of Step::frameMs over a set of steps.
    struct Summary
    {
        int steps = 0;
        double medianMs = 0;
        double p90Ms = 0;
        double maxMs = 0;
        double medianResizeMs = 0; ///< median of Step::resizeMs
        int over16Ms = 0; ///< steps whose frame came later than one 60 Hz refresh
    };

    /// True if the `kk.ui.resize` logging category is enabled at debug level.
    [[nodiscard]] static bool loggingEnabled();
    /// Attaches a profiler to the top-level @p window if loggingEnabled() (or @p force); it is
    /// a child of @p window. Returns it, or nullptr when not enabled.
    static ResizeProfiler* attach(QWidget* window, bool force = false);

    /// Steps recorded since the last reset(), oldest first.
    [[nodiscard]] const QVector<Step>& steps() const { return m_steps; }
    /// Forgets the recorded steps.
    void reset() { m_steps.clear(); }
    /// Statistics of the steps with a frame (Step::frameMs >= 0).
    [[nodiscard]] static Summary summarize(const QVector<Step>& steps);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit ResizeProfiler(QWidget* window);
    void attachWindow();
    void logSummary();

    QPointer<QWidget> m_widget;
    QPointer<QWindow> m_window;
    QVector<Step> m_steps;
    QElapsedTimer m_clock;
    bool m_resending = false; ///< inside the timed re-delivery of an event
    int m_depth = 0; ///< nesting of timed events (QWindow resize -> widget resize)
    qint64 m_stepStartNs = -1; ///< start of the step waiting for its frame, -1: none
    int m_gestureStart = 0; ///< index in m_steps of the current gesture's first step
    QTimer* m_summaryTimer = nullptr;
};

} // namespace kk
