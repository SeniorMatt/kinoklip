// SPDX-License-Identifier: GPL-3.0-or-later
// SourceTapeBar implementation.
#include "ui/cut/SourceTapeBar.h"

#include "ui/common/SchemeColors.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {
constexpr int kBarHeight = 22;
constexpr int kSidePadding = 4;
constexpr int kMinNameWidth = 30; ///< blocks narrower than this show no name
} // namespace

SourceTapeBar::SourceTapeBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("SourceTapeBar"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setToolTip(tr("Source tape: the clips of the bin, back to back. Click to jump."));
    connect(SchemeColorsNotifier::instance(), &SchemeColorsNotifier::changed, this,
        qOverload<>(&QWidget::update));
}

QSize SourceTapeBar::sizeHint() const
{
    return {400, kBarHeight};
}

QSize SourceTapeBar::minimumSizeHint() const
{
    return {100, kBarHeight};
}

void SourceTapeBar::setTape(const SourceTape& tape)
{
    m_tape = tape;
    update();
}

void SourceTapeBar::setPosition(int64_t frame)
{
    if (frame == m_position)
        return;
    m_position = frame;
    update();
}

void SourceTapeBar::setMarks(int64_t in, int64_t out)
{
    m_in = in;
    m_out = out;
    update();
}

double SourceTapeBar::xOfFrame(double frame) const
{
    const double length = double(std::max<int64_t>(1, m_tape.duration()));
    return kSidePadding + frame * (width() - 2 * kSidePadding) / length;
}

int64_t SourceTapeBar::frameAtX(double x) const
{
    const int64_t length = m_tape.duration();
    if (length <= 0)
        return 0;
    const double frame
        = (x - kSidePadding) * double(length) / std::max(1, width() - 2 * kSidePadding);
    return std::clamp<int64_t>(int64_t(std::floor(frame)), 0, length - 1);
}

void SourceTapeBar::paintEvent(QPaintEvent*)
{
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    p.fillRect(rect(), sc.lane);
    const int current = m_tape.entryAt(m_position);
    const auto& entries = m_tape.entries();
    for (size_t i = 0; i < entries.size(); ++i) {
        const SourceTapeEntry& e = entries[i];
        const double x0 = xOfFrame(double(e.start));
        const double x1 = xOfFrame(double(e.end()));
        const QRectF block(x0, 3, std::max(1.0, x1 - x0 - 1), height() - 6);
        const QColor base = e.hasVideo ? sc.videoClip : sc.audioClip;
        const QColor fill = int(i) == current ? base : mixColors(base, sc.lane, 0.45);
        p.fillRect(block, fill);
        if (block.width() >= kMinNameWidth) {
            p.setPen(sc.textOn(fill));
            p.drawText(block.adjusted(3, 0, -3, 0), Qt::AlignLeft | Qt::AlignVCenter,
                fontMetrics().elidedText(e.name, Qt::ElideRight, int(block.width()) - 6));
        }
    }
    if (m_in >= 0 || m_out > 0) {
        const double a = xOfFrame(double(std::max<int64_t>(0, m_in)));
        const double b = xOfFrame(double(m_out > 0 ? m_out : m_tape.duration()));
        p.fillRect(QRectF(a, 0, std::max(1.0, b - a), 3), sc.playhead);
    }
    const double px = xOfFrame(double(m_position));
    p.setPen(QPen(sc.playhead, 2));
    p.drawLine(QPointF(px, 0), QPointF(px, height()));
}

void SourceTapeBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && !m_tape.isEmpty())
        emit positionRequested(frameAtX(event->position().x()));
}

void SourceTapeBar::mouseMoveEvent(QMouseEvent* event)
{
    if ((event->buttons() & Qt::LeftButton) && !m_tape.isEmpty())
        emit positionRequested(frameAtX(event->position().x()));
}

} // namespace kk
