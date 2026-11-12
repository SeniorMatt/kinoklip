// SPDX-License-Identifier: GPL-3.0-or-later
// EqCurveView painting and handle gestures.
#include "ui/audio/EqCurveView.h"

#include "render/Biquad.h"
#include "ui/audio/AudioScales.h"
#include "ui/common/SchemeColors.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {
constexpr double kSampleRate = 48000.0;
constexpr int kHandleRadius = 7;
constexpr int kMargin = 24; ///< room for the axis labels
constexpr double kQWheelFactor = 1.12; ///< Q change per wheel notch
constexpr double kGridHz[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000};
constexpr double kGridDb[] = {-18, -12, -6, 0, 6, 12, 18};

QString hzLabel(double hz)
{
    return hz >= 1000 ? QStringLiteral("%1k").arg(hz / 1000.0) : QString::number(hz);
}
} // namespace

EqCurveView::EqCurveView(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(false);
    setFocusPolicy(Qt::StrongFocus);
    setToolTip(tr("Drag a band to change its frequency and gain (Shift: fine). The wheel or "
                  "Alt-drag changes its Q, double-click resets the band, arrow keys move it."));
}

void EqCurveView::setSettings(const EqSettings& settings)
{
    if (settings == m_settings)
        return;
    m_settings = settings;
    update();
}

void EqCurveView::setBandCount(int count)
{
    count = std::clamp(count, 1, kEqBandCount);
    if (count == m_count)
        return;
    m_count = count;
    if (m_selected >= count)
        m_selected = -1;
    update();
}

void EqCurveView::setRangeDb(double db)
{
    m_rangeDb = std::max(1.0, db);
    update();
}

void EqCurveView::setShowBandCurves(bool on)
{
    m_showBandCurves = on;
    update();
}

void EqCurveView::setSelectedBand(int band)
{
    if (band == m_selected)
        return;
    m_selected = band;
    update();
}

QRectF EqCurveView::plot() const
{
    return QRectF(kMargin, 8, width() - kMargin - 8, height() - 8 - kMargin);
}

double EqCurveView::xOf(double hz) const
{
    const QRectF r = plot();
    return r.left() + audioscale::frequencyPosition(hz) * r.width();
}

double EqCurveView::yOf(double db) const
{
    const QRectF r = plot();
    return r.center().y() - std::clamp(db, -m_rangeDb, m_rangeDb) / m_rangeDb * r.height() / 2.0;
}

QPointF EqCurveView::handlePosition(int band) const
{
    const EqBand& b = m_settings.bands[size_t(band)];
    return {xOf(b.frequencyHz), yOf(eqBandHasGain(b.type) ? b.gainDb : 0.0)};
}

int EqCurveView::bandAt(QPointF pos) const
{
    int best = -1;
    double bestDist = kHandleRadius * 1.6;
    for (int i = 0; i < m_count; ++i) {
        const double d = QLineF(pos, handlePosition(i)).length();
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

void EqCurveView::paintEvent(QPaintEvent*)
{
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    p.fillRect(rect(), sc.window);
    const QRectF r = plot();
    p.fillRect(r, sc.view);

    // Grid and labels.
    QFont f = font();
    f.setPointSizeF(std::max(6.0, f.pointSizeF() * 0.8));
    p.setFont(f);
    for (double hz : kGridHz) {
        const double x = xOf(hz);
        p.setPen(sc.grid);
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.setPen(sc.textDim);
        p.drawText(QRectF(x - 20, r.bottom() + 2, 40, kMargin - 4), Qt::AlignCenter, hzLabel(hz));
    }
    for (double db : kGridDb) {
        const double y = yOf(db);
        p.setPen(db == 0 ? sc.line : sc.grid);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        p.setPen(sc.textDim);
        p.drawText(QRectF(0, y - 7, kMargin - 3, 14), Qt::AlignRight | Qt::AlignVCenter,
            QString::number(int(db)));
    }

    // Band coefficients (exactly the mixer's) and the summed response.
    std::array<dsp::BiquadCoeffs, kEqBandCount> coeffs;
    for (int i = 0; i < kEqBandCount; ++i) {
        const EqBand& b = m_settings.bands[size_t(i)];
        coeffs[size_t(i)] = b.enabled
            ? dsp::designBand(b.type, b.frequencyHz, b.gainDb, b.q, kSampleRate)
            : dsp::BiquadCoeffs {};
    }
    const int columns = std::max(2, int(r.width()));
    auto curve = [&](auto&& dbAt) {
        QPainterPath path;
        for (int c = 0; c <= columns; ++c) {
            const double hz = audioscale::positionFrequency(double(c) / columns);
            const QPointF pt(r.left() + c * r.width() / columns, yOf(dbAt(hz)));
            if (c == 0)
                path.moveTo(pt);
            else
                path.lineTo(pt);
        }
        return path;
    };
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setClipRect(r);
    for (int i = 0; i < m_count; ++i) {
        const bool selected = i == m_selected;
        if (!m_settings.bands[size_t(i)].enabled || (!selected && !m_showBandCurves))
            continue;
        const dsp::BiquadCoeffs& c = coeffs[size_t(i)];
        QPen pen(withAlpha(sc.series(i), selected ? 200 : 90), selected ? 1.4 : 1.0);
        if (selected)
            pen.setStyle(Qt::DashLine);
        p.setPen(pen);
        p.drawPath(curve([&](double hz) { return dsp::magnitudeDb(c, hz, kSampleRate); }));
    }
    const QPainterPath total = curve([&](double hz) {
        double db = 0.0;
        for (int i = 0; i < m_count; ++i)
            db += dsp::magnitudeDb(coeffs[size_t(i)], hz, kSampleRate);
        return db;
    });
    QPainterPath area = total;
    area.lineTo(r.right(), yOf(0.0));
    area.lineTo(r.left(), yOf(0.0));
    area.closeSubpath();
    const QColor curveColor = m_settings.enabled ? sc.focus : sc.textDisabled;
    p.fillPath(area, withAlpha(curveColor, 40));
    p.setPen(QPen(curveColor, 2));
    p.drawPath(total);
    p.setClipping(false);

    // Handles.
    for (int i = 0; i < m_count; ++i) {
        const EqBand& b = m_settings.bands[size_t(i)];
        const QPointF h = handlePosition(i);
        const QColor c = b.enabled ? sc.series(i) : sc.textDisabled;
        if (i == m_selected && hasFocus()) {
            p.setPen(QPen(withAlpha(sc.focus, 160), 2));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(h, kHandleRadius + 4, kHandleRadius + 4);
        }
        p.setPen(QPen(i == m_selected ? sc.text : sc.shadow, i == m_selected ? 2 : 1));
        p.setBrush(c);
        p.drawEllipse(h, kHandleRadius, kHandleRadius);
        p.setPen(sc.textOn(c));
        p.drawText(QRectF(h.x() - kHandleRadius, h.y() - kHandleRadius, 2 * kHandleRadius,
                       2 * kHandleRadius),
            Qt::AlignCenter, QString::number(i + 1));
    }
}

void EqCurveView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton)
        return;
    const int band = bandAt(event->position());
    setFocus(Qt::MouseFocusReason);
    if (band != m_selected) {
        setSelectedBand(band);
        emit bandSelected(band);
    }
    if (event->button() == Qt::RightButton) {
        emit bandContextRequested(band, event->globalPosition().toPoint());
        return;
    }
    if (band < 0)
        return;
    m_dragging = band;
    m_dragPoint = handlePosition(band);
    m_lastMouse = event->position();
    m_dragQ = false;
    m_dragQValue = m_settings.bands[size_t(band)].q;
    emit bandPressed(band);
}

void EqCurveView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging < 0)
        return;
    const QPointF delta = event->position() - m_lastMouse;
    m_lastMouse = event->position();
    if (event->modifiers() & Qt::AltModifier) {
        // Alt-drag: Q, one octave of Q per 60 pixels up.
        m_dragQ = true;
        const double factor = (event->modifiers() & Qt::ShiftModifier) ? 0.1 : 1.0;
        m_dragQValue = std::clamp(
            m_dragQValue * std::pow(2.0, -delta.y() * factor / 60.0), kEqMinQ, kEqMaxQ);
        emit bandQDragged(m_dragging, m_dragQValue);
        return;
    }
    const QRectF r = plot();
    const double scale = (event->modifiers() & Qt::ShiftModifier) ? 0.1 : 1.0;
    m_dragPoint += delta * scale;
    m_dragPoint.setX(std::clamp(m_dragPoint.x(), r.left(), r.right()));
    m_dragPoint.setY(std::clamp(m_dragPoint.y(), r.top(), r.bottom()));
    const double x = (m_dragPoint.x() - r.left()) / std::max(1.0, r.width());
    const double hz
        = std::clamp(audioscale::positionFrequency(x), kEqMinFrequencyHz, kEqMaxFrequencyHz);
    const double db = std::clamp(
        (r.center().y() - m_dragPoint.y()) / (r.height() / 2.0) * m_rangeDb, -m_rangeDb, m_rangeDb);
    emit bandDragged(m_dragging, hz, db);
}

void EqCurveView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_dragging < 0)
        return;
    const int band = m_dragging;
    m_dragging = -1;
    emit bandReleased(band);
}

void EqCurveView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const int band = bandAt(event->position());
    if (band >= 0)
        emit bandResetRequested(band);
}

void EqCurveView::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    if (m_selected < 0 && event->reason() != Qt::MouseFocusReason) {
        m_selected = event->reason() == Qt::BacktabFocusReason ? m_count - 1 : 0;
        emit bandSelected(m_selected);
    }
    update();
}

bool EqCurveView::focusNextPrevChild(bool next)
{
    if (hasFocus() && m_selected >= 0) {
        const int target = m_selected + (next ? 1 : -1);
        if (target >= 0 && target < m_count) {
            setSelectedBand(target);
            emit bandSelected(target);
            return true;
        }
        setSelectedBand(-1); // leaving: the next visit starts at the first / last band
    }
    return QWidget::focusNextPrevChild(next);
}

void EqCurveView::keyPressEvent(QKeyEvent* event)
{
    const int b = m_selected;
    if (b < 0 || b >= m_count) {
        QWidget::keyPressEvent(event);
        return;
    }
    const EqBand& band = m_settings.bands[size_t(b)];
    const bool fine = event->modifiers() & Qt::ShiftModifier;
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Right: {
        // 1/12 octave per step, 1/96 with Shift.
        const double octaves = (event->key() == Qt::Key_Right ? 1.0 : -1.0) / (fine ? 96.0 : 12.0);
        const double hz = std::clamp(
            band.frequencyHz * std::pow(2.0, octaves), kEqMinFrequencyHz, kEqMaxFrequencyHz);
        emit bandPressed(b);
        emit bandDragged(b, hz, band.gainDb);
        emit bandReleased(b);
        break;
    }
    case Qt::Key_Up:
    case Qt::Key_Down: {
        const double step = (event->key() == Qt::Key_Up ? 1.0 : -1.0) * (fine ? 0.1 : 0.5);
        const double db = std::clamp(band.gainDb + step, -m_rangeDb, m_rangeDb);
        emit bandPressed(b);
        emit bandDragged(b, band.frequencyHz, db);
        emit bandReleased(b);
        break;
    }
    case Qt::Key_PageUp:
    case Qt::Key_PageDown: {
        const double q = std::clamp(
            band.q * std::pow(kQWheelFactor, event->key() == Qt::Key_PageUp ? 1 : -1), kEqMinQ,
            kEqMaxQ);
        emit bandQRequested(b, q);
        break;
    }
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        emit bandResetRequested(b);
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void EqCurveView::wheelEvent(QWheelEvent* event)
{
    const int band = bandAt(event->position());
    const int notches = event->angleDelta().y() / 120;
    if (band >= 0 && notches != 0) {
        const double q = m_settings.bands[size_t(band)].q * std::pow(kQWheelFactor, notches);
        emit bandQRequested(band, std::clamp(q, kEqMinQ, kEqMaxQ));
        event->accept();
        return;
    }
    event->ignore(); // not over a handle: the Inspector scrolls
}

} // namespace kk
