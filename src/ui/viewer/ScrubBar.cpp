// SPDX-License-Identifier: GPL-3.0-or-later
// ScrubBar painting and mouse handling.
#include "ui/viewer/ScrubBar.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {
constexpr int kHeight = 20; ///< widget height (px)
constexpr int kSideMargin = 8; ///< track inset, matches the picture margin
constexpr int kTrackHeight = 4;
constexpr int kHeadSize = 5; ///< playhead triangle half-width
const QColor kTrackColor(0x2a, 0x2a, 0x2e);
const QColor kRangeColor(0x4c, 0x9b, 0xe8, 90);
const QColor kMarkColor(0x8c, 0xc4, 0xf8);
const QColor kPlayheadColor(0xe8, 0x4c, 0x4c);
} // namespace

ScrubBar::ScrubBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ViewerScrubBar"));
    setFixedHeight(kHeight);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Drag to scrub"));
}

QSize ScrubBar::sizeHint() const
{
    return QSize(200, kHeight);
}

void ScrubBar::setDuration(int64_t frames)
{
    frames = std::max<int64_t>(0, frames);
    if (frames != m_duration) {
        m_duration = frames;
        update();
    }
}

void ScrubBar::setPosition(int64_t frame)
{
    if (frame != m_position) {
        // Repaint only the old and new playhead columns: during playback this runs every
        // frame, a full repaint of a wide bar is wasted work.
        const int oldX = int(xOf(m_position));
        m_position = frame;
        const int newX = int(xOf(m_position));
        update(QRect(oldX - kHeadSize - 1, 0, 2 * kHeadSize + 3, height()));
        update(QRect(newX - kHeadSize - 1, 0, 2 * kHeadSize + 3, height()));
    }
}

void ScrubBar::setMarks(int64_t in, int64_t out)
{
    if (in != m_in || out != m_out) {
        m_in = in;
        m_out = out;
        update();
    }
}

QRect ScrubBar::trackRect() const
{
    return QRect(kSideMargin, 0, std::max(1, width() - 2 * kSideMargin), height());
}

double ScrubBar::xOf(int64_t frame) const
{
    const QRect t = trackRect();
    if (m_duration <= 0) {
        return t.left();
    }
    return t.left() + double(frame) * t.width() / double(m_duration);
}

int64_t ScrubBar::frameAt(int x) const
{
    const QRect t = trackRect();
    if (m_duration <= 0) {
        return 0;
    }
    const int64_t f = int64_t(std::floor(double(x - t.left()) * double(m_duration) / t.width()));
    return std::clamp<int64_t>(f, 0, m_duration - 1);
}

void ScrubBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect t = trackRect();
    const int midY = height() / 2;
    p.fillRect(QRect(t.left(), midY - kTrackHeight / 2, t.width(), kTrackHeight), kTrackColor);
    if (m_duration <= 0) {
        return;
    }
    // In/out band with brackets.
    if (m_in >= 0 || m_out > 0) {
        const double x0 = xOf(m_in >= 0 ? m_in : 0);
        const double x1 = xOf(m_out > 0 ? m_out : m_duration);
        p.fillRect(QRectF(x0, 2, std::max(1.0, x1 - x0), height() - 4), kRangeColor);
        p.setPen(QPen(kMarkColor, 2));
        if (m_in >= 0) {
            p.drawLine(QPointF(x0 + 1, 2), QPointF(x0 + 1, height() - 2));
            p.drawLine(QPointF(x0 + 1, 2), QPointF(x0 + 5, 2));
            p.drawLine(QPointF(x0 + 1, height() - 2), QPointF(x0 + 5, height() - 2));
        }
        if (m_out > 0) {
            p.drawLine(QPointF(x1 - 1, 2), QPointF(x1 - 1, height() - 2));
            p.drawLine(QPointF(x1 - 1, 2), QPointF(x1 - 5, 2));
            p.drawLine(QPointF(x1 - 1, height() - 2), QPointF(x1 - 5, height() - 2));
        }
    }
    // Playhead: centred on its frame.
    const double frameWidth = double(t.width()) / double(m_duration);
    const double x = xOf(m_position) + std::min(frameWidth, 4.0) / 2.0;
    p.setPen(QPen(kPlayheadColor, 1.5));
    p.drawLine(QPointF(x, 0), QPointF(x, height()));
    p.setPen(Qt::NoPen);
    p.setBrush(kPlayheadColor);
    const QPointF head[] = {{x - kHeadSize, 0}, {x + kHeadSize, 0}, {x, double(kHeadSize)}};
    p.drawPolygon(head, 3);
}

void ScrubBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_duration <= 0) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    Q_EMIT scrubStarted();
    Q_EMIT scrubbed(frameAt(int(event->position().x())));
}

void ScrubBar::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging) {
        Q_EMIT scrubbed(frameAt(int(event->position().x())));
    }
}

void ScrubBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        Q_EMIT scrubFinished();
    }
}

void ScrubBar::wheelEvent(QWheelEvent* event)
{
    const int steps = event->angleDelta().y() / 120;
    if (steps != 0 && m_duration > 0) {
        // Wheel down = forward, like a jog wheel turned towards you.
        Q_EMIT scrubbed(std::clamp<int64_t>(m_position - steps, 0, m_duration - 1));
    }
    event->accept();
}

} // namespace kk
