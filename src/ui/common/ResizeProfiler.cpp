// SPDX-License-Identifier: GPL-3.0-or-later
// ResizeProfiler — times each live-resize step from the window's resize event to its frame.
#include "ui/common/ResizeProfiler.h"

#include <QCoreApplication>
#include <QEvent>
#include <QLoggingCategory>
#include <QResizeEvent>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <algorithm>

namespace kk {

namespace {

Q_LOGGING_CATEGORY(lcResize, "kk.ui.resize", QtInfoMsg)
/// Every timed event with its start time (where the gaps between steps are).
Q_LOGGING_CATEGORY(lcResizeEvents, "kk.ui.resize.events", QtInfoMsg)

constexpr int kSummaryDelayMs = 300; ///< a gesture ended when no new size came for this long
constexpr double kFrameBudgetMs = 16.0; ///< one refresh at 60 Hz
/// An expose / update request that took longer than this painted something (an empty one, with
/// nothing dirty, takes a few microseconds).
constexpr qint64 kPaintedNs = 100'000;
/// Paints this long after a resize event belong to other things (playback, a timer).
constexpr qint64 kStepNs = 100'000'000;

double percentile(QVector<double> values, double p)
{
    if (values.isEmpty())
        return 0;
    std::sort(values.begin(), values.end());
    const auto index = qsizetype(p * double(values.size() - 1) + 0.5);
    return values[std::clamp<qsizetype>(index, 0, values.size() - 1)];
}

} // namespace

bool ResizeProfiler::loggingEnabled()
{
    return lcResize().isDebugEnabled();
}

ResizeProfiler* ResizeProfiler::attach(QWidget* window, bool force)
{
    if (!window || !(force || loggingEnabled()))
        return nullptr;
    // One per window (the app attaches one with the logging rule; a test may force another).
    if (auto* existing = window->findChild<ResizeProfiler*>(QString(), Qt::FindDirectChildrenOnly))
        return existing;
    return new ResizeProfiler(window);
}

ResizeProfiler::ResizeProfiler(QWidget* window)
    : QObject(window)
    , m_widget(window)
{
    m_clock.start();
    m_summaryTimer = new QTimer(this);
    m_summaryTimer->setSingleShot(true);
    m_summaryTimer->setInterval(kSummaryDelayMs);
    connect(m_summaryTimer, &QTimer::timeout, this, &ResizeProfiler::logSummary);
    // The QWindow exists only once the widget is created (shown); it can also be re-created.
    window->installEventFilter(this);
    attachWindow();
}

void ResizeProfiler::attachWindow()
{
    QWindow* handle = m_widget ? m_widget->windowHandle() : nullptr;
    if (handle == m_window)
        return;
    if (m_window)
        m_window->removeEventFilter(this);
    m_window = handle;
    if (m_window)
        m_window->installEventFilter(this);
}

bool ResizeProfiler::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    const bool onWidget = watched == m_widget;
    if (onWidget && (type == QEvent::WinIdChange || type == QEvent::Show))
        attachWindow();
    if (!onWidget && watched != m_window)
        return false;
    const bool resize = type == QEvent::Resize;
    // A QWindow also gets QEvent::Paint when the platform asks for a synchronous repaint.
    const bool windowPaint = type == QEvent::Paint && !onWidget;
    if (!resize && !windowPaint && type != QEvent::Expose && type != QEvent::UpdateRequest)
        return false;

    if (m_resending)
        return false; // the timed delivery below

    // Deliver the event again from here, timed, and stop the original delivery. A window
    // manager's resize reaches the QWindow first and the widget's own resize event (the layout)
    // inside it; a programmatic QWidget::resize() only the widget: m_depth makes the outermost
    // event the step.
    const qint64 start = m_clock.nsecsElapsed();
    ++m_depth;
    m_resending = true;
    QCoreApplication::sendEvent(watched, event);
    m_resending = false;
    --m_depth;
    const qint64 end = m_clock.nsecsElapsed();
    if (m_depth > 0 || !m_widget)
        return true;
    qCDebug(lcResizeEvents, "t=%.1f ms %s on %s: %.2f ms", start / 1e6,
        resize                       ? "resize"
            : windowPaint            ? "paint"
            : type == QEvent::Expose ? "expose"
                                     : "update request",
        onWidget ? "widget" : "window", (end - start) / 1e6);

    if (resize) {
        const QSize size = m_widget->size();
        if (!m_steps.isEmpty() && m_steps.constLast().size == size)
            return true;
        finishStep();
        if (!m_summaryTimer->isActive())
            m_gestureStart = int(m_steps.size());
        // On Wayland (and with OpenGL composition) Qt paints and commits inside the resize
        // event already; otherwise the paint follows as an expose / update request.
        const double ms = (end - start) / 1e6;
        m_steps.append({size, ms, ms});
        m_stepStartNs = start;
        m_summaryTimer->start();
    } else if (m_stepStartNs >= 0 && end - start > kPaintedNs && start - m_stepStartNs < kStepNs
        && m_widget->isVisible()) {
        m_steps.last().frameMs = (end - m_stepStartNs) / 1e6;
    }
    return true;
}

void ResizeProfiler::finishStep()
{
    if (m_stepStartNs < 0)
        return;
    m_stepStartNs = -1;
    const Step& step = m_steps.constLast();
    qCDebug(lcResize).noquote() << QStringLiteral(
        "resize step %1x%2: resize event %3 ms, frame %4 ms")
                                       .arg(step.size.width())
                                       .arg(step.size.height())
                                       .arg(step.resizeMs, 0, 'f', 1)
                                       .arg(step.frameMs, 0, 'f', 1);
}

ResizeProfiler::Summary ResizeProfiler::summarize(const QVector<Step>& steps)
{
    Summary s;
    QVector<double> frames;
    QVector<double> layouts;
    for (const Step& step : steps) {
        frames.append(step.frameMs);
        layouts.append(step.resizeMs);
        if (step.frameMs > kFrameBudgetMs)
            ++s.over16Ms;
    }
    s.steps = int(frames.size());
    s.medianMs = percentile(frames, 0.5);
    s.p90Ms = percentile(frames, 0.9);
    s.maxMs = percentile(frames, 1.0);
    s.medianResizeMs = percentile(layouts, 0.5);
    return s;
}

void ResizeProfiler::logSummary()
{
    finishStep();
    if (!loggingEnabled())
        return;
    const Summary s = summarize(m_steps.mid(m_gestureStart));
    qCDebug(lcResize).noquote() << QStringLiteral(
        "resize gesture: %1 steps, frame median %2 ms / p90 %3 ms / max %4 ms, "
        "%5 over 16 ms; resize event median %6 ms")
                                       .arg(s.steps)
                                       .arg(s.medianMs, 0, 'f', 1)
                                       .arg(s.p90Ms, 0, 'f', 1)
                                       .arg(s.maxMs, 0, 'f', 1)
                                       .arg(s.over16Ms)
                                       .arg(s.medianResizeMs, 0, 'f', 1);
}

} // namespace kk
