// SPDX-License-Identifier: GPL-3.0-or-later
// NormalizeAudioDialog implementation.
#include "ui/dialogs/NormalizeAudioDialog.h"

#include "core/EditOps.h"
#include "core/LevelOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace kk {

namespace ops = editops;

namespace {

constexpr int kPollMs = 50; ///< progress bar refresh while measuring
constexpr double kMinTarget = -60.0; ///< lowest target level offered (dB / LUFS)
constexpr double kMaxTarget = 0.0;

} // namespace

QVector<quint64> normalizeTargets(const Project& project, const SelectionModel& selection)
{
    const TimelineData& d = project.timeline()->data();
    QVector<quint64> out;
    auto consider = [&](quint64 id) {
        const auto loc = ops::findClip(d, id);
        if (!loc || loc->track.type != TrackType::Audio || d.track(loc->track)->locked)
            return;
        const ClipData& c = d.track(loc->track)->clips[size_t(loc->clipIndex)];
        if (c.kind == ClipKind::Media && !out.contains(id))
            out.append(id);
    };
    for (const quint64 id : selection.clips()) {
        consider(id);
        for (const quint64 linked : ops::linkedClips(d, id))
            consider(linked); // a selected video clip brings its audio
    }
    // Timeline order, so the measurements and the Edit Index read the same way.
    std::sort(out.begin(), out.end(), [&d](quint64 a, quint64 b) {
        const ClipData* ca = ops::clipById(d, a);
        const ClipData* cb = ops::clipById(d, b);
        return ca->start != cb->start ? ca->start < cb->start : a < b;
    });
    return out;
}

NormalizeAudioDialog::NormalizeAudioDialog(
    Project* project, QVector<quint64> clipIds, QWidget* parent)
    : QDialog(parent)
    , m_project(project)
    , m_clips(std::move(clipIds))
{
    setObjectName(QStringLiteral("NormalizeAudioDialog"));
    setWindowTitle(tr("Normalize Audio Levels"));

    // The sources are copied now: the worker never touches the project.
    const TimelineData& d = project->timeline()->data();
    for (const quint64 id : std::as_const(m_clips)) {
        audionorm::Source s;
        if (const ClipData* c = ops::clipById(d, id)) {
            if (const MediaItem* item = project->mediaPool()->item(c->mediaId))
                s.path = item->path;
            const int64_t a = c->sourceTimeUsAt(c->start, d.fps);
            const int64_t b = c->sourceTimeUsAt(c->end(), d.fps);
            s.startUs = std::max<int64_t>(0, std::min(a, b)); // reverse clips run backwards
            s.endUs = std::max(a, b);
            s.attributes = c->attributes;
        }
        m_sources.push_back(s);
    }

    m_mode = new QComboBox;
    m_mode->setObjectName(QStringLiteral("NormalizeMode"));
    m_mode->addItem(tr("Sample Peak Program"), int(audionorm::Mode::SamplePeak));
    m_mode->addItem(tr("True Peak"), int(audionorm::Mode::TruePeak));
    m_mode->addItem(tr("ITU-R BS.1770-4"), int(audionorm::Mode::Loudness));
    m_target = new QDoubleSpinBox;
    m_target->setObjectName(QStringLiteral("NormalizeTarget"));
    m_target->setRange(kMinTarget, kMaxTarget);
    m_target->setDecimals(1);
    m_target->setSingleStep(0.5);
    m_relative = new QRadioButton(tr("Relative"));
    m_relative->setToolTip(tr("One gain for every clip: the loudest one reaches the target"));
    m_independent = new QRadioButton(tr("Independent"));
    m_independent->setToolTip(tr("Every clip reaches the target on its own"));
    m_independent->setChecked(true);
    auto* scopeGroup = new QButtonGroup(this);
    scopeGroup->addButton(m_relative);
    scopeGroup->addButton(m_independent);
    auto* scopeRow = new QHBoxLayout;
    scopeRow->addWidget(m_relative);
    scopeRow->addWidget(m_independent);
    scopeRow->addStretch(1);
    const bool several = m_clips.size() > 1;
    m_relative->setEnabled(several);
    m_independent->setEnabled(several);

    auto* form = new QFormLayout;
    form->addRow(tr("Normalization Mode"), m_mode);
    form->addRow(tr("Target Level"), m_target);
    form->addRow(tr("Set Level"), scopeRow);

    m_progress = new QProgressBar;
    m_progress->setObjectName(QStringLiteral("NormalizeProgress"));
    m_progress->setRange(0, 1000);
    m_progress->setTextVisible(false);
    m_progress->setVisible(false);
    m_status = new QLabel(m_clips.size() == 1 ? tr("1 audio clip")
                                              : tr("%n audio clips", nullptr, int(m_clips.size())));
    m_status->setObjectName(QStringLiteral("NormalizeStatus"));

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Normalize"));
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(!m_clips.isEmpty());
    connect(m_buttons, &QDialogButtonBox::accepted, this, &NormalizeAudioDialog::start);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &NormalizeAudioDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_status);
    layout->addWidget(m_progress);
    layout->addWidget(m_buttons);

    m_poll = new QTimer(this);
    m_poll->setInterval(kPollMs);
    connect(m_poll, &QTimer::timeout, this, &NormalizeAudioDialog::pollProgress);
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this] {
        updateUnit();
        m_target->setValue(audionorm::defaultTarget(mode()));
    });
    updateUnit();
    m_target->setValue(audionorm::defaultTarget(mode()));
}

NormalizeAudioDialog::~NormalizeAudioDialog()
{
    if (m_shared)
        m_shared->cancel = true;
    if (m_future.isValid())
        m_future.waitForFinished();
}

void NormalizeAudioDialog::setMode(audionorm::Mode mode)
{
    m_mode->setCurrentIndex(m_mode->findData(int(mode)));
}

audionorm::Mode NormalizeAudioDialog::mode() const
{
    return audionorm::Mode(m_mode->currentData().toInt());
}

void NormalizeAudioDialog::setTarget(double level)
{
    m_target->setValue(level);
}

double NormalizeAudioDialog::target() const
{
    return m_target->value();
}

void NormalizeAudioDialog::setScope(audionorm::Scope scope)
{
    (scope == audionorm::Scope::Relative ? m_relative : m_independent)->setChecked(true);
}

audionorm::Scope NormalizeAudioDialog::scope() const
{
    return m_relative->isChecked() ? audionorm::Scope::Relative : audionorm::Scope::Independent;
}

void NormalizeAudioDialog::updateUnit()
{
    switch (mode()) {
    case audionorm::Mode::SamplePeak:
        m_target->setSuffix(tr(" dBFS"));
        break;
    case audionorm::Mode::TruePeak:
        m_target->setSuffix(tr(" dBTP"));
        break;
    case audionorm::Mode::Loudness:
        m_target->setSuffix(tr(" LUFS"));
        break;
    }
}

bool NormalizeAudioDialog::isRunning() const
{
    return m_future.isValid() && !m_future.isFinished();
}

void NormalizeAudioDialog::start()
{
    if (isRunning() || m_clips.isEmpty())
        return;
    for (QWidget* w : {static_cast<QWidget*>(m_mode), static_cast<QWidget*>(m_target),
             static_cast<QWidget*>(m_relative), static_cast<QWidget*>(m_independent)})
        w->setEnabled(false);
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    m_progress->setValue(0);
    m_progress->setVisible(true);
    m_status->setText(tr("Measuring…"));
    m_shared = std::make_shared<Shared>();
    // The worker owns copies of the sources and the shared flags; nothing else.
    m_future = QtConcurrent::run([sources = m_sources, shared = m_shared] {
        std::vector<audionorm::Measurement> out;
        const double count = double(std::max<size_t>(1, sources.size()));
        for (size_t i = 0; i < sources.size(); ++i) {
            if (shared->cancel)
                break;
            out.push_back(audionorm::measure(sources[i], &shared->cancel,
                [&](double f) { shared->permille = int(1000.0 * (double(i) + f) / count); }));
        }
        return out;
    });
    m_poll->start();
}

void NormalizeAudioDialog::pollProgress()
{
    if (m_shared)
        m_progress->setValue(m_shared->permille);
    if (m_future.isValid() && m_future.isFinished()) {
        m_poll->stop();
        finishAnalysis();
    }
}

void NormalizeAudioDialog::finishAnalysis()
{
    m_results = m_future.result();
    m_future = {};
    if (m_shared && m_shared->cancel)
        return; // reject() already closed the dialog
    std::vector<double> levels;
    for (size_t i = 0; i < size_t(m_clips.size()); ++i) {
        const bool ok = i < m_results.size() && m_results[i].valid;
        levels.push_back(ok ? audionorm::levelOf(m_results[i], mode()) : -200.0);
    }
    const std::vector<double> gains = audionorm::gains(levels, target(), scope());
    m_project->beginEdit(tr("Normalize Audio Levels"));
    TimelineData& data = m_project->timeline()->mutableData();
    for (size_t i = 0; i < gains.size(); ++i) {
        if (std::isfinite(gains[i]))
            levelops::setClipGain(data, m_clips[qsizetype(i)], gains[i]);
    }
    m_project->endEdit();
    QDialog::accept();
}

void NormalizeAudioDialog::reject()
{
    // Also a finished analysis whose result was not collected yet: it must not apply.
    if (m_future.isValid()) {
        m_shared->cancel = true;
        m_poll->stop();
        m_future.waitForFinished(); // at most one 100 ms block of decoding
        m_future = {};
    }
    QDialog::reject();
}

} // namespace kk
