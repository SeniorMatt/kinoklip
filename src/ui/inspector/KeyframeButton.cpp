// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeButton / KeyframeArrowButton painting.
#include "ui/inspector/KeyframeButton.h"

#include <QPainter>
#include <QPainterPath>

namespace kk {

namespace {
constexpr int kDiamondButtonSize = 18;
constexpr int kArrowButtonWidth = 11;
constexpr qreal kDiamondRadius = 5.0; ///< half diagonal of the diamond in px
} // namespace

KeyframeButton::KeyframeButton(QWidget* parent)
    : QAbstractButton(parent)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setToolTip(tr("Add or remove a keyframe at the playhead"));
}

void KeyframeButton::setState(State state)
{
    if (state == m_state)
        return;
    m_state = state;
    update();
}

QSize KeyframeButton::sizeHint() const
{
    return {kDiamondButtonSize, kDiamondButtonSize};
}

void KeyframeButton::enterEvent(QEnterEvent* event)
{
    m_hover = true;
    update();
    QAbstractButton::enterEvent(event);
}

void KeyframeButton::leaveEvent(QEvent* event)
{
    m_hover = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void KeyframeButton::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF c = QRectF(rect()).center();
    const qreal r = kDiamondRadius;
    QPainterPath diamond;
    diamond.moveTo(c.x(), c.y() - r);
    diamond.lineTo(c.x() + r, c.y());
    diamond.lineTo(c.x(), c.y() + r);
    diamond.lineTo(c.x() - r, c.y());
    diamond.closeSubpath();

    const QPalette& pal = palette();
    const QColor accent = pal.color(QPalette::Highlight);
    QColor outline = pal.color(QPalette::WindowText);
    switch (m_state) {
    case State::NotAnimated:
        outline.setAlphaF(m_hover ? 0.9f : 0.55f);
        p.setPen(QPen(outline, 1.2));
        p.setBrush(Qt::NoBrush);
        break;
    case State::Animated:
        p.setPen(QPen(m_hover ? accent.lighter(130) : accent, 1.4));
        p.setBrush(Qt::NoBrush);
        break;
    case State::OnKey:
        p.setPen(QPen(accent.lighter(m_hover ? 140 : 115), 1.2));
        p.setBrush(accent);
        break;
    }
    if (!isEnabled())
        p.setOpacity(0.5);
    p.drawPath(diamond);
}

KeyframeArrowButton::KeyframeArrowButton(bool forward, QWidget* parent)
    : QAbstractButton(parent)
    , m_forward(forward)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setToolTip(forward ? tr("Next keyframe") : tr("Previous keyframe"));
}

QSize KeyframeArrowButton::sizeHint() const
{
    return {kArrowButtonWidth, kDiamondButtonSize};
}

void KeyframeArrowButton::enterEvent(QEnterEvent* event)
{
    m_hover = true;
    update();
    QAbstractButton::enterEvent(event);
}

void KeyframeArrowButton::leaveEvent(QEvent* event)
{
    m_hover = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void KeyframeArrowButton::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor color = palette().color(QPalette::WindowText);
    color.setAlphaF(!isEnabled() ? 0.25f : (m_hover ? 0.95f : 0.7f));
    const QPointF c = QRectF(rect()).center();
    constexpr qreal w = 3.0; // half width / height of the triangle
    QPainterPath tri;
    if (m_forward) {
        tri.moveTo(c.x() - w + 1, c.y() - w);
        tri.lineTo(c.x() + w - 0.5, c.y());
        tri.lineTo(c.x() - w + 1, c.y() + w);
    } else {
        tri.moveTo(c.x() + w - 1, c.y() - w);
        tri.lineTo(c.x() - w + 0.5, c.y());
        tri.lineTo(c.x() + w - 1, c.y() + w);
    }
    tri.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(tri);
}

} // namespace kk
