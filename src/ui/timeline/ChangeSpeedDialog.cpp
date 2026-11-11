// SPDX-License-Identifier: GPL-3.0-or-later
// ChangeSpeedDialog implementation.
#include "ui/timeline/ChangeSpeedDialog.h"

#include "core/Retime.h"
#include "ui/common/DialogSizing.h"
#include "ui/common/Metrics.h"
#include "ui/common/TimecodeFormat.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

constexpr double kMinPercent = 1.0; ///< like the usual speed dialogs: 1 % .. 100000 %
constexpr double kMaxPercent = 100000.0;

} // namespace

ChangeSpeedDialog::ChangeSpeedDialog(
    const ClipData& clip, Rational fps, Rational sourceFps, QWidget* parent)
    : QDialog(parent)
    , m_timelineFps(fps)
{
    setWindowTitle(tr("Change Clip Speed"));
    m_sourceFps = sourceFps.isValid() ? sourceFps.toDouble() : fps.toDouble();
    // The material the clip covers (retime frames) and its speed, as the Inspector shows them.
    const retimeops::SpeedState state = retimeops::speedState(clip);
    m_material = state.material;
    m_value = state.speed;

    m_speed = new QDoubleSpinBox;
    m_speed->setObjectName(QStringLiteral("SpeedPercent"));
    m_speed->setRange(kMinPercent, kMaxPercent);
    m_speed->setDecimals(2);
    m_speed->setSuffix(QStringLiteral(" %"));
    m_fps = new QDoubleSpinBox;
    m_fps->setObjectName(QStringLiteral("SpeedFps"));
    m_fps->setRange(m_sourceFps * kMinPercent / 100.0, m_sourceFps * kMaxPercent / 100.0);
    m_fps->setDecimals(3);
    m_duration = new QLineEdit;
    m_duration->setObjectName(QStringLiteral("SpeedDuration"));

    m_reverse = new QCheckBox(tr("Reverse speed"));
    m_reverse->setObjectName(QStringLiteral("SpeedReverse"));
    m_reverse->setChecked(state.reverse);
    m_freeze = new QCheckBox(tr("Freeze frame"));
    m_freeze->setObjectName(QStringLiteral("SpeedFreeze"));
    m_freeze->setChecked(state.freeze);
    m_ripple = new QCheckBox(tr("Ripple sequence"));
    m_ripple->setObjectName(QStringLiteral("SpeedRipple"));
    m_ripple->setChecked(true);
    m_pitch = new QCheckBox(tr("Pitch correction"));
    m_pitch->setObjectName(QStringLiteral("SpeedPitch"));
    m_pitch->setChecked(clip.pitchCorrection);
    m_pitch->setToolTip(tr("Keep the pitch of the clip's audio when it plays faster or slower."));

    auto* form = new QFormLayout;
    form->setHorizontalSpacing(metrics::kSpaceL);
    form->setVerticalSpacing(metrics::kSpaceM);
    form->addRow(tr("Speed"), m_speed);
    form->addRow(tr("Frames per second"), m_fps);
    form->addRow(tr("Duration"), m_duration);
    auto* options = new QVBoxLayout;
    options->setSpacing(metrics::kSpaceS);
    for (QCheckBox* box : {m_reverse, m_freeze, m_ripple, m_pitch})
        options->addWidget(box);
    form->addRow(QString(), options);
    if (state.curve && state.segments > 1) {
        auto* note = new QLineEdit; // read-only hint: the curve is replaced
        note->setReadOnly(true);
        note->setFrame(false);
        note->setText(
            tr("Replaces the %n speed segments with one speed.", nullptr, state.segments));
        form->addRow(QString(), note);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Change"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);

    connect(m_speed, &QDoubleSpinBox::valueChanged, this,
        [this](double percent) { setSpeed(percent / 100.0, m_speed); });
    connect(m_fps, &QDoubleSpinBox::valueChanged, this,
        [this](double value) { setSpeed(value / m_sourceFps, m_fps); });
    connect(m_duration, &QLineEdit::editingFinished, this, [this] {
        const auto frames = timecode::parse(m_duration->text(), m_timelineFps);
        if (frames && *frames > 0)
            setSpeed(m_material / double(*frames), m_duration);
        else
            setSpeed(m_value, nullptr); // restore the text
    });
    connect(m_freeze, &QCheckBox::toggled, this, &ChangeSpeedDialog::updateEnabled);
    setSpeed(m_value, nullptr);
    updateEnabled();
    dialogsizing::fit(this);
}

void ChangeSpeedDialog::setSpeed(double speed, QObject* from)
{
    if (m_syncing)
        return;
    m_value = std::clamp(speed, kMinPercent / 100.0, kMaxPercent / 100.0);
    m_syncing = true;
    if (from != m_speed)
        m_speed->setValue(m_value * 100.0);
    if (from != m_fps)
        m_fps->setValue(m_value * m_sourceFps);
    if (from != m_duration)
        m_duration->setText(timecode::format(
            std::max<int64_t>(1, std::llround(m_material / m_value)), m_timelineFps));
    m_syncing = false;
}

void ChangeSpeedDialog::updateEnabled()
{
    const bool amounts = !m_freeze->isChecked();
    for (QWidget* w : std::initializer_list<QWidget*> {m_speed, m_fps, m_duration, m_reverse})
        w->setEnabled(amounts);
}

retimeops::SpeedChange ChangeSpeedDialog::result() const
{
    retimeops::SpeedChange c;
    c.speed = m_value;
    c.reverse = m_reverse->isChecked();
    c.freeze = m_freeze->isChecked();
    c.ripple = m_ripple->isChecked();
    c.pitchCorrection = m_pitch->isChecked();
    return c;
}

} // namespace kk
