// SPDX-License-Identifier: GPL-3.0-or-later
// EqCurveView painting and handle gestures.
#include "ui/audio/EqCurveView.h"

#include "render/Biquad.h"
#include "ui/audio/AudioScales.h"
#include "ui/common/SchemeColors.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {
constexpr double kSampleRate = 48000.0;
constexpr double kRangeDb = kEqMaxGainDb; ///< the plot shows +-24 dB
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
    setToolTip(tr("Drag a band to change its frequency and gain; the wheel changes its Q"));
}

void EqCurveView::setSettings(const EqSettings& settings)
{
    if (settings == m_settings)
        return;
    m_settings = settings;
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
    return r.center().y() - std::clamp(db, -kRangeDb, kRangeDb) / kRangeDb * r.height() / 2.0;
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
    for (int i = 0; i < kEqBandCount; ++i) {
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
    if (m_selected >= 0 && m_settings.bands[size_t(m_selected)].enabled) {
        const dsp::BiquadCoeffs& c = coeffs[size_t(m_selected)];
        p.setPen(QPen(withAlpha(sc.series(m_selected), 170), 1.2, Qt::DashLine));
        p.drawPath(curve([&](double hz) { return dsp::magnitudeDb(c, hz, kSampleRate); }));
    }
    const QPainterPath total = curve([&](double hz) {
        double db = 0.0;
        for (const dsp::BiquadCoeffs& c : coeffs)
            db += dsp::magnitudeDb(c, hz, kSampleRate);
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
    for (int i = 0; i < kEqBandCount; ++i) {
        const EqBand& b = m_settings.bands[size_t(i)];
        const QPointF h = handlePosition(i);
        const QColor c = b.enabled ? sc.series(i) : sc.textDisabled;
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
    if (event->button() != Qt::LeftButton)
        return;
    const int band = bandAt(event->position());
    if (band != m_selected) {
        setSelectedBand(band);
        emit bandSelected(band);
    }
    if (band < 0)
        return;
    m_dragging = band;
    emit bandPressed(band);
}

void EqCurveView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging < 0)
        return;
    const QRectF r = plot();
    const double x = (event->position().x() - r.left()) / std::max(1.0, r.width());
    const double hz
        = std::clamp(audioscale::positionFrequency(x), kEqMinFrequencyHz, kEqMaxFrequencyHz);
    const double db
        = std::clamp((r.center().y() - event->position().y()) / (r.height() / 2.0) * kRangeDb,
            -kRangeDb, kRangeDb);
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

void EqCurveView::wheelEvent(QWheelEvent* event)
{
    const int band = bandAt(event->position());
    const int notches = event->angleDelta().y() / 120;
    if (band >= 0 && notches != 0) {
        const double q = m_settings.bands[size_t(band)].q * std::pow(kQWheelFactor, notches);
        emit bandQRequested(band, std::clamp(q, kEqMinQ, kEqMaxQ));
    }
    event->accept();
}

} // namespace kk
