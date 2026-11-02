// SPDX-License-Identifier: GPL-3.0-or-later
// LoudnessPanel layout, readouts and the loudness gauge.
#include "ui/audio/LoudnessPanel.h"

#include "core/MixerOps.h"
#include "ui/audio/LevelMeter.h"
#include "ui/audio/MeterModel.h"
#include "ui/audio/MixerController.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Metrics.h"
#include "ui/common/SchemeColors.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

namespace kk {

namespace {
constexpr double kGaugeFloor = -50.0; ///< LUFS at the bottom of the gauge
constexpr double kOnTargetLu = 1.0; ///< +-LU counted as "on target"
constexpr double kNone = -199.0; ///< readings at or below this show "-inf"

QString lufsText(double v, const char* unit)
{
    if (v <= kNone)
        return QStringLiteral("-inf %1").arg(QLatin1String(unit));
    return QStringLiteral("%1 %2").arg(v, 0, 'f', 1).arg(QLatin1String(unit));
}
} // namespace

// ---- LoudnessGauge ----

LoudnessGauge::LoudnessGauge(MeterModel* meters, QWidget* parent)
    : QWidget(parent)
    , m_meters(meters)
{
    connect(m_meters, &MeterModel::changed, this, qOverload<>(&QWidget::update));
    setToolTip(tr("Momentary (M) and short-term (S) loudness; the line marks the target"));
}

void LoudnessGauge::paintEvent(QPaintEvent*)
{
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    const MeterFrame& f = m_meters->lastFrame();
    const QRect area(0, 14, width(), height() - 16);
    auto yOf = [&](double lufs) {
        const double t = std::clamp((lufs - kGaugeFloor) / -kGaugeFloor, 0.0, 1.0);
        return area.bottom() - int(t * area.height());
    };
    const double values[2] = {f.momentaryLufs, f.shortTermLufs};
    const QString names[2] = {tr("M"), tr("S")};
    const int barWidth = (area.width() - 8) / 2;
    for (int i = 0; i < 2; ++i) {
        const QRect bar(4 + i * (barWidth + 4), area.top(), barWidth - 4, area.height());
        p.fillRect(bar, mixColors(sc.view, sc.shadow, 0.35));
        const double v = values[i];
        if (v > kGaugeFloor) {
            const QColor c = v > LoudnessPanel::kTargetLufs + kOnTargetLu ? sc.negative
                : v >= LoudnessPanel::kTargetLufs - kOnTargetLu           ? sc.positive
                                                                          : sc.neutral;
            const int top = yOf(v);
            p.fillRect(QRect(bar.left(), top, bar.width(), bar.bottom() - top + 1), c);
        }
        p.setPen(sc.textDim);
        p.drawText(QRect(bar.left(), 0, bar.width(), 14), Qt::AlignCenter, names[i]);
    }
    p.setPen(QPen(sc.text, 1, Qt::DashLine));
    const int ty = yOf(LoudnessPanel::kTargetLufs);
    p.drawLine(0, ty, width(), ty);
}

// ---- LoudnessPanel ----

LoudnessPanel::LoudnessPanel(
    AppContext& ctx, MixerController* controller, MeterModel* meters, QWidget* parent)
    : PanelFrame(tr("Meters"), parent)
    , m_controller(controller)
    , m_meters(meters)
{
    setObjectName(QStringLiteral("LoudnessPanel"));
    if (ctx.actions && ctx.actions->contains(QStringLiteral("audio.reset_loudness")))
        addTitleBarAction(ctx.actions->action(QStringLiteral("audio.reset_loudness")));

    auto* body = new QWidget;
    auto* layout = new QHBoxLayout(body);
    layout->setContentsMargins(
        metrics::kSpaceM, metrics::kSpaceS, metrics::kSpaceM, metrics::kSpaceS);
    layout->setSpacing(metrics::kSpaceL);

    auto* outColumn = new QVBoxLayout;
    m_output = new LevelMeter(meters, LevelMeter::Source::Output);
    m_output->setScaleVisible(true);
    m_output->setMinimumWidth(48);
    outColumn->addWidget(m_output, 1);
    auto* outLabel = new QLabel(tr("Output"));
    outLabel->setAlignment(Qt::AlignCenter);
    outColumn->addWidget(outLabel);
    layout->addLayout(outColumn);

    m_busRow = new QHBoxLayout;
    m_busRow->setSpacing(metrics::kSpaceS);
    layout->addLayout(m_busRow);

    layout->addWidget(new LoudnessGauge(meters));

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    m_momentary = new QLabel;
    m_shortTerm = new QLabel;
    m_integrated = new QLabel;
    m_truePeak = new QLabel;
    form->addRow(tr("Momentary"), m_momentary);
    form->addRow(tr("Short term"), m_shortTerm);
    form->addRow(tr("Integrated"), m_integrated);
    form->addRow(tr("True peak max"), m_truePeak);
    form->addRow(tr("Target"), new QLabel(lufsText(kTargetLufs, "LUFS")));
    layout->addLayout(form);
    layout->addStretch(1);
    setContentWidget(body);

    connect(m_meters, &MeterModel::changed, this, &LoudnessPanel::updateReadouts);
    connect(m_controller, &MixerController::mixChanged, this, &LoudnessPanel::syncBuses);
    syncBuses();
    updateReadouts();
}

void LoudnessPanel::syncBuses()
{
    const TimelineData& d = m_controller->data();
    QVector<int> ids;
    for (const BusData& b : d.mixer.buses)
        ids.append(b.id);
    if (ids != m_busIds) {
        for (LevelMeter* m : std::as_const(m_busMeters))
            m->deleteLater();
        for (QLabel* l : std::as_const(m_busLabels))
            l->deleteLater();
        m_busMeters.clear();
        m_busLabels.clear();
        while (QLayoutItem* item = m_busRow->takeAt(0)) {
            if (QLayout* sub = item->layout())
                delete sub;
            else
                delete item;
        }
        for (int id : ids) {
            auto* column = new QVBoxLayout;
            auto* meter = new LevelMeter(m_meters, LevelMeter::Source::Bus, quint64(id));
            auto* label = new QLabel;
            label->setAlignment(Qt::AlignCenter);
            column->addWidget(meter, 1, Qt::AlignHCenter);
            column->addWidget(label);
            m_busRow->addLayout(column);
            m_busMeters.append(meter);
            m_busLabels.append(label);
        }
        m_busIds = ids;
    }
    for (int i = 0; i < m_busIds.size(); ++i) {
        const BusData* b = mixerops::bus(d, m_busIds[i]);
        m_busLabels[i]->setText(b ? busDisplayName(*b) : QString());
        m_busLabels[i]->setEnabled(b && b->id == d.mixer.outputBus);
    }
    m_output->setToolTip(tr("Output bus: %1")
                             .arg(mixerops::bus(d, d.mixer.outputBus)
                                     ? busDisplayName(*mixerops::bus(d, d.mixer.outputBus))
                                     : QString()));
}

void LoudnessPanel::updateReadouts()
{
    const MeterFrame& f = m_meters->lastFrame();
    m_momentary->setText(lufsText(f.momentaryLufs, "LUFS"));
    m_shortTerm->setText(lufsText(f.shortTermLufs, "LUFS"));
    m_integrated->setText(lufsText(f.integratedLufs, "LUFS"));
    m_truePeak->setText(lufsText(f.truePeakMaxDb, "dBTP"));
}

QString LoudnessPanel::readoutText() const
{
    return QStringList {
        m_momentary->text(), m_shortTerm->text(), m_integrated->text(), m_truePeak->text()}
        .join(QStringLiteral(" | "));
}

} // namespace kk
