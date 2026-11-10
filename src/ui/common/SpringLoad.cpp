// SPDX-License-Identifier: GPL-3.0-or-later
// SpringLoad implementation.
#include "ui/common/SpringLoad.h"

#include "ui/common/UiAnimation.h"

#include <QAbstractButton>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QPainter>
#include <QTimer>

#include <algorithm>

namespace kk {

namespace {
constexpr int kTickMs = 16;
constexpr int kHighlightAlpha = 60;
constexpr int kFillAlpha = 110;
constexpr int kBarHeight = 3;
} // namespace

/// Transparent child of the button that draws the feedback over the button's own painting.
class SpringLoad::Overlay : public QWidget
{
public:
    Overlay(QWidget* parent, const SpringLoad* owner)
        : QWidget(parent)
        , m_owner(owner)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        hide();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QColor accent = palette().color(QPalette::Highlight);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        // Highlight the whole button: the drag is "on" it.
        accent.setAlpha(kHighlightAlpha);
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawRoundedRect(r, 4, 4);
        // The sweep: a fill growing from the left edge to the right edge, with a bar at the
        // bottom so the progress is readable on any scheme.
        const double w = r.width() * m_owner->progress();
        accent.setAlpha(kFillAlpha);
        p.setBrush(accent);
        p.setClipRect(QRectF(r.left(), r.top(), w, r.height()));
        p.drawRoundedRect(r, 4, 4);
        accent.setAlpha(255);
        p.setBrush(accent);
        p.drawRect(QRectF(r.left(), r.bottom() - kBarHeight, w, kBarHeight));
    }

private:
    const SpringLoad* m_owner;
};

SpringLoad::SpringLoad(QAbstractButton* button)
    : QObject(button)
    , m_button(button)
{
    m_button->setAcceptDrops(true);
    m_button->installEventFilter(this);
    m_overlay = new Overlay(m_button, this);
    m_timer = new QTimer(this);
    m_timer->setInterval(kTickMs);
    connect(m_timer, &QTimer::timeout, this, &SpringLoad::tick);
}

bool SpringLoad::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_button)
        return false;
    switch (event->type()) {
    case QEvent::DragEnter:
        // Accept any drag (files from a file manager, the app's own clips): the cursor is
        // never "forbidden" over a page button.
        static_cast<QDragEnterEvent*>(event)->acceptProposedAction();
        start();
        return true;
    case QEvent::DragMove:
        static_cast<QDragMoveEvent*>(event)->acceptProposedAction();
        return true;
    case QEvent::DragLeave:
        cancel();
        return true;
    case QEvent::Drop:
        // The drop only switches the page at once; nothing is imported.
        cancel();
        if (!m_button->isChecked())
            m_button->click();
        event->ignore();
        return true;
    case QEvent::Resize:
        m_overlay->setGeometry(m_button->rect());
        break;
    default:
        break;
    }
    return false;
}

void SpringLoad::start()
{
    cancel();
    if (!m_button->isEnabled() || m_button->isChecked())
        return; // the current page: nothing to switch to
    m_active = true;
    m_startMs = uianim::nowMs();
    m_progress = 0;
    m_overlay->setGeometry(m_button->rect());
    m_overlay->raise();
    m_overlay->show();
    m_timer->start();
}

void SpringLoad::cancel()
{
    m_active = false;
    m_progress = 0;
    m_timer->stop();
    m_overlay->hide();
}

void SpringLoad::tick()
{
    if (!m_active)
        return;
    const double t = double(uianim::nowMs() - m_startMs) / kSpringLoadDelayMs;
    if (t >= 1.0) {
        cancel();
        m_button->click(); // same as the user's click: the drag goes on over the new page
        return;
    }
    // Animations off (reduced motion): keep the highlight, no moving sweep.
    m_progress = uianim::enabled() ? std::clamp(t, 0.0, 1.0) : 0.0;
    m_overlay->update();
}

} // namespace kk
