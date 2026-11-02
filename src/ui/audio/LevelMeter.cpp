// SPDX-License-Identifier: GPL-3.0-or-later
// LevelMeter — zone-coloured bars, hold marks and scale.
#include "ui/audio/LevelMeter.h"

#include "ui/audio/AudioScales.h"
#include "ui/audio/MeterModel.h"
#include "ui/common/SchemeColors.h"

#include <QPainter>

namespace kk {

namespace {
constexpr int kBarWidth = 6;
constexpr int kBarGap = 2;
constexpr int kScaleWidth = 22;
constexpr int kMinHeight = 60;
constexpr double kScaleMarks[] = {0.0, -6.0, -12.0, -18.0, -24.0, -36.0, -48.0, -60.0};
} // namespace

LevelMeter::LevelMeter(MeterModel* model, Source source, quint64 id, QWidget* parent)
    : QWidget(parent)
    , m_model(model)
    , m_source(source)
    , m_id(id)
{
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    connect(m_model, &MeterModel::changed, this, qOverload<>(&QWidget::update));
}

void LevelMeter::setScaleVisible(bool visible)
{
    m_scale = visible;
    updateGeometry();
    update();
}

void LevelMeter::setSource(Source source, quint64 id)
{
    m_source = source;
    m_id = id;
    update();
}

QSize LevelMeter::sizeHint() const
{
    return {(m_scale ? kScaleWidth : 0) + 2 * kBarWidth + kBarGap + 4, 160};
}

QSize LevelMeter::minimumSizeHint() const
{
    return {(m_scale ? kScaleWidth : 0) + 2 * kBarWidth + kBarGap + 4, kMinHeight};
}

void LevelMeter::paintEvent(QPaintEvent*)
{
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    const MeterReading r = m_source == Source::Track ? m_model->track(m_id)
        : m_source == Source::Bus                    ? m_model->bus(int(m_id))
                                                     : m_model->output();
    const int left = m_scale ? kScaleWidth : 0;
    const QRect area(left, 2, width() - left, height() - 4);
    const int bars = r.mono ? 1 : 2;
    const int barWidth = std::max(3, (area.width() - 4 - (bars - 1) * kBarGap) / bars);
    auto yOf = [&](double db) {
        return area.bottom() - int(audioscale::meterPosition(db) * area.height());
    };

    if (m_scale) {
        QFont f = font();
        f.setPointSizeF(std::max(6.0, f.pointSizeF() * 0.75));
        p.setFont(f);
        p.setPen(sc.textDim);
        for (double mark : kScaleMarks) {
            const int y = yOf(mark);
            p.drawLine(left - 4, y, left - 1, y);
            p.drawText(QRect(0, y - 6, left - 5, 12), Qt::AlignRight | Qt::AlignVCenter,
                QString::number(int(mark)));
        }
    }

    const QColor trough = mixColors(sc.view, sc.shadow, 0.35);
    const int zoneNeutral = yOf(audioscale::kMeterNeutralDb);
    const int zoneNegative = yOf(audioscale::kMeterNegativeDb);
    for (int b = 0; b < bars; ++b) {
        const QRect bar(
            area.left() + 2 + b * (barWidth + kBarGap), area.top(), barWidth, area.height());
        p.fillRect(bar, trough);
        const int top = yOf(r.levelDb[b]);
        if (top < bar.bottom()) {
            // Fill each zone the bar reaches: positive below -18, neutral to -6, negative above.
            auto fill = [&](int from, int to, const QColor& c) {
                const int y0 = std::max(top, to);
                if (y0 < from)
                    p.fillRect(QRect(bar.left(), y0, bar.width(), from - y0), c);
            };
            fill(bar.bottom() + 1, zoneNeutral, sc.positive);
            fill(zoneNeutral, zoneNegative, sc.neutral);
            fill(zoneNegative, bar.top(), sc.negative);
        }
        if (r.holdDb[b] > audioscale::kMeterFloorDb) {
            const int y = yOf(r.holdDb[b]);
            const QColor c = r.holdDb[b] >= audioscale::kMeterNegativeDb ? sc.negative
                : r.holdDb[b] >= audioscale::kMeterNeutralDb             ? sc.neutral
                                                                         : sc.text;
            p.fillRect(QRect(bar.left(), y, bar.width(), 2), c);
        }
    }
}

} // namespace kk
