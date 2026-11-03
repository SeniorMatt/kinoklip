// SPDX-License-Identifier: GPL-3.0-or-later
// LiveResize — Wayland swap interval and the redundant expose after a resize-driven frame.
#include "ui/common/LiveResize.h"

#include <QEvent>
#include <QGuiApplication>
#include <QPaintEvent>
#include <QPointer>
#include <QSurfaceFormat>
#include <QWidget>
#include <QWindow>

namespace kk::liveresize {

namespace {

bool onWayland()
{
    return QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
}

/// Drops the expose event Qt's Wayland plugin sends after applying a configure, when the window
/// has already painted (and so committed) the whole window at that size while handling the
/// resize: the expose would only flush the same pixels again, which with OpenGL composition is
/// a full-window composition, texture upload and buffer swap per resize step.
///
/// Only an expose of a window that was exposed before, is exposed now, has the size of the
/// frame just painted, and is the first expose after that frame is dropped; anything else
/// (first show, unminimise, output change, a resize without a paint) is delivered. Dropping it
/// changes no state: QGuiApplication updates QWindow::isExposed() before delivering the event.
class RedundantExposeFilter : public QObject
{
public:
    explicit RedundantExposeFilter(QWidget* widget)
        : QObject(widget)
        , m_widget(widget)
    {
        widget->installEventFilter(this);
        attachWindow();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        const QEvent::Type type = event->type();
        if (watched == m_widget) {
            if (type == QEvent::WinIdChange || type == QEvent::Show)
                attachWindow();
            // The top level paints its whole area in every full repaint (its background);
            // a paint event right after a resize means this size is being painted + flushed.
            if (type == QEvent::Paint && m_resized
                && static_cast<QPaintEvent*>(event)->rect().contains(m_widget->rect())) {
                m_paintedSize = m_widget->size();
                m_resized = false;
            }
            return false;
        }
        if (watched != m_window)
            return false;
        if (type == QEvent::Resize) {
            m_resized = true;
            m_paintedSize = QSize();
        } else if (type == QEvent::Expose) {
            const bool exposed = m_window->isExposed();
            const bool redundant = exposed && m_wasExposed && m_paintedSize.isValid()
                && m_paintedSize == m_widget->size();
            m_wasExposed = exposed;
            m_paintedSize = QSize(); // at most one expose per frame
            if (redundant)
                return true;
        }
        return false;
    }

private:
    void attachWindow()
    {
        QWindow* handle = m_widget ? m_widget->windowHandle() : nullptr;
        if (handle == m_window)
            return;
        if (m_window)
            m_window->removeEventFilter(this);
        m_window = handle;
        m_wasExposed = m_window && m_window->isExposed();
        if (m_window)
            m_window->installEventFilter(this);
    }

    QPointer<QWidget> m_widget;
    QPointer<QWindow> m_window;
    bool m_resized = false; ///< a resize came, its full paint not yet
    bool m_wasExposed = false; ///< exposed before the current expose event
    QSize m_paintedSize; ///< size of the full paint after the last resize (invalid: none)
};

} // namespace

void adjustSwapInterval()
{
    const bool overridden = qEnvironmentVariableIsSet("KK_SWAP_INTERVAL");
    if (!overridden && !onWayland())
        return;
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSwapInterval(overridden ? qEnvironmentVariableIntValue("KK_SWAP_INTERVAL") : 0);
    QSurfaceFormat::setDefaultFormat(format);
}

void install(QWidget* window, bool force)
{
    if (window && (force || onWayland()) && !qEnvironmentVariableIsSet("KK_KEEP_RESIZE_EXPOSE"))
        new RedundantExposeFilter(window);
}

} // namespace kk::liveresize
