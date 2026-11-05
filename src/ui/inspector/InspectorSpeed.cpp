// SPDX-License-Identifier: GPL-3.0-or-later
// InspectorSpeed — the Video tab's Dynamic Zoom, Speed Change and Retime and Scaling sections
// (spec 24.20, 24.21). Part of InspectorPanel. The speed fields drive the same edit as the
// Change Clip Speed dialog (retimeops::changeClipSpeed) and show retimeops::speedState().
#include "core/DynamicZoom.h"
#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/RetimeOps.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/inspector/InspectorPanel.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/inspector/ParamRows.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

namespace rops = retimeops;

constexpr double kMinPercent = 1.0; ///< the Change Clip Speed dialog's range
constexpr double kMaxPercent = 100000.0;

void setIfIdle(QDoubleSpinBox* box, double value)
{
    if (box->hasFocus() || std::abs(box->value() - value) < 1e-9)
        return;
    const QSignalBlocker block(box);
    box->setValue(value);
}

} // namespace

void InspectorPanel::buildDynamicZoomSection(quint64 clipId)
{
    const EffectDescriptor* desc
        = EffectRegistry::instance().find(QLatin1String(groups::DynamicZoom));
    if (!desc)
        return;
    ParamGroupBox* group = addGroup(VideoTab, desc->id, desc->name, clipId);
    group->setObjectName(QStringLiteral("DynamicZoomGroup"));
    QCheckBox* enable = group->addEnableToggle();
    enable->setObjectName(QStringLiteral("DynamicZoomEnable"));
    attachHelp(group, desc->id, desc->name);
    connect(enable, &QCheckBox::clicked, this, [this, clipId](bool on) {
        editClip(on ? tr("Enable Dynamic Zoom") : tr("Disable Dynamic Zoom"), clipId,
            [on](ClipData& c) { c.transform.setStatic(keys::DynamicZoom, on); });
    });
    for (const ParamDescriptor& p : desc->params)
        group->addParam(ParamRef {clipId, ParamScope::Transform, 0, p.key}, p, *this);
    auto* swap = new QPushButton(tr("Swap"));
    swap->setObjectName(QStringLiteral("DynamicZoomSwap"));
    swap->setToolTip(tr("Swap the start and end rectangles (a push-in becomes a pull-out)"));
    group->addWidgetRow(QString(), swap);
    connect(swap, &QPushButton::clicked, this, [this, clipId] {
        editClip(
            tr("Swap Dynamic Zoom"), clipId, [](ClipData& c) { dynamiczoom::swap(c.transform); });
    });
    QToolButton* reset = group->addHeaderButton(InspectorGlyph::Reset, tr("Reset all"));
    connect(reset, &QToolButton::clicked, this, [this, clipId, name = desc->name] {
        editClip(
            tr("Reset %1").arg(name), clipId, [](ClipData& c) { dynamiczoom::reset(c.transform); });
    });
    group->setHeaderRefresh([enable, swap](const ClipData& c) {
        const bool on = dynamiczoom::isEnabled(c.transform);
        if (enable->isChecked() != on)
            enable->setChecked(on);
        swap->setEnabled(on);
    });
}

void InspectorPanel::applySpeed(quint64 clipId, const std::function<void(rops::SpeedChange&)>& edit)
{
    const ClipData* shown = clip(clipId);
    if (!shown)
        return;
    const rops::SpeedState state = rops::speedState(*shown);
    rops::SpeedChange change;
    change.speed = state.speed;
    change.reverse = state.reverse;
    change.freeze = state.freeze;
    change.freezeFrame = shown->contains(m_ctx.project->timeline()->playhead())
        ? m_ctx.project->timeline()->playhead()
        : -1;
    change.ripple = m_speedRipple;
    change.pitchCorrection = shown->pitchCorrection;
    edit(change);
    QVector<quint64> ids {clipId};
    ids += editPeers(clipId);
    Project* project = m_ctx.project;
    const QVector<quint64> targets = rops::primaries(project->timeline()->data(), ids);
    project->beginEdit(tr("Change Clip Speed"));
    project->markEditKeepsPlaying();
    bool any = false;
    for (quint64 id : targets)
        any |= rops::changeClipSpeed(project->timeline()->mutableData(), id, change);
    if (any)
        project->endEdit();
    else
        project->cancelEdit();
}

void InspectorPanel::buildSpeedSections(const ClipData& c)
{
    const quint64 clipId = m_clipId;
    const TimelineData& data = m_ctx.project->timeline()->data();
    const Rational fps = data.fps;
    double sourceFps = fps.toDouble();
    if (const MediaItem* item = m_ctx.project->mediaPool()->item(c.mediaId)) {
        if (item->info.frameRate.isValid())
            sourceFps = item->info.frameRate.toDouble();
    }

    // ---- Speed Change ----
    ParamGroupBox* speed
        = addGroup(VideoTab, QStringLiteral("speed_change"), tr("Speed Change"), clipId);
    speed->setObjectName(QStringLiteral("SpeedChangeGroup"));
    auto* direction = new QComboBox;
    direction->setObjectName(QStringLiteral("SpeedDirection"));
    direction->addItem(tr("Forward"));
    direction->addItem(tr("Reverse"));
    speed->addWidgetRow(tr("Direction"), direction);
    auto* freeze = new QCheckBox(tr("Freeze Frame"));
    freeze->setObjectName(QStringLiteral("SpeedFreeze"));
    speed->addWidgetRow(QString(), freeze);
    auto* percent = new QDoubleSpinBox;
    percent->setObjectName(QStringLiteral("SpeedPercent"));
    percent->setRange(kMinPercent, kMaxPercent);
    percent->setDecimals(2);
    percent->setSuffix(QStringLiteral(" %"));
    percent->setKeyboardTracking(false); // one edit per Enter / focus change, not per digit
    speed->addWidgetRow(tr("Speed"), percent);
    auto* perSecond = new QDoubleSpinBox;
    perSecond->setObjectName(QStringLiteral("SpeedFps"));
    perSecond->setRange(sourceFps * kMinPercent / 100.0, sourceFps * kMaxPercent / 100.0);
    perSecond->setDecimals(3);
    perSecond->setKeyboardTracking(false);
    speed->addWidgetRow(tr("Frames per second"), perSecond);
    auto* duration = new QLineEdit;
    duration->setObjectName(QStringLiteral("SpeedDuration"));
    speed->addWidgetRow(tr("Duration"), duration);
    auto* ripple = new QCheckBox(tr("Ripple Sequence"));
    ripple->setObjectName(QStringLiteral("SpeedRipple"));
    ripple->setChecked(m_speedRipple);
    ripple->setToolTip(tr("Later clips move when the speed changes the clip's length"));
    speed->addWidgetRow(QString(), ripple);
    auto* pitch = new QCheckBox(tr("Pitch Correction"));
    pitch->setObjectName(QStringLiteral("SpeedPitch"));
    speed->addWidgetRow(QString(), pitch);

    connect(ripple, &QCheckBox::toggled, this, [this](bool on) { m_speedRipple = on; });
    connect(direction, &QComboBox::activated, this, [this, clipId](int index) {
        applySpeed(clipId, [index](rops::SpeedChange& ch) {
            ch.reverse = index == 1;
            ch.freeze = false;
        });
    });
    connect(freeze, &QCheckBox::clicked, this, [this, clipId](bool on) {
        applySpeed(clipId, [on](rops::SpeedChange& ch) { ch.freeze = on; });
    });
    connect(percent, &QDoubleSpinBox::valueChanged, this, [this, clipId](double value) {
        applySpeed(clipId, [value](rops::SpeedChange& ch) {
            ch.speed = value / 100.0;
            ch.freeze = false;
        });
    });
    connect(
        perSecond, &QDoubleSpinBox::valueChanged, this, [this, clipId, sourceFps](double value) {
            applySpeed(clipId, [value, sourceFps](rops::SpeedChange& ch) {
                ch.speed = value / sourceFps;
                ch.freeze = false;
            });
        });
    connect(duration, &QLineEdit::editingFinished, this, [this, clipId, duration, fps] {
        const ClipData* shown = clip(clipId);
        const auto frames = timecode::parse(duration->text(), fps);
        if (!shown || !frames || *frames <= 0 || *frames == shown->duration) {
            refreshValues(); // restore the text
            return;
        }
        const double material = rops::speedState(*shown).material;
        applySpeed(clipId, [material, f = *frames](rops::SpeedChange& ch) {
            ch.speed = std::clamp(material / double(f), kMinPercent / 100.0, kMaxPercent / 100.0);
            ch.freeze = false;
            ch.ripple = true; // the typed duration is the point of the edit
        });
    });
    connect(pitch, &QCheckBox::clicked, this, [this, clipId](bool on) {
        Project* project = m_ctx.project;
        project->beginEdit(tr("Pitch Correction"));
        project->markEditKeepsPlaying();
        if (rops::setPitchCorrection(project->timeline()->mutableData(), clipId, on))
            project->endEdit();
        else
            project->cancelEdit();
    });
    speed->setHeaderRefresh([direction, freeze, percent, perSecond, duration, pitch, sourceFps,
                                fps](const ClipData& cd) {
        const rops::SpeedState s = rops::speedState(cd);
        if (direction->currentIndex() != (s.reverse ? 1 : 0)) {
            const QSignalBlocker block(direction);
            direction->setCurrentIndex(s.reverse ? 1 : 0);
        }
        if (freeze->isChecked() != s.freeze)
            freeze->setChecked(s.freeze);
        setIfIdle(percent, s.speed * 100.0);
        setIfIdle(perSecond, s.speed * sourceFps);
        if (!duration->hasFocus()) {
            const QString text = timecode::format(cd.duration, fps);
            if (duration->text() != text)
                duration->setText(text);
        }
        for (QWidget* w : std::initializer_list<QWidget*> {direction, percent, perSecond})
            w->setEnabled(!s.freeze);
        if (pitch->isChecked() != cd.pitchCorrection)
            pitch->setChecked(cd.pitchCorrection);
    });

    // ---- Retime and Scaling ----
    ParamGroupBox* retime
        = addGroup(VideoTab, QStringLiteral("retime_scaling"), tr("Retime and Scaling"), clipId);
    retime->setObjectName(QStringLiteral("RetimeScalingGroup"));
    auto* process = new QComboBox;
    process->setObjectName(QStringLiteral("RetimeProcess"));
    const std::pair<RetimeProcess, QString> processes[] = {
        {RetimeProcess::ProjectDefault, tr("Project Settings")},
        {RetimeProcess::Nearest, tr("Nearest")},
        {RetimeProcess::FrameBlend, tr("Frame Blend")},
        {RetimeProcess::OpticalFlow, tr("Optical Flow")},
    };
    for (const auto& [value, text] : processes)
        process->addItem(text, int(value));
    retime->addWidgetRow(tr("Retime Process"), process);
    connect(process, &QComboBox::activated, this, [this, clipId, process](int index) {
        const auto value = RetimeProcess(process->itemData(index).toInt());
        Project* project = m_ctx.project;
        project->beginEdit(tr("Retime Process"));
        project->markEditKeepsPlaying();
        bool any = false;
        QVector<quint64> ids {clipId};
        ids += editPeers(clipId);
        for (quint64 id : rops::primaries(project->timeline()->data(), ids))
            any |= rops::setProcess(project->timeline()->mutableData(), id, value);
        if (any)
            project->endEdit();
        else
            project->cancelEdit();
    });
    retime->setHeaderRefresh([process](const ClipData& cd) {
        const int index = process->findData(int(cd.retimeProcess));
        if (index >= 0 && process->currentIndex() != index) {
            const QSignalBlocker block(process);
            process->setCurrentIndex(index);
        }
    });
}

} // namespace kk
