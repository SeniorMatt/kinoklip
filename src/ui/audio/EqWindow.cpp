// SPDX-License-Identifier: GPL-3.0-or-later
// EqWindow layout and edits.
#include "ui/audio/EqWindow.h"

#include "core/Timeline.h"
#include "ui/audio/EqCurveView.h"
#include "ui/audio/MixerController.h"
#include "ui/common/Metrics.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace kk {

namespace {

struct TypeEntry
{
    EqBandType type;
    const char* name;
};
constexpr TypeEntry kTypes[] = {
    {EqBandType::HighPass, QT_TRANSLATE_NOOP("kk::EqWindow", "High Pass")},
    {EqBandType::LowShelf, QT_TRANSLATE_NOOP("kk::EqWindow", "Low Shelf")},
    {EqBandType::Bell, QT_TRANSLATE_NOOP("kk::EqWindow", "Bell")},
    {EqBandType::Notch, QT_TRANSLATE_NOOP("kk::EqWindow", "Notch")},
    {EqBandType::HighShelf, QT_TRANSLATE_NOOP("kk::EqWindow", "High Shelf")},
    {EqBandType::LowPass, QT_TRANSLATE_NOOP("kk::EqWindow", "Low Pass")},
};

QDoubleSpinBox* spin(double min, double max, int decimals, const QString& suffix)
{
    auto* s = new QDoubleSpinBox;
    s->setRange(min, max);
    s->setDecimals(decimals);
    s->setSuffix(suffix);
    s->setKeyboardTracking(false); // typed values commit on Enter / focus out: one step
    s->setAccelerated(true);
    return s;
}

} // namespace

EqWindow::EqWindow(MixerController* controller, const MixTarget& target, QWidget* parent)
    : QWidget(parent, Qt::Tool)
    , m_controller(controller)
    , m_target(target)
{
    setObjectName(QStringLiteral("EqWindow"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceM, metrics::kSpaceM, metrics::kSpaceM, metrics::kSpaceM);
    layout->setSpacing(metrics::kSpaceM);
    m_enabled = new QCheckBox(tr("EQ on"));
    layout->addWidget(m_enabled);
    m_curve = new EqCurveView;
    m_curve->setShowBandCurves(true);
    layout->addWidget(m_curve, 1);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(metrics::kSpaceM);
    grid->setVerticalSpacing(metrics::kSpaceS);
    const QStringList headers {tr("Band"), tr("Type"), tr("Frequency"), tr("Gain"), tr("Q")};
    for (int c = 0; c < headers.size(); ++c)
        grid->addWidget(new QLabel(headers[c]), 0, c);
    for (int i = 0; i < kEqBandCount; ++i) {
        BandRow& row = m_rows[size_t(i)];
        row.on = new QCheckBox(QString::number(i + 1));
        row.type = new QComboBox;
        for (const TypeEntry& t : kTypes)
            row.type->addItem(tr(t.name), int(t.type));
        row.frequency = spin(kEqMinFrequencyHz, kEqMaxFrequencyHz, 0, tr(" Hz"));
        row.gain = spin(-kEqMaxGainDb, kEqMaxGainDb, 1, tr(" dB"));
        row.gain->setSingleStep(0.5);
        row.q = spin(kEqMinQ, kEqMaxQ, 2, QString());
        row.q->setSingleStep(0.1);
        grid->addWidget(row.on, i + 1, 0);
        grid->addWidget(row.type, i + 1, 1);
        grid->addWidget(row.frequency, i + 1, 2);
        grid->addWidget(row.gain, i + 1, 3);
        grid->addWidget(row.q, i + 1, 4);

        connect(row.on, &QCheckBox::toggled, this, [this, i](bool on) {
            editBand(i, on ? tr("Enable EQ Band") : tr("Disable EQ Band"),
                [on](EqBand& b) { b.enabled = on; });
        });
        connect(row.type, &QComboBox::activated, this, [this, i](int index) {
            const auto type = EqBandType(m_rows[size_t(i)].type->itemData(index).toInt());
            editBand(i, tr("EQ Band Type"), [type](EqBand& b) { b.type = type; });
        });
        connect(row.frequency, &QDoubleSpinBox::valueChanged, this, [this, i](double v) {
            editBand(i, tr("EQ Frequency"), [v](EqBand& b) { b.frequencyHz = v; });
        });
        connect(row.gain, &QDoubleSpinBox::valueChanged, this,
            [this, i](double v) { editBand(i, tr("EQ Gain"), [v](EqBand& b) { b.gainDb = v; }); });
        connect(row.q, &QDoubleSpinBox::valueChanged, this,
            [this, i](double v) { editBand(i, tr("EQ Q"), [v](EqBand& b) { b.q = v; }); });
    }
    layout->addLayout(grid);

    connect(m_enabled, &QCheckBox::toggled, this, [this](bool on) {
        m_controller->edit(on ? tr("Enable EQ") : tr("Bypass EQ"), [&](TimelineData& d) {
            ChannelStrip* s = mixerops::strip(d, m_target);
            if (!s || s->eq.enabled == on)
                return false;
            s->eq.enabled = on;
            return true;
        });
    });
    // Handle drags: one undo step per drag; dragging a band also switches the EQ on.
    connect(m_curve, &EqCurveView::bandPressed, this,
        [this](int) { m_controller->beginGesture(tr("EQ Band")); });
    connect(m_curve, &EqCurveView::bandDragged, this, [this](int band, double hz, double db) {
        m_controller->updateGesture([&](TimelineData& d) {
            ChannelStrip* s = mixerops::strip(d, m_target);
            if (!s)
                return false;
            EqBand& b = s->eq.bands[size_t(band)];
            b.frequencyHz = hz;
            if (eqBandHasGain(b.type))
                b.gainDb = db;
            b.enabled = true;
            s->eq.enabled = true;
            return true;
        });
    });
    connect(m_curve, &EqCurveView::bandReleased, this, [this](int) { m_controller->endGesture(); });
    connect(m_curve, &EqCurveView::bandQDragged, this, [this](int band, double q) {
        m_controller->updateGesture([&](TimelineData& d) {
            ChannelStrip* s = mixerops::strip(d, m_target);
            if (!s)
                return false;
            s->eq.bands[size_t(band)].q = q;
            return true;
        });
    });
    connect(m_curve, &EqCurveView::bandQRequested, this,
        [this](int band, double q) { editBand(band, tr("EQ Q"), [q](EqBand& b) { b.q = q; }); });
    connect(m_curve, &EqCurveView::bandSelected, this, [this](int band) {
        if (band >= 0)
            m_rows[size_t(band)].frequency->setFocus();
    });

    connect(m_controller, &MixerController::mixChanged, this, &EqWindow::refresh);
    refresh();
}

void EqWindow::editBand(int band, const QString& label, const std::function<void(EqBand&)>& change)
{
    m_controller->edit(label, [&](TimelineData& d) {
        ChannelStrip* s = mixerops::strip(d, m_target);
        if (!s)
            return false;
        const EqSettings before = s->eq;
        change(s->eq.bands[size_t(band)]);
        mixerops::clampStrip(*s);
        return !(s->eq == before);
    });
}

void EqWindow::refresh()
{
    const TimelineData& d = m_controller->data();
    const ChannelStrip* s = mixerops::strip(d, m_target);
    if (!s) {
        close(); // the track / bus is gone
        return;
    }
    setWindowTitle(tr("EQ - %1").arg(mixerops::targetName(d, m_target)));
    const QSignalBlocker blockEnabled(m_enabled);
    m_enabled->setChecked(s->eq.enabled);
    m_curve->setSettings(s->eq);
    for (int i = 0; i < kEqBandCount; ++i) {
        const EqBand& b = s->eq.bands[size_t(i)];
        BandRow& row = m_rows[size_t(i)];
        const QSignalBlocker b1(row.on), b2(row.type), b3(row.frequency), b4(row.gain), b5(row.q);
        row.on->setChecked(b.enabled);
        row.type->setCurrentIndex(row.type->findData(int(b.type)));
        // A field being typed in keeps its text (the mix changes during playback too).
        auto setIdle = [](QDoubleSpinBox* box, double v) {
            if (!box->hasFocus() || box->value() == v)
                box->setValue(v);
        };
        setIdle(row.frequency, b.frequencyHz);
        setIdle(row.gain, b.gainDb);
        row.gain->setEnabled(eqBandHasGain(b.type));
        setIdle(row.q, b.q);
    }
}

} // namespace kk
