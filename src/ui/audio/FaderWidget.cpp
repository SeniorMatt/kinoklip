// SPDX-License-Identifier: GPL-3.0-or-later
// FaderWidget and PanKnob painting and gestures.
#include "ui/audio/FaderWidget.h"

#include "core/MixerData.h"
#include "ui/audio/AudioScales.h"
#include "ui/common/SchemeColors.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk {

namespace {
constexpr int kCapHeight = 22;
constexpr int kCapWidth = 22;
constexpr int kScaleWidth = 20; ///< dB labels left of the groove
constexpr double kFineFactor = 0.1; ///< Shift drags
constexpr double kWheelDb = 0.5;
constexpr double kScaleMarks[] = {12.0, 6.0, 0.0, -6.0, -12.0, -20.0, -30.0, -40.0, -60.0};
constexpr double kPanDragPx = 200.0;
constexpr double kPanWheelStep = 0.05;
constexpr double kKnobSweep = 0.75 * std::numbers::pi; ///< +-135 degrees

QFont smallFont(const QWidget* w)
{
    QFont f = w->font();
    f.setPointSizeF(std::max(6.0, f.pointSizeF() * 0.72));
    return f;
}
} // namespace

// ---- FaderWidget ----

FaderWidget::FaderWidget(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::NoFocus);
    setToolTip(tr("Fader: drag (Shift for fine steps), double-click for 0 dB"));
}

void FaderWidget::setValue(double db)
{
    const double v = std::clamp(db, kFaderMinDb, kFaderMaxDb);
    if (v == m_db)
        return;
    m_db = v;
    update();
}

QRect FaderWidget::track() const
{
    return QRect(kScaleWidth, kCapHeight / 2, width() - kScaleWidth, height() - kCapHeight);
}

int FaderWidget::yOf(double db) const
{
    const QRect t = track();
    return t.bottom() - int(std::lround(audioscale::faderPosition(db) * t.height()));
}

QRect FaderWidget::capRect() const
{
    const QRect t = track();
    const int cx = t.left() + t.width() / 2;
    return QRect(cx - kCapWidth / 2, yOf(m_db) - kCapHeight / 2, kCapWidth, kCapHeight);
}

void FaderWidget::paintEvent(QPaintEvent*)
{
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRect t = track();
    const int cx = t.left() + t.width() / 2;
    // Scale.
    p.setFont(smallFont(this));
    for (double mark : kScaleMarks) {
        const int y = yOf(mark);
        p.setPen(mark == 0.0 ? sc.text : sc.textDim);
        p.drawLine(QPointF(cx - 9, y + 0.5), QPointF(cx - 5, y + 0.5));
        p.drawText(QRect(0, y - 6, kScaleWidth + cx - t.left() - 12, 12),
            Qt::AlignRight | Qt::AlignVCenter,
            mark > 0 ? QStringLiteral("+%1").arg(int(mark)) : QString::number(int(mark)));
    }
    // Groove.
    p.setPen(Qt::NoPen);
    p.setBrush(mixColors(sc.view, sc.shadow, 0.5));
    p.drawRoundedRect(QRectF(cx - 2, t.top(), 4, t.height()), 2, 2);
    // Cap: a plate with a centre line.
    const QRect cap = capRect();
    p.setBrush(m_dragging ? sc.highlight : sc.button);
    p.setPen(sc.line);
    p.drawRoundedRect(QRectF(cap).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    p.setPen(QPen(m_dragging ? sc.highlightText : sc.text, 2));
    p.drawLine(QPointF(cap.left() + 4, cap.center().y() + 0.5),
        QPointF(cap.right() - 3, cap.center().y() + 0.5));
}

void FaderWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_dragging = true;
    m_pressY = event->position().y();
    m_pressPosition = audioscale::faderPosition(m_db);
    emit pressed();
    update();
}

void FaderWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging)
        return;
    const double scale = (event->modifiers() & Qt::ShiftModifier) ? kFineFactor : 1.0;
    const double dy = (m_pressY - event->position().y()) * scale;
    const double pos = m_pressPosition + dy / std::max(1, track().height());
    setValue(audioscale::faderDb(pos));
    emit moved(m_db);
}

void FaderWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_dragging)
        return;
    m_dragging = false;
    emit released();
    update();
}

void FaderWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        emit valueRequested(0.0);
}

void FaderWidget::wheelEvent(QWheelEvent* event)
{
    const int notches = event->angleDelta().y() / 120;
    if (notches != 0)
        emit valueRequested(std::clamp(m_db + notches * kWheelDb, kFaderMinDb, kFaderMaxDb));
    event->accept();
}

// ---- PanKnob ----

PanKnob::PanKnob(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::NoFocus);
    setToolTip(tr("Pan: drag to turn, double-click to centre"));
}

void PanKnob::setValue(double pan)
{
    const double v = std::clamp(pan, -1.0, 1.0);
    if (v == m_pan)
        return;
    m_pan = v;
    update();
}

void PanKnob::paintEvent(QPaintEvent*)
{
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const double side = std::min(width(), height()) - 6.0;
    const QRectF r((width() - side) / 2.0, (height() - side) / 2.0, side, side);
    p.setPen(QPen(sc.line, 1));
    p.setBrush(m_dragging ? sc.highlight : sc.button);
    p.drawEllipse(r);
    // Arc from the centre position to the value (the scheme's focus colour).
    const int startAngle = 90 * 16;
    const int span = -int(std::lround(m_pan * kKnobSweep / std::numbers::pi * 180.0 * 16));
    p.setPen(QPen(sc.focus, 3, Qt::SolidLine, Qt::FlatCap));
    p.setBrush(Qt::NoBrush);
    p.drawArc(r.adjusted(-2, -2, 2, 2), startAngle, span);
    // Pointer.
    const double a = std::numbers::pi / 2.0 - m_pan * kKnobSweep;
    const QPointF c = r.center();
    p.setPen(QPen(m_dragging ? sc.highlightText : sc.text, 2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(c, c + QPointF(std::cos(a), -std::sin(a)) * (side / 2.0 - 3.0));
}

void PanKnob::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_dragging = true;
    m_pressPos = event->position();
    m_pressPan = m_pan;
    emit pressed();
    update();
}

void PanKnob::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging)
        return;
    const double scale = (event->modifiers() & Qt::ShiftModifier) ? kFineFactor : 1.0;
    const QPointF d = event->position() - m_pressPos;
    const double delta = (d.x() - d.y()) * scale / kPanDragPx * 2.0;
    setValue(m_pressPan + delta);
    emit moved(m_pan);
}

void PanKnob::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_dragging)
        return;
    m_dragging = false;
    emit released();
    update();
}

void PanKnob::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        emit valueRequested(0.0);
}

void PanKnob::wheelEvent(QWheelEvent* event)
{
    const int notches = event->angleDelta().y() / 120;
    if (notches != 0)
        emit valueRequested(std::clamp(m_pan + notches * kPanWheelStep, -1.0, 1.0));
    event->accept();
}

} // namespace kk
