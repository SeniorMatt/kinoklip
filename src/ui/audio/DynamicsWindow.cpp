// SPDX-License-Identifier: GPL-3.0-or-later
// DynamicsWindow layout, edits and the transfer curve.
#include "ui/audio/DynamicsWindow.h"

#include "render/Dynamics.h"
#include "ui/audio/MeterModel.h"
#include "ui/audio/MixerController.h"
#include "ui/common/Metrics.h"
#include "ui/common/SchemeColors.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>

namespace kk {

namespace {
constexpr double kCurveFloorDb = -60.0;
}

// ---- TransferCurveView ----

TransferCurveView::TransferCurveView(QWidget* parent)
    : QWidget(parent)
{ }

void TransferCurveView::setSettings(const DynamicsSettings& settings)
{
    if (settings == m_settings)
        return;
    m_settings = settings;
    update();
}

void TransferCurveView::paintEvent(QPaintEvent*)
{
    const SchemeColors& sc = schemeColors();
    QPainter p(this);
    const double side = std::min(width(), height()) - 2.0;
    const QRectF r(1, 1, side, side);
    p.fillRect(r, sc.view);
    auto pt = [&](double inDb, double outDb) {
        return QPointF(r.left() + (inDb - kCurveFloorDb) / -kCurveFloorDb * r.width(),
            r.bottom()
                - (std::clamp(outDb, kCurveFloorDb, 0.0) - kCurveFloorDb) / -kCurveFloorDb
                    * r.height());
    };
    p.setPen(sc.grid);
    for (double db = -48.0; db < 0.0; db += 12.0) {
        p.drawLine(pt(db, kCurveFloorDb), pt(db, 0.0));
        p.drawLine(pt(kCurveFloorDb, db), pt(0.0, db));
    }
    p.setPen(QPen(sc.line, 1, Qt::DashLine));
    p.drawLine(pt(kCurveFloorDb, kCurveFloorDb), pt(0.0, 0.0)); // unity
    const DynamicsSettings& d = m_settings;
    QPainterPath path;
    for (int i = 0; i <= 120; ++i) {
        const double in = kCurveFloorDb + i * (-kCurveFloorDb / 120.0);
        double out = in;
        if (d.compressorEnabled)
            out = in - dsp::compressorGainReductionDb(in, d.thresholdDb, d.ratio, d.kneeDb)
                + d.makeupDb;
        if (d.limiterEnabled)
            out = std::min(out, d.limiterCeilingDb);
        if (i == 0)
            path.moveTo(pt(in, out));
        else
            path.lineTo(pt(in, out));
    }
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(d.compressorEnabled || d.limiterEnabled ? sc.focus : sc.textDisabled, 2));
    p.drawPath(path);
    if (d.compressorEnabled) {
        p.setPen(QPen(sc.neutral, 1, Qt::DotLine));
        p.drawLine(pt(d.thresholdDb, kCurveFloorDb), pt(d.thresholdDb, 0.0));
    }
}

// ---- DynamicsWindow ----

DynamicsWindow::DynamicsWindow(
    MixerController* controller, MeterModel* meters, const MixTarget& target, QWidget* parent)
    : QWidget(parent, Qt::Tool)
    , m_controller(controller)
    , m_meters(meters)
    , m_target(target)
{
    setObjectName(QStringLiteral("DynamicsWindow"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceM, metrics::kSpaceM, metrics::kSpaceM, metrics::kSpaceM);
    layout->setSpacing(metrics::kSpaceL);

    auto* left = new QVBoxLayout;
    m_curve = new TransferCurveView;
    m_curve->setMinimumSize(180, 180);
    left->addWidget(m_curve, 1);
    m_reduction = new QLabel;
    left->addWidget(m_reduction);
    layout->addLayout(left, 1);

    auto* form = new QFormLayout;
    m_compressor = new QCheckBox(tr("Compressor"));
    form->addRow(m_compressor);
    addField(form, tr("Threshold"), kDynMinThresholdDb, 0.0, 1, tr(" dB"),
        &DynamicsSettings::thresholdDb);
    addField(form, tr("Ratio"), 1.0, kDynMaxRatio, 1, tr(":1"), &DynamicsSettings::ratio);
    addField(form, tr("Attack"), kDynMinTimeMs, kDynMaxAttackMs, 1, tr(" ms"),
        &DynamicsSettings::attackMs);
    addField(form, tr("Release"), kDynMinTimeMs, kDynMaxReleaseMs, 0, tr(" ms"),
        &DynamicsSettings::releaseMs);
    addField(form, tr("Knee"), 0.0, kDynMaxKneeDb, 1, tr(" dB"), &DynamicsSettings::kneeDb);
    addField(form, tr("Make-up"), 0.0, kDynMaxMakeupDb, 1, tr(" dB"), &DynamicsSettings::makeupDb);
    m_limiter = new QCheckBox(tr("Limiter"));
    form->addRow(m_limiter);
    addField(form, tr("Ceiling"), kLimiterMinCeilingDb, 0.0, 1, tr(" dB"),
        &DynamicsSettings::limiterCeilingDb);
    addField(form, tr("Limiter release"), kDynMinTimeMs, kDynMaxReleaseMs, 0, tr(" ms"),
        &DynamicsSettings::limiterReleaseMs);
    layout->addLayout(form);

    connect(m_compressor, &QCheckBox::toggled, this, [this](bool on) {
        editDynamics(on ? tr("Enable Compressor") : tr("Bypass Compressor"),
            [on](DynamicsSettings& d) { d.compressorEnabled = on; });
    });
    connect(m_limiter, &QCheckBox::toggled, this, [this](bool on) {
        editDynamics(on ? tr("Enable Limiter") : tr("Bypass Limiter"),
            [on](DynamicsSettings& d) { d.limiterEnabled = on; });
    });
    connect(m_controller, &MixerController::mixChanged, this, &DynamicsWindow::refresh);
    if (m_meters) {
        connect(m_meters, &MeterModel::changed, this, [this] {
            const MeterReading r
                = m_target.isBus() ? m_meters->bus(int(m_target.id)) : m_meters->track(m_target.id);
            m_reduction->setText(tr("Gain reduction: %1 dB").arg(-r.gainReductionDb, 0, 'f', 1));
        });
    }
    m_reduction->setText(tr("Gain reduction: %1 dB").arg(0.0, 0, 'f', 1));
    refresh();
}

QDoubleSpinBox* DynamicsWindow::addField(QFormLayout* form, const QString& label, double min,
    double max, int decimals, const QString& suffix, double DynamicsSettings::*field)
{
    auto* box = new QDoubleSpinBox;
    box->setRange(min, max);
    box->setDecimals(decimals);
    box->setSuffix(suffix);
    box->setKeyboardTracking(false);
    box->setAccelerated(true);
    form->addRow(label, box);
    m_fields.append({box, field});
    connect(box, &QDoubleSpinBox::valueChanged, this, [this, label, field](double v) {
        editDynamics(label, [field, v](DynamicsSettings& d) { d.*field = v; });
    });
    return box;
}

void DynamicsWindow::editDynamics(
    const QString& label, const std::function<void(DynamicsSettings&)>& change)
{
    m_controller->edit(label, [&](TimelineData& d) {
        ChannelStrip* s = mixerops::strip(d, m_target);
        if (!s)
            return false;
        const DynamicsSettings before = s->dynamics;
        change(s->dynamics);
        mixerops::clampStrip(*s);
        return !(s->dynamics == before);
    });
}

void DynamicsWindow::refresh()
{
    const TimelineData& d = m_controller->data();
    const ChannelStrip* s = mixerops::strip(d, m_target);
    if (!s) {
        close();
        return;
    }
    setWindowTitle(tr("Dynamics - %1").arg(mixerops::targetName(d, m_target)));
    const QSignalBlocker b1(m_compressor), b2(m_limiter);
    m_compressor->setChecked(s->dynamics.compressorEnabled);
    m_limiter->setChecked(s->dynamics.limiterEnabled);
    for (const auto& [box, field] : std::as_const(m_fields)) {
        const QSignalBlocker block(box);
        const double v = s->dynamics.*field;
        if (!box->hasFocus() || box->value() == v)
            box->setValue(v);
    }
    m_curve->setSettings(s->dynamics);
}

} // namespace kk
