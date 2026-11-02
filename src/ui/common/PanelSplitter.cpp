// SPDX-License-Identifier: GPL-3.0-or-later
// PanelSplitter implementation: a handle that paints its own hover / press highlight.
#include "ui/common/PanelSplitter.h"

#include <QMouseEvent>
#include <QPainter>

namespace kk {

namespace {

constexpr int kLinePx = 1; ///< drawn separator line when idle
constexpr int kActiveLinePx = 3; ///< drawn line while hovered / dragged

/// Splitter handle with a visible hover and press state. Breeze does not draw splitter
/// handles at all (and QSS ":hover" on QSplitter::handle is unreliable because handles have
/// no WA_Hover by default), so it paints itself: the panel background, a 1 px separator line,
/// and a 3 px highlight-coloured bar while active.
///
/// It also keeps Breeze's "splitter proxy" away: for thin handles Breeze shows an invisible
/// 24 px widget over the handle on hover and calls QWidget::grabMouse() on it when pressed.
/// Qt Wayland refuses explicit grabs outside popups ("This plugin supports grabbing the mouse
/// only for popup windows", twice per splitter drag: grab and release - the owner's log had
/// six). The handle filters its own hover events ahead of Breeze's filter, so the proxy never
/// appears; the drag itself is QSplitterHandle's normal press / move / release, which needs no
/// explicit grab.
class PanelSplitterHandle final : public QSplitterHandle
{
public:
    PanelSplitterHandle(Qt::Orientation orientation, QSplitter* parent)
        : QSplitterHandle(orientation, parent)
    {
        setAttribute(Qt::WA_Hover);
        // QSplitterHandle already sets SplitHCursor / SplitVCursor from the orientation.
    }

protected:
    bool event(QEvent* event) override
    {
        // QWidget::event(Polish) runs the style's polish(), where Breeze installs its filter;
        // QApplication::setStyle() polishes existing widgets directly and then sends
        // StyleChange. Installing ours after either puts it first: the last installed filter
        // runs first.
        const bool result = QSplitterHandle::event(event);
        if (event->type() == QEvent::Polish || event->type() == QEvent::StyleChange) {
            removeEventFilter(this);
            installEventFilter(this);
        }
        return result;
    }
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched != this)
            return QSplitterHandle::eventFilter(watched, event);
        switch (event->type()) {
        case QEvent::HoverEnter:
        case QEvent::HoverMove:
            if (!m_hover) {
                m_hover = true;
                update();
            }
            return true; // consumed: Breeze's splitter proxy must not see it
        case QEvent::HoverLeave:
            m_hover = false;
            update();
            return true;
        default:
            return false;
        }
    }
    void mousePressEvent(QMouseEvent* event) override
    {
        m_pressed = event->button() == Qt::LeftButton;
        update();
        QSplitterHandle::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        m_pressed = false;
        update();
        QSplitterHandle::mouseReleaseEvent(event);
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QPalette& pal = palette();
        p.fillRect(rect(), pal.color(QPalette::Window));
        const bool active = m_hover || m_pressed;
        const int line = active ? kActiveLinePx : kLinePx;
        QColor color = active ? pal.color(QPalette::Highlight) : pal.color(QPalette::Mid);
        if (m_hover && !m_pressed)
            color.setAlphaF(0.7f);
        QRect bar = rect();
        if (orientation() == Qt::Horizontal) // vertical line between left and right panels
            bar = QRect(bar.center().x() - line / 2, bar.top(), line, bar.height());
        else
            bar = QRect(bar.left(), bar.center().y() - line / 2, bar.width(), line);
        p.fillRect(bar, color);
    }

private:
    bool m_hover = false;
    bool m_pressed = false;
};

} // namespace

PanelSplitter::PanelSplitter(Qt::Orientation orientation, QWidget* parent)
    : QSplitter(orientation, parent)
{
    setObjectName(QStringLiteral("PanelSplitter"));
    setChildrenCollapsible(false);
    setHandleWidth(kHandleGrabPx);
}

QSplitterHandle* PanelSplitter::createHandle()
{
    return new PanelSplitterHandle(orientation(), this);
}

} // namespace kk
